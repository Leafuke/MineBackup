static bool ValidateRestoreArchives(const vector<filesystem::path>& archives, const Config& config, Console& console) {
	console.AddLog(L("LOG_VERIFYING_BACKUPS"));
	for (const auto& backup : archives) {
		wstring testCommand = L"\"" + config.zipPath + L"\" t \"" + backup.wstring() + L"\" -y";
		if (!RunCommandInBackground(testCommand, console, config.useLowPriority)) {
			console.AddLog(L("ERROR_BACKUP_CORRUPTED"), wstring_to_utf8(backup.filename().wstring()).c_str());
			return false;
		}
	}
	console.AddLog(L("LOG_BACKUP_VERIFICATION_PASSED"));
	return true;
}

static bool ApplyRestoreChain(const vector<filesystem::path>& backupsToApply, const filesystem::path& destinationFolder, const Config& config, Console& console, const wstring& filesToExtractStr = L"") {
	for (size_t i = 0; i < backupsToApply.size(); ++i) {
		const auto& backup = backupsToApply[i];
		console.AddLog(L("RESTORE_STEPS"), i + 1, backupsToApply.size(), wstring_to_utf8(backup.filename().wstring()).c_str());
		wstring command = L"\"" + config.zipPath + L"\" x \"" + backup.wstring() + L"\" -o\"" + destinationFolder.wstring() + L"\" -y" + filesToExtractStr;
		if (!RunCommandInBackground(command, console, config.useLowPriority)) {
			return false;
		}
	}
	return true;
}

static RestoreChainResult BuildMetadataRestoreChain(const filesystem::path& metadataDir, const filesystem::path& backupDir, const filesystem::path& targetBackupPath) {
	RestoreChainResult result;
	BackupMetadataSummary summary;
	if (!LoadBackupMetadataSummary(metadataDir, summary) || summary.records.empty()) {
		result.status = RestoreChainStatus::METADATA_UNAVAILABLE;
		return result;
	}

	map<wstring, BackupMetadataRecordIndex> recordMap;
	for (const auto& record : summary.records) {
		if (!record.archiveFileName.empty()) {
			recordMap[record.archiveFileName] = record;
		}
	}

	set<wstring> visited;
	wstring current = targetBackupPath.filename().wstring();
	while (!current.empty()) {
		if (!visited.insert(current).second) {
			result.status = RestoreChainStatus::INVALID;
			result.chain.clear();
			return result;
		}

		auto recordIt = recordMap.find(current);
		if (recordIt == recordMap.end()) {
			result.status = RestoreChainStatus::METADATA_UNAVAILABLE;
			result.chain.clear();
			return result;
		}

		filesystem::path currentArchive = backupDir / current;
		if (!filesystem::exists(currentArchive)) {
			result.status = RestoreChainStatus::INVALID;
			result.chain.clear();
			return result;
		}

		result.chain.push_back(currentArchive);
		const auto& record = recordIt->second;
		wstring recordType = record.backupType.empty() ? current : record.backupType;
		if (!IsIncrementalBackupType(recordType)) {
			break;
		}

		if (record.previousBackupFileName.empty()) {
			result.status = RestoreChainStatus::MISSING_BASE_FULL;
			result.chain.clear();
			return result;
		}
		current = record.previousBackupFileName;
	}

	reverse(result.chain.begin(), result.chain.end());
	if (result.chain.empty()) {
		result.status = RestoreChainStatus::INVALID;
		return result;
	}

	const wstring firstName = result.chain.front().filename().wstring();
	auto firstRecordIt = recordMap.find(firstName);
	const wstring firstType = firstRecordIt == recordMap.end() ? firstName : firstRecordIt->second.backupType;
	if (!IsFullLikeBackupType(firstType)) {
		result.chain.clear();
		result.status = RestoreChainStatus::MISSING_BASE_FULL;
		return result;
	}

	result.status = RestoreChainStatus::OK;
	result.usedMetadata = true;
	return result;
}

static vector<filesystem::path> BuildLegacyForwardRestoreChain(const filesystem::path& backupDir, const filesystem::path& targetBackupPath) {
	vector<filesystem::path> backupsToApply;
	const auto targetTime = filesystem::last_write_time(targetBackupPath);

	if (targetBackupPath.filename().wstring().find(L"[Smart]") != wstring::npos) {
		filesystem::path baseFullBackup;
		auto baseFullTime = filesystem::file_time_type{};
		for (const auto& entry : filesystem::directory_iterator(backupDir)) {
			if (!entry.is_regular_file()) continue;
			if (entry.path().filename().wstring().find(L"[Full]") == wstring::npos) continue;
			auto entryTime = entry.last_write_time();
			if (entryTime < targetTime && entryTime > baseFullTime) {
				baseFullTime = entryTime;
				baseFullBackup = entry.path();
			}
		}
		if (baseFullBackup.empty()) {
			return {};
		}
		backupsToApply.push_back(baseFullBackup);
		for (const auto& entry : filesystem::directory_iterator(backupDir)) {
			if (!entry.is_regular_file()) continue;
			if (entry.path().filename().wstring().find(L"[Smart]") == wstring::npos) continue;
			auto entryTime = entry.last_write_time();
			if (entryTime > baseFullTime && entryTime <= targetTime) {
				backupsToApply.push_back(entry.path());
			}
		}
		sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
			return filesystem::last_write_time(a) < filesystem::last_write_time(b);
		});
		return backupsToApply;
	}

	backupsToApply.push_back(targetBackupPath);
	return backupsToApply;
}

static vector<filesystem::path> BuildReverseRestoreChain(const filesystem::path& backupDir, const filesystem::path& targetBackupPath) {
	vector<filesystem::path> backupsToApply;
	const auto targetTime = filesystem::last_write_time(targetBackupPath);
	for (const auto& entry : filesystem::directory_iterator(backupDir)) {
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() != targetBackupPath.extension()) continue;
		if (entry.last_write_time() >= targetTime) {
			backupsToApply.push_back(entry.path());
		}
	}
	sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
		return filesystem::last_write_time(a) > filesystem::last_write_time(b);
	});
	return backupsToApply;
}

static bool TryBuildSmartRestorePlan(const filesystem::path& metadataDir, const vector<filesystem::path>& chain, SmartRestorePlan& outPlan) {
	outPlan = SmartRestorePlan{};
	if (chain.empty()) return false;

	BackupChangeRecord baseRecord;
	if (!LoadBackupChangeRecord(metadataDir, chain.front().filename().wstring(), baseRecord) || baseRecord.fullFileList.empty()) {
		return false;
	}

	map<wstring, wstring> owners;
	for (const auto& file : baseRecord.fullFileList) {
		if (!file.empty()) owners[file] = chain.front().filename().wstring();
	}

	for (size_t i = 1; i < chain.size(); ++i) {
		BackupChangeRecord record;
		if (!LoadBackupChangeRecord(metadataDir, chain[i].filename().wstring(), record)) {
			return false;
		}

		for (const auto& deleted : record.deletedFiles) {
			owners.erase(deleted);
		}
		for (const auto& added : record.addedFiles) {
			owners[added] = record.archiveFileName;
		}
		for (const auto& modified : record.modifiedFiles) {
			owners[modified] = record.archiveFileName;
		}

		set<wstring> expected(record.fullFileList.begin(), record.fullFileList.end());
		if (owners.size() != expected.size()) {
			return false;
		}
		for (const auto& pair : owners) {
			if (!expected.count(pair.first)) {
				return false;
			}
		}
	}

	map<wstring, filesystem::path> archiveLookup;
	map<wstring, size_t> archiveOrder;
	for (size_t i = 0; i < chain.size(); ++i) {
		archiveLookup[chain[i].filename().wstring()] = chain[i];
		archiveOrder[chain[i].filename().wstring()] = i;
	}

	map<wstring, vector<wstring>> groupedFiles;
	for (const auto& pair : owners) {
		groupedFiles[pair.second].push_back(pair.first);
	}

	vector<SmartRestoreArchiveGroup> groups;
	for (auto& pair : groupedFiles) {
		auto archiveIt = archiveLookup.find(pair.first);
		if (archiveIt == archiveLookup.end()) continue;
		sort(pair.second.begin(), pair.second.end());
		groups.push_back({ archiveIt->second, pair.second });
	}
	sort(groups.begin(), groups.end(), [&](const SmartRestoreArchiveGroup& a, const SmartRestoreArchiveGroup& b) {
		return archiveOrder[a.archive.filename().wstring()] < archiveOrder[b.archive.filename().wstring()];
	});

	outPlan.chain = chain;
	outPlan.archiveGroups = std::move(groups);
	return true;
}

static bool ApplySmartRestorePlan(const SmartRestorePlan& plan, const filesystem::path& destinationFolder, const Config& config, Console& console) {
	vector<SmartRestoreArchiveGroup> groups;
	for (const auto& group : plan.archiveGroups) {
		if (!group.files.empty()) {
			groups.push_back(group);
		}
	}
	if (groups.empty()) {
		return true;
	}

	for (size_t i = 0; i < groups.size(); ++i) {
		const auto& group = groups[i];
		console.AddLog(L("RESTORE_STEPS"), i + 1, groups.size(), wstring_to_utf8(group.archive.filename().wstring()).c_str());

		wstringstream fileNameBuilder;
		fileNameBuilder << L"MineBackup_Restore_" << chrono::steady_clock::now().time_since_epoch().count() << L"_" << i << L".txt";
		filesystem::path listFile = filesystem::temp_directory_path() / fileNameBuilder.str();
		try {
			ofstream out(listFile, ios::binary | ios::trunc);
			for (const auto& file : group.files) {
				string utf8Path = wstring_to_utf8(file);
				out.write(utf8Path.data(), static_cast<std::streamsize>(utf8Path.size()));
				out.put('\n');
			}
			out.close();

			wstring command = L"\"" + config.zipPath + L"\" x \"" + group.archive.wstring() + L"\" @\"" + listFile.wstring() + L"\" -o\"" + destinationFolder.wstring() + L"\" -y";
			if (!RunCommandInBackground(command, console, config.useLowPriority)) {
				filesystem::remove(listFile);
				return false;
			}
		}
		catch (...) {
			filesystem::remove(listFile);
			return false;
		}
		filesystem::remove(listFile);
	}

	return true;
}

bool DoRestore2(const Config& config, const wstring& worldName, const filesystem::path& fullBackupPath, Console& console, int restoreMethod) {
	filesystem::path destinationFolder = JoinPath(config.saveRoot, worldName);
	WorldOperationGuard opGuard(destinationFolder, FolderState::RESTORE);
	if (!opGuard.Acquired()) {
		console.AddLog(
			L("LOG_OP_REJECTED_BUSY"),
			wstring_to_utf8(worldName).c_str(),
			L(FolderStateToI18nKey(opGuard.Existing())),
			L(FolderStateToI18nKey(opGuard.Requested()))
		);
		return false;
	}

	auto failRestore = [&](const string& reason) {
		BroadcastEvent("event=restore_failed;config=" + to_string(g_appState.currentConfigIndex) + ";world=" + wstring_to_utf8(worldName) + ";error=" + reason);
		return false;
	};

	console.AddLog(L("LOG_RESTORE_START_HEADER"));
	console.AddLog(L("LOG_RESTORE_PREPARE"), wstring_to_utf8(worldName).c_str());
	console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(fullBackupPath.wstring()).c_str());

	if (!filesystem::exists(config.zipPath)) {
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return failRestore("seven_zip_not_found");
	}

	vector<filesystem::path> backupsToApply = { fullBackupPath };
	if (!ValidateRestoreArchives(backupsToApply, config, console)) {
		return failRestore("archive_integrity_check_failed");
	}

	filesystem::path safeRestoreTempDir;
	string workspaceError;
	bool safeWorkspacePrepared = false;
	if (restoreMethod == 0) {
		if (!TryPrepareSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, workspaceError)) {
			if (!safeRestoreTempDir.empty()) {
				string rollbackError;
				if (!TryRollbackSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, rollbackError)) {
					console.AddLog("[Error] Failed to rollback after workspace prepare failure: %s", rollbackError.c_str());
				}
			}
			console.AddLog("[Error] Failed to prepare safe restore workspace: %s", workspaceError.c_str());
			return failRestore("snapshot_prepare_failed");
		}
		safeWorkspacePrepared = !safeRestoreTempDir.empty();
	}
	else {
		error_code ec;
		filesystem::create_directories(destinationFolder, ec);
	}

	bool restoreSucceeded = ApplyRestoreChain(backupsToApply, destinationFolder, config, console);
	if (restoreSucceeded) {
		CleanupInternalRestoreMarkers(destinationFolder);
		if (safeWorkspacePrepared) {
			const vector<wstring> effectiveRestoreWhitelist = BuildEffectiveRestoreWhitelist(restoreWhitelist);
			if (!TryCommitSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, effectiveRestoreWhitelist, workspaceError)) {
				restoreSucceeded = false;
				console.AddLog("[Error] Failed to commit safe restore workspace: %s", workspaceError.c_str());
			}
		}
	}

	if (!restoreSucceeded) {
		if (safeWorkspacePrepared) {
			if (!TryRollbackSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, workspaceError)) {
				console.AddLog("[Error] Failed to rollback safe restore workspace: %s", workspaceError.c_str());
			}
		}
		return failRestore("command_failed");
	}

	console.AddLog(L("LOG_RESTORE_END_HEADER"));
	BroadcastEvent("event=restore_success;config=" + to_string(g_appState.currentConfigIndex) + ";world=" + wstring_to_utf8(worldName) + ";backup=" + wstring_to_utf8(fullBackupPath.filename().wstring()));
	return true;
}

bool DoRestore(const Config& config, const wstring& worldName, const wstring& backupFile, Console& console, int restoreMethod, const string& customRestoreList) {
	filesystem::path destinationFolder = JoinPath(config.saveRoot, worldName);
	WorldOperationGuard opGuard(destinationFolder, FolderState::RESTORE);
	if (!opGuard.Acquired()) {
		console.AddLog(
			L("LOG_OP_REJECTED_BUSY"),
			wstring_to_utf8(worldName).c_str(),
			L(FolderStateToI18nKey(opGuard.Existing())),
			L(FolderStateToI18nKey(opGuard.Requested()))
		);
		return false;
	}

	auto failRestoreWithMessage = [&](const string& reason, const string& message) {
		if (!message.empty()) {
			console.AddLog("[Error] %s", message.c_str());
		}
		BroadcastEvent("event=restore_failed;config=" + to_string(g_appState.currentConfigIndex) + ";world=" + wstring_to_utf8(worldName) + ";error=" + reason);
		return false;
	};
	auto failRestore = [&](const string& reason) {
		return failRestoreWithMessage(reason, string{});
	};

	console.AddLog(L("LOG_RESTORE_START_HEADER"));
	console.AddLog(L("LOG_RESTORE_PREPARE"), wstring_to_utf8(worldName).c_str());
	console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(backupFile).c_str());

	if (!filesystem::exists(config.zipPath)) {
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return failRestore("seven_zip_not_found");
	}

	filesystem::path sourceDir = JoinPath(config.backupPath, worldName);
	filesystem::path targetBackupPath = sourceDir / backupFile;
	const int resolvedConfigIndex = ResolveConfigIndexForCloud(config);
	HistoryEntry targetHistoryEntry;
	const bool hasHistoryEntry = resolvedConfigIndex >= 0
		&& TryGetHistoryEntry(resolvedConfigIndex, worldName, backupFile, targetHistoryEntry);

	// 云存档补链发生在本地存在性校验之前：
	// 这样本地缺包、增量链缺失元数据时，都可以先尝试从云端补齐。
	if (hasHistoryEntry && config.cloudAutoDownloadBeforeRestore) {
		EnsureRestoreChainAvailable(config, resolvedConfigIndex, targetHistoryEntry, console);
	}

	if ((backupFile.find(L"[Smart]") == wstring::npos && backupFile.find(L"[Full]") == wstring::npos) || !filesystem::exists(targetBackupPath)) {
		console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(backupFile).c_str());
		return failRestore("backup_not_found");
	}

#ifdef _WIN32
	if (IsFileLocked(destinationFolder.wstring() + L"\\session.lock")) {
		int msgboxID = MessageBoxW(
			NULL,
			utf8_to_wstring(L("RESTORE_OVER_RUNNING_WORLD_MSG")).c_str(),
			utf8_to_wstring(L("RESTORE_OVER_RUNNING_WORLD_TITLE")).c_str(),
			MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2
		);
		if (msgboxID == IDNO) {
			console.AddLog("[Info] Restore cancelled by user due to active game session.");
			return failRestore("cancelled_active_world");
		}
	}
#endif

	const bool targetIsIncremental = IsIncrementalBackupType(backupFile);
	const filesystem::path metadataDir = GetMetadataDirectory(config, worldName);
	RestoreChainResult chainResult;
	vector<filesystem::path> backupsToApply;

	if (restoreMethod == 2) {
		backupsToApply = BuildReverseRestoreChain(sourceDir, targetBackupPath);
		if (backupsToApply.empty()) {
			console.AddLog(L("LOG_BACKUP_SMART_NO_FOUND"));
			return failRestore("reverse_chain_not_found");
		}
	}
	else if (targetIsIncremental) {
		chainResult = BuildMetadataRestoreChain(metadataDir, sourceDir, targetBackupPath);
		if (chainResult.status == RestoreChainStatus::OK) {
			backupsToApply = chainResult.chain;
		}
		else {
			if (restoreMethod == 0) {
				console.AddLog("[Error] Exact Clean Restore for Smart backups requires valid metadata and an intact full base.");
				return failRestore("exact_clean_restore_unavailable");
			}

			backupsToApply = BuildLegacyForwardRestoreChain(sourceDir, targetBackupPath);
			if (backupsToApply.empty()) {
				console.AddLog(L("LOG_BACKUP_SMART_NO_FOUND"));
				return failRestore("restore_chain_not_found");
			}
		}
	}
	else {
		backupsToApply.push_back(targetBackupPath);
	}

	wstring filesToExtractStr;
	if (restoreMethod == 3 && !customRestoreList.empty()) {
		console.AddLog(L("LOG_CUSTOM_RESTORE_START"));
		stringstream ss(customRestoreList);
		string item;
		while (getline(ss, item, ',')) {
			item.erase(0, item.find_first_not_of(" \t\n\r"));
			item.erase(item.find_last_not_of(" \t\n\r") + 1);
			if (!item.empty()) {
				filesToExtractStr += L" \"" + utf8_to_wstring(item) + L"\"";
			}
		}
	}

	if (!ValidateRestoreArchives(backupsToApply, config, console)) {
		return failRestore("archive_integrity_check_failed");
	}

	SmartRestorePlan smartRestorePlan;
	const bool useExactSmartCleanRestore = restoreMethod == 0 && targetIsIncremental && chainResult.status == RestoreChainStatus::OK && chainResult.usedMetadata;
	if (useExactSmartCleanRestore) {
		if (!TryBuildSmartRestorePlan(metadataDir, backupsToApply, smartRestorePlan)) {
			console.AddLog("[Error] Smart restore metadata is incomplete or inconsistent. Clean restore aborted to protect data.");
			return failRestore("smart_restore_plan_invalid");
		}
	}

	filesystem::path safeRestoreTempDir;
	string workspaceError;
	bool safeWorkspacePrepared = false;
	if (restoreMethod == 0) {
		if (!TryPrepareSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, workspaceError)) {
			if (!safeRestoreTempDir.empty()) {
				string rollbackError;
				if (!TryRollbackSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, rollbackError)) {
					console.AddLog("[Error] Failed to rollback after workspace prepare failure: %s", rollbackError.c_str());
				}
			}
			console.AddLog("[Error] Failed to prepare safe restore workspace: %s", workspaceError.c_str());
			return failRestore("snapshot_prepare_failed");
		}
		safeWorkspacePrepared = !safeRestoreTempDir.empty();
	}
	else {
		error_code ec;
		filesystem::create_directories(destinationFolder, ec);
	}

	bool restoreSucceeded = false;
	if (useExactSmartCleanRestore) {
		restoreSucceeded = ApplySmartRestorePlan(smartRestorePlan, destinationFolder, config, console);
	}
	else {
		restoreSucceeded = ApplyRestoreChain(backupsToApply, destinationFolder, config, console, filesToExtractStr);
	}

	if (restoreSucceeded) {
		CleanupInternalRestoreMarkers(destinationFolder);
		if (safeWorkspacePrepared) {
			const vector<wstring> effectiveRestoreWhitelist = BuildEffectiveRestoreWhitelist(restoreWhitelist);
			if (!TryCommitSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, effectiveRestoreWhitelist, workspaceError)) {
				restoreSucceeded = false;
				console.AddLog("[Error] Failed to commit safe restore workspace: %s", workspaceError.c_str());
			}
		}
	}

	if (!restoreSucceeded) {
		if (safeWorkspacePrepared) {
			if (!TryRollbackSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, workspaceError)) {
				console.AddLog("[Error] Failed to rollback safe restore workspace: %s", workspaceError.c_str());
			}
		}
		return failRestore("command_failed");
	}

	console.AddLog(L("LOG_RESTORE_END_HEADER"));
	BroadcastEvent("event=restore_success;config=" + to_string(g_appState.currentConfigIndex) + ";world=" + wstring_to_utf8(worldName) + ";backup=" + wstring_to_utf8(backupFile));
	return true;
}
