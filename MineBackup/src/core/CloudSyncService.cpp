
#include "CloudSyncService.h"
#include "CloudSyncInternal.h"
#include "AppState.h"

#include "ConfigManager.h"
#include "AppPaths.h"
#include "HistoryManager.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "FolderRewindMetadataStore.h"
#include "MigrationCoordinator.h"
#include "Logging.h"
#include "TaskCoordinator.h"
#include "ExternalToolManager.h"
#include "RcloneClient.h"
#include "i18n.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

using namespace std;

namespace CloudSyncInternal {
	mutex g_cloudMutex;

	vector<HistoryEntry> LoadRemoteHistoryEntriesNoLock(const Config& config, int configIndex, CloudCommandResult& outResult);
	bool BelongsToConfiguration(const Config& config, const HistoryEntry& entry);
	bool TryResolveKnownConfigId(const HistoryEntry& entry, wstring& outConfigId);

	bool IsLegacyHistoryJson(const nlohmann::json& root) {
		if (!root.is_array()) return false;
		return any_of(root.begin(), root.end(), [](const nlohmann::json& item) {
			return item.is_object() && (item.contains("backupFile") || item.contains("configIndex"));
		});
	}

	wstring NormalizeRemotePath(const wstring& value) {
		wstring result = value;
		for (wchar_t& ch : result) {
			if (ch == L'\\') ch = L'/';
		}
		while (!result.empty() && result.back() == L'/') {
			result.pop_back();
		}
		return result;
	}

	ExternalToolResolution ResolveRcloneExecutable(const Config& config) {
		return ExternalToolManager::ResolveRclone(
			config.rclonePath, GetAppPaths(), TaskCoordinator::CurrentStopToken());
	}

	bool IsIncrementalBackupType(const wstring& typeOrFileName) {
		return FolderRewindFormat::IsSmartBackupType(typeOrFileName);
	}

	bool IsFullLikeBackupType(const wstring& typeOrFileName) {
		return FolderRewindFormat::IsFullLikeBackupType(typeOrFileName);
	}

	void SetCloudRuntimeState(int configIndex, bool busy, int progress, const wstring& statusText, const wstring& lastMessage) {
		lock_guard<mutex> lock(g_appState.cloudTask.mutex);
		g_appState.cloudTask.busy = busy;
		g_appState.cloudTask.progress = progress;
		g_appState.cloudTask.activeConfigIndex = configIndex;
		if (!statusText.empty()) {
			g_appState.cloudTask.statusText = statusText;
		}
		if (!lastMessage.empty()) {
			g_appState.cloudTask.lastMessage = lastMessage;
		}
	}

	void UpdateConfigCloudLastResult(int configIndex, const CloudCommandResult& result) {
		lock_guard<mutex> lock(g_appState.configsMutex);
		auto it = g_appState.configs.find(configIndex);
		if (it == g_appState.configs.end()) return;

		it->second.cloudLastRunUtc = FolderRewindFormat::MakeUtcTimestampString();
		it->second.cloudLastExitCode = result.exitCode;
		it->second.cloudLastErrorMessage = result.success ? L"" : result.message;
	}

	CloudCommandResult MakeConfigErrorResult(const char* key, const wstring& detail) {
		CloudCommandResult result;
		result.success = false;
		result.exitCode = -1;
		result.message = utf8_to_wstring(L(key));
		result.detail = detail;
		return result;
	}

	bool EnsureCloudConfigured(const Config& config, CloudCommandResult& outResult) {
		if (config.pendingLocalBinding) {
			outResult = MakeConfigErrorResult("CLOUD_CONFIG_INVALID", L"This configuration is waiting for local path binding.");
			return false;
		}
		if (config.rcloneRemotePath.empty()) {
			outResult = MakeConfigErrorResult("CLOUD_CONFIG_INVALID");
			return false;
		}
		const auto rclone = ResolveRcloneExecutable(config);
		if (!rclone.available) {
			outResult = MakeConfigErrorResult("CLOUD_RCLONE_NOT_FOUND", rclone.diagnostic);
			return false;
		}
		if (!config.cloudWorkingDirectory.empty() && !filesystem::exists(config.cloudWorkingDirectory)) {
			outResult = MakeConfigErrorResult("CLOUD_WORKDIR_MISSING", config.cloudWorkingDirectory);
			return false;
		}
		return true;
	}

	FolderRewindFormat::StoragePaths ResolveStoragePaths(const Config& config, const wstring& folderName, const wstring& folderPath) {
		FolderRewindFormat::StoragePaths storagePaths;
		if (!FolderRewindFormat::TryResolveStoragePaths(config.backupPath, folderName, folderPath, storagePaths)) {
			storagePaths.folderName = FolderRewindFormat::SanitizePathSegment(folderName);
			storagePaths.backupSubDir = filesystem::path(config.backupPath) / storagePaths.folderName;
			storagePaths.metadataDir = filesystem::path(config.backupPath) / FolderRewindFormat::kMetadataRootDirName / storagePaths.folderName;
			storagePaths.recordsDir = storagePaths.metadataDir / FolderRewindFormat::kMetadataRecordsDirName;
			storagePaths.statePath = storagePaths.metadataDir / FolderRewindFormat::kMetadataStateFileName;
		}
		return storagePaths;
	}

	HistoryCloudPaths BuildHistoryPaths(const Config& config, const HistoryEntry& entry) {
		HistoryCloudPaths paths;
		FolderRewindFormat::StoragePaths storagePaths = ResolveStoragePaths(config, entry.worldName, entry.worldPath);

		paths.archiveLocalPath = storagePaths.backupSubDir / entry.backupFile;
		paths.metadataDir = storagePaths.metadataDir;
		paths.metadataStateLocalPath = storagePaths.statePath;
		paths.metadataRecordLocalPath = storagePaths.recordsDir / (entry.backupFile + L".json");

		paths.archiveRemotePath = entry.cloudArchiveRemotePath.empty()
			? FolderRewindFormat::BuildArchiveRemotePath(config, storagePaths.folderName, entry.backupFile)
			: entry.cloudArchiveRemotePath;
		paths.metadataStateRemotePath = entry.cloudMetadataStateRemotePath.empty()
			? FolderRewindFormat::BuildMetadataStateRemotePath(config, storagePaths.folderName)
			: entry.cloudMetadataStateRemotePath;
		paths.metadataRecordRemotePath = entry.cloudMetadataRecordRemotePath.empty()
			? FolderRewindFormat::BuildMetadataRecordRemotePath(config, storagePaths.folderName, entry.backupFile)
			: entry.cloudMetadataRecordRemotePath;
		return paths;
	}

	ProcessSpec BuildRcloneCopyToCommand(const Config& config, const wstring& sourcePath, const wstring& destinationPath) {
		return RcloneClient::BuildCopyToCommand(
			ResolveRcloneExecutable(config).executable,
			sourcePath,
			destinationPath);
	}

	CloudOperationScope::CloudOperationScope(int configIndex, const wstring& initialStatus)
		: configIndex_(configIndex), initialStatus_(initialStatus) {
		SetCloudRuntimeState(configIndex_, true, 0, initialStatus_);
	}

	CloudOperationScope::~CloudOperationScope() {
		if (!finished_) {
			// 提前返回或异常也必须只在这里清除 busy，避免 UI 永久停留在运行态。
			SetCloudRuntimeState(configIndex_, false, 100, initialStatus_);
		}
	}

	void CloudOperationScope::Finish(const CloudCommandResult& result, bool updateConfig) {
		if (finished_) return;
		if (updateConfig) UpdateConfigCloudLastResult(configIndex_, result);
		SetCloudRuntimeState(configIndex_, false, 100, result.message, result.message);
		finished_ = true;
	}

	void CloudOperationScope::Finish(const wstring& message, bool success, bool updateConfig) {
		CloudCommandResult result;
		result.success = success;
		result.exitCode = success ? 0 : -1;
		result.message = message;
		Finish(result, updateConfig);
	}


	ProcessSpec BuildRcloneCopyCommand(const Config& config, const wstring& sourcePath, const wstring& destinationPath) {
		return RcloneClient::BuildCopyCommand(
			ResolveRcloneExecutable(config).executable,
			sourcePath,
			destinationPath);
	}

	CloudCommandResult ExecuteCommandWithRetry(
		const Config& config,
		int configIndex,
		const ProcessSpec& command,
		const char* busyStatusKey,
		int progress) {
		SetCloudRuntimeState(configIndex, true, progress, utf8_to_wstring(L(busyStatusKey)));
		RcloneClientOptions options;
		options.executable = command.executable;
		options.workingDirectory = config.cloudWorkingDirectory;
		options.timeout = chrono::seconds(config.cloudTimeoutSeconds);
		options.retryCount = config.cloudRetryCount;
		options.useLowPriority = config.useLowPriority;
		options.stopToken = TaskCoordinator::CurrentStopToken();
		const RcloneExecutionResult execution =
			RcloneClient(std::move(options)).Execute(command.arguments);
		CloudCommandResult result = execution.command;
		result.message = result.success
			? utf8_to_wstring(L("CLOUD_TASK_COMPLETED"))
			: (result.timedOut
				? MineFormatMessage("CLOUD_TIMEOUT", config.cloudTimeoutSeconds)
				: MineFormatMessage("CLOUD_COMMAND_FAILED_WITH_CODE", result.exitCode));
		return result;
	}

	bool IsRemoteObjectMissing(const CloudCommandResult& result) {
		return RcloneClient::IsRemoteObjectMissing(result);
	}

	nlohmann::json SerializeHistoryEntryForCloud(const Config& config, const HistoryEntry& entry) {
		return FolderRewindHistoryStore::SerializeHistoryItem(config, entry);
	}

	bool TryParseHistoryEntryFromCloudJson(const nlohmann::json& item, HistoryEntry& outEntry, wstring& outConfigId) {
		outConfigId.clear();
		if (FolderRewindHistoryStore::TryParseHistoryItem(item, outEntry, outConfigId)) {
			return true;
		}

		int parsedConfigIndex = -1;
		if (!FolderRewindHistoryStore::TryParseLegacyHistoryItem(item, outEntry, parsedConfigIndex)) {
			return false;
		}

		if (parsedConfigIndex >= 0) {
			lock_guard<mutex> lock(g_appState.configsMutex);
			auto it = g_appState.configs.find(parsedConfigIndex);
			if (it != g_appState.configs.end()) {
				outConfigId = it->second.configId;
			}
		}
		if (outConfigId.empty()) {
			TryResolveKnownConfigId(outEntry, outConfigId);
		}
		outEntry.configId = outConfigId;
		return true;
	}

	bool TryParseCloudHistoryArray(const nlohmann::json& root, vector<HistoryEntry>& outEntries) {
		outEntries.clear();
		if (root.is_discarded() || !root.is_array()) {
			return false;
		}
		for (const auto& item : root) {
			HistoryEntry entry;
			wstring parsedConfigId;
			if (TryParseHistoryEntryFromCloudJson(item, entry, parsedConfigId)) {
				entry.configId = parsedConfigId;
				outEntries.push_back(std::move(entry));
			}
		}
		return root.empty() || !outEntries.empty();
	}

	filesystem::path BuildTempFilePath(const wchar_t* prefix, const wchar_t* extension) {
		wstringstream name;
		name << prefix << L"_" << chrono::steady_clock::now().time_since_epoch().count() << extension;
		return GetAppPaths().runtimeRoot / name.str();
	}

	CloudActiveHistoryManifest BuildActiveManifest(int configIndex) {
		CloudActiveHistoryManifest manifest;
		auto cfgIt = g_appState.configs.find(configIndex);
		if (cfgIt == g_appState.configs.end()) return manifest;

		manifest.configId = cfgIt->second.configId;
		manifest.configName = utf8_to_wstring(cfgIt->second.name);
		manifest.updatedAtUtc = FolderRewindFormat::MakeUtcTimestampString();
		for (const auto& entry : GetHistoryEntriesForConfig(configIndex)) {
			CloudActiveHistoryEntry item;
			item.folderPath = entry.worldPath;
			item.folderName = entry.worldName;
			item.fileName = entry.backupFile;
			item.timestamp = entry.timestamp_str;
			manifest.entries.push_back(std::move(item));
		}
		return manifest;
	}

	wstring BuildActiveManifestRemotePath(const Config& config) {
		return FolderRewindFormat::BuildActiveHistoryManifestRemotePath(config);
	}

	nlohmann::json SerializeManifest(const CloudActiveHistoryManifest& manifest) {
		nlohmann::json root;
		root["ConfigId"] = wstring_to_utf8(manifest.configId);
		root["ConfigName"] = wstring_to_utf8(manifest.configName);
		root["UpdatedAtUtc"] = wstring_to_utf8(manifest.updatedAtUtc);
		root["Entries"] = nlohmann::json::array();
		for (const auto& entry : manifest.entries) {
			nlohmann::json item;
			item["FolderPath"] = wstring_to_utf8(entry.folderPath);
			item["FolderName"] = wstring_to_utf8(entry.folderName);
			item["FileName"] = wstring_to_utf8(entry.fileName);
			item["Timestamp"] = wstring_to_utf8(entry.timestamp);
			root["Entries"].push_back(std::move(item));
		}
		return root;
	}

	bool TryParseManifest(const nlohmann::json& root, CloudActiveHistoryManifest& manifest) {
		return FolderRewindHistoryStore::TryParseActiveHistoryManifest(root, manifest);
	}

	bool ManifestContainsHistoryItem(const CloudActiveHistoryManifest& manifest, const HistoryEntry& entry) {
		return FolderRewindHistoryStore::ManifestContainsHistoryItem(manifest, entry);
	}

	bool HasRemotePrefix(const wstring& value, const wstring& root) {
		if (value.empty() || root.empty()) return false;
		const wstring normalizedValue = NormalizeRemotePath(value);
		const wstring normalizedRoot = NormalizeRemotePath(root);
		return normalizedValue == normalizedRoot
			|| normalizedValue.rfind(normalizedRoot + L"/", 0) == 0;
	}

	bool TryResolveKnownConfigId(const HistoryEntry& entry, wstring& outConfigId) {
		lock_guard<mutex> lock(g_appState.configsMutex);
		const Config* matchedConfig = nullptr;
		for (const auto& pair : g_appState.configs) {
			if (!BelongsToConfiguration(pair.second, entry)) {
				continue;
			}
			if (matchedConfig != nullptr) {
				outConfigId.clear();
				return false;
			}
			matchedConfig = &pair.second;
		}
		if (matchedConfig == nullptr || matchedConfig->configId.empty()) {
			outConfigId.clear();
			return false;
		}
		outConfigId = matchedConfig->configId;
		return true;
	}

	bool BelongsToConfiguration(const Config& config, const HistoryEntry& entry) {
		if (!entry.configId.empty() && !config.configId.empty()) {
			return _wcsicmp(entry.configId.c_str(), config.configId.c_str()) == 0;
		}

		const wstring configRoot = FolderRewindFormat::BuildConfigCloudRoot(config);
		if (HasRemotePrefix(entry.cloudArchiveRemotePath, configRoot)
			|| HasRemotePrefix(entry.cloudMetadataRecordRemotePath, configRoot)
			|| HasRemotePrefix(entry.cloudMetadataStateRemotePath, configRoot)) {
			return true;
		}
		for (const auto& world : config.worlds) {
			if (entry.worldName == world.first) return true;
		}
		return false;
	}

	CloudCommandResult UploadConfigurationHistorySnapshotNoLock(const Config& config, int configIndex) {
		CloudCommandResult configError;
		if (!EnsureCloudConfigured(config, configError)) {
			return configError;
		}

		const filesystem::path tempHistoryPath = BuildTempFilePath(L"MineBackup_cloud_history", L".json");
		const filesystem::path tempManifestPath = BuildTempFilePath(L"MineBackup_cloud_manifest", L".json");
		const auto cleanupTempFiles = [&]() {
			error_code ec;
			filesystem::remove(tempHistoryPath, ec);
			filesystem::remove(tempManifestPath, ec);
		};

		CloudCommandResult result;
		CloudCommandResult remoteLoadResult;
		vector<HistoryEntry> mergedEntries = LoadRemoteHistoryEntriesNoLock(config, configIndex, remoteLoadResult);
		if (!remoteLoadResult.success) {

			if (IsRemoteObjectMissing(remoteLoadResult)) {
				// A genuinely absent remote history is the expected first-upload case.
				mergedEntries.clear();
			}
			else {
				cleanupTempFiles();
				return remoteLoadResult;
			}
		}
		mergedEntries.erase(
			remove_if(mergedEntries.begin(), mergedEntries.end(), [&](const HistoryEntry& entry) {
				return BelongsToConfiguration(config, entry);
			}),
			mergedEntries.end());
		vector<HistoryEntry> localEntries = GetHistoryEntriesForConfig(configIndex);
		mergedEntries.insert(mergedEntries.end(), localEntries.begin(), localEntries.end());

		ofstream historyOut(tempHistoryPath, ios::binary | ios::trunc);
		if (!historyOut.is_open()) {
			cleanupTempFiles();
			return MakeConfigErrorResult("CLOUD_HISTORY_EXPORT_FAILED");
		}
		nlohmann::json historyRoot = nlohmann::json::array();
		for (const auto& entry : mergedEntries) {
			historyRoot.push_back(SerializeHistoryEntryForCloud(BelongsToConfiguration(config, entry) ? config : Config{}, entry));
		}
		historyOut << historyRoot.dump(2);
		historyOut.close();

		ofstream manifestOut(tempManifestPath, ios::binary | ios::trunc);
		if (!manifestOut.is_open()) {
			cleanupTempFiles();
			return MakeConfigErrorResult("CLOUD_HISTORY_EXPORT_FAILED");
		}
		manifestOut << SerializeManifest(BuildActiveManifest(configIndex)).dump(2);
		manifestOut.close();

		const wstring historyRemotePath = FolderRewindFormat::BuildGlobalHistoryRemotePath(config);
		result = ExecuteCommandWithRetry(config, configIndex,
			BuildRcloneCopyToCommand(config, tempHistoryPath.wstring(), historyRemotePath),
			"CLOUD_STATUS_UPLOADING_HISTORY",
			85);
		if (!result.success) {
			cleanupTempFiles();
			return result;
		}

		const wstring manifestRemotePath = BuildActiveManifestRemotePath(config);
		CloudCommandResult manifestResult = ExecuteCommandWithRetry(config, configIndex,
			BuildRcloneCopyToCommand(config, tempManifestPath.wstring(), manifestRemotePath),
			"CLOUD_STATUS_UPLOADING_HISTORY",
			92);

		if (!manifestResult.success) {
			manifestResult.success = true;
			manifestResult.message = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
			cleanupTempFiles();
			return manifestResult;
		}

		result.success = true;
		result.exitCode = 0;
		result.message = utf8_to_wstring(L("CLOUD_HISTORY_UPLOAD_SUCCEEDED"));
		cleanupTempFiles();
		return result;
	}

	CloudCommandResult UploadHistoryEntryNoLock(const Config& config, int configIndex, const HistoryEntry& entry) {
		CloudCommandResult configError;
		if (!EnsureCloudConfigured(config, configError)) {
			return configError;
		}

		const HistoryCloudPaths paths = BuildHistoryPaths(config, entry);
		if (!filesystem::exists(paths.archiveLocalPath)) {
			return MakeConfigErrorResult("CLOUD_LOCAL_ARCHIVE_MISSING", paths.archiveLocalPath.wstring());
		}

		CloudCommandResult result = ExecuteCommandWithRetry(
			config,
			configIndex,
			BuildRcloneCopyToCommand(config, paths.archiveLocalPath.wstring(), paths.archiveRemotePath),
			"CLOUD_STATUS_UPLOADING_ARCHIVE",
			40);
		if (!result.success) {
			result.message = MineFormatMessage("CLOUD_UPLOAD_FAILED", wstring_to_utf8(entry.backupFile).c_str());
			return result;
		}

		wstring warningMessage;
		const filesystem::path cloudMigrationMarker = paths.metadataDir / L".cloud-v15-migrated";
		const bool needsLegacyMetadataSnapshot = filesystem::exists(paths.metadataDir / L"metadata.json")
			&& !filesystem::exists(cloudMigrationMarker);
		if (needsLegacyMetadataSnapshot) {
			const wstring stamp = FolderRewindFormat::MakeLocalTimestampString();
			const wstring snapshotRoot = FolderRewindFormat::AppendRemotePath(config.rcloneRemotePath,
				{ L"_minebackup", L"migration-backups", L"1.15", stamp, utf8_to_wstring(config.name), entry.worldName });
			// Missing remote metadata is normal for archives that were never uploaded by 1.15.
			CloudCommandResult stateSnapshot = ExecuteCommandWithRetry(config, configIndex,
				BuildRcloneCopyToCommand(config, paths.metadataStateRemotePath,
					FolderRewindFormat::AppendRemotePath(snapshotRoot, { L"state.json" })), "CLOUD_STATUS_UPLOADING_METADATA", 52);
			if (!stateSnapshot.success && !IsRemoteObjectMissing(stateSnapshot)) {
				stateSnapshot.message = L"Remote metadata snapshot failed; converted metadata was not uploaded.";
				return stateSnapshot;
			}
			CloudCommandResult recordSnapshot = ExecuteCommandWithRetry(config, configIndex,
				BuildRcloneCopyToCommand(config, paths.metadataRecordRemotePath,
					FolderRewindFormat::AppendRemotePath(snapshotRoot, { L"records", entry.backupFile + L".json" })), "CLOUD_STATUS_UPLOADING_METADATA", 54);
			if (!recordSnapshot.success && !IsRemoteObjectMissing(recordSnapshot)) {
				recordSnapshot.message = L"Remote metadata record snapshot failed; converted metadata was not uploaded.";
				return recordSnapshot;
			}
		}
		bool metadataUploadComplete = true;
		if (filesystem::exists(paths.metadataStateLocalPath)) {
			CloudCommandResult metadataStateResult = ExecuteCommandWithRetry(
				config,
				configIndex,
				BuildRcloneCopyToCommand(config, paths.metadataStateLocalPath.wstring(), paths.metadataStateRemotePath),
				"CLOUD_STATUS_UPLOADING_METADATA",
				60);
			if (!metadataStateResult.success) {
				warningMessage = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
				metadataUploadComplete = false;
			}
		}

		if (filesystem::exists(paths.metadataRecordLocalPath)) {
			CloudCommandResult metadataRecordResult = ExecuteCommandWithRetry(
				config,
				configIndex,
				BuildRcloneCopyToCommand(config, paths.metadataRecordLocalPath.wstring(), paths.metadataRecordRemotePath),
				"CLOUD_STATUS_UPLOADING_METADATA",
				75);
			if (!metadataRecordResult.success) {
				warningMessage = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
				metadataUploadComplete = false;
			}
		}
		if (needsLegacyMetadataSnapshot && metadataUploadComplete) {
			ofstream marker(cloudMigrationMarker, ios::binary | ios::trunc);
			marker << "FolderRewind v3 metadata uploaded after 1.15 recovery snapshot.";
		}

		UpdateHistoryCloudState(
			configIndex,
			entry.worldName,
			entry.backupFile,
			true,
			FolderRewindFormat::MakeUtcTimestampString(),
			paths.archiveRemotePath,
			paths.metadataRecordRemotePath,
			paths.metadataStateRemotePath);

		if (config.cloudSyncHistoryAfterUpload) {
			CloudCommandResult historyResult = UploadConfigurationHistorySnapshotNoLock(config, configIndex);
			if (!historyResult.success) {
				warningMessage = historyResult.message;
			}
		}

		result.success = true;
		result.exitCode = 0;
		result.message = warningMessage.empty()
			? MineFormatMessage("CLOUD_UPLOAD_SUCCEEDED", wstring_to_utf8(entry.backupFile).c_str())
			: warningMessage;
		return result;
	}

	CloudCommandResult DownloadHistoryEntryNoLock(const Config& config, int configIndex, const HistoryEntry& entry) {
		CloudCommandResult configError;
		if (!EnsureCloudConfigured(config, configError)) {
			return configError;
		}

		HistoryCloudPaths paths = BuildHistoryPaths(config, entry);
		if (paths.archiveRemotePath.empty()) {
			return MakeConfigErrorResult("CLOUD_NO_REMOTE_COPY");
		}

		error_code ec;
		filesystem::create_directories(paths.archiveLocalPath.parent_path(), ec);
		filesystem::create_directories(paths.metadataDir, ec);

		CloudCommandResult result = ExecuteCommandWithRetry(
			config,
			configIndex,
			BuildRcloneCopyToCommand(config, paths.archiveRemotePath, paths.archiveLocalPath.wstring()),
			"CLOUD_STATUS_DOWNLOADING_ARCHIVE",
			45);
		if (!result.success) {
			result.message = MineFormatMessage("CLOUD_DOWNLOAD_FAILED", wstring_to_utf8(entry.backupFile).c_str());
			return result;
		}

		wstring warningMessage;
		CloudCommandResult metadataStateResult = ExecuteCommandWithRetry(
			config,
			configIndex,
			BuildRcloneCopyToCommand(config, paths.metadataStateRemotePath, paths.metadataStateLocalPath.wstring()),

			"CLOUD_STATUS_DOWNLOADING_METADATA",
			65);
		if (!metadataStateResult.success) {
			warningMessage = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
		}

		CloudCommandResult metadataRecordResult = ExecuteCommandWithRetry(
			config,
			configIndex,
			BuildRcloneCopyToCommand(config, paths.metadataRecordRemotePath, paths.metadataRecordLocalPath.wstring()),
			"CLOUD_STATUS_DOWNLOADING_METADATA",
			80);
		if (!metadataRecordResult.success) {
			warningMessage = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
		}

		// A 1.15 cloud state used the state.json destination but contained metadata v2 camelCase.
		// Convert the downloaded small JSON files locally; the archive remains untouched.
		bool downloadedLegacyState = false;
		try {
			ifstream stateIn(paths.metadataStateLocalPath, ios::binary);
			nlohmann::json stateRoot = nlohmann::json::parse(stateIn, nullptr, false);
			downloadedLegacyState = stateRoot.is_object() && stateRoot.contains("fileStates") && !stateRoot.contains("FileStates");
		}
		catch (...) {}
		if (downloadedLegacyState) {
			const filesystem::path legacySummary = paths.metadataDir / L"metadata.json";
			const filesystem::path legacyRecord = paths.metadataDir / (entry.backupFile + L".json");
			error_code migrateEc;
			filesystem::copy_file(paths.metadataStateLocalPath, legacySummary, filesystem::copy_options::overwrite_existing, migrateEc);
			migrateEc.clear();
			if (filesystem::exists(paths.metadataRecordLocalPath))
				filesystem::copy_file(paths.metadataRecordLocalPath, legacyRecord, filesystem::copy_options::overwrite_existing, migrateEc);
			filesystem::remove(paths.metadataStateLocalPath, migrateEc);
			filesystem::remove(paths.metadataRecordLocalPath, migrateEc);
			const MigrationUnitResult migrated = MigrationCoordinator::EnsureWorldMigrated(config, configIndex, entry.worldName, entry.worldPath);
			if (migrated.status == MigrationStatus::Failed || migrated.status == MigrationStatus::Degraded)
				warningMessage = L"Downloaded legacy metadata could not be migrated completely: " + migrated.message;
		}

		UpdateHistoryCloudState(
			configIndex,
			entry.worldName,
			entry.backupFile,
			true,
			entry.cloudArchivedAtUtc.empty() ? FolderRewindFormat::MakeUtcTimestampString() : entry.cloudArchivedAtUtc,
			paths.archiveRemotePath,
			paths.metadataRecordRemotePath,
			paths.metadataStateRemotePath);

		result.success = true;
		result.exitCode = 0;
		result.message = warningMessage.empty()
			? MineFormatMessage("CLOUD_DOWNLOAD_SUCCEEDED", wstring_to_utf8(entry.backupFile).c_str())
			: warningMessage;
		return result;
	}

	vector<HistoryEntry> LoadRemoteHistoryEntriesNoLock(const Config& config, int configIndex, CloudCommandResult& outResult) {
		vector<HistoryEntry> entries;
		const MigrationUnitResult cloudGate = MigrationCoordinator::EnsureCloudMigrated(configIndex);
		if (cloudGate.status == MigrationStatus::Failed) {
			outResult.success = false;
			outResult.message = cloudGate.message;
			return entries;
		}
		const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_cloud_analysis", L".json");
		const wstring remoteHistoryPath = FolderRewindFormat::BuildGlobalHistoryRemotePath(config);
		outResult = ExecuteCommandWithRetry(
			config,
			configIndex,
			BuildRcloneCopyToCommand(config, remoteHistoryPath, tempPath.wstring()),
			"CLOUD_STATUS_ANALYZING",
			20);
		if (!outResult.success) {
			error_code ec;
			filesystem::remove(tempPath, ec);
			return entries;
		}

		try {
			ifstream in(tempPath, ios::binary);
			nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
			if (!TryParseCloudHistoryArray(root, entries)) {
				outResult.success = false;
				outResult.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_FAILED"));
			}
			else if (IsLegacyHistoryJson(root)) {
				const bool hasUnmapped = any_of(entries.begin(), entries.end(), [](const HistoryEntry& entry) { return entry.configId.empty(); });
				if (hasUnmapped || entries.size() != root.size()) {
					outResult.success = false;
					outResult.message = L"Legacy cloud history contains entries that cannot be mapped safely; remote data was not changed.";
					MigrationCoordinator::RecordCloudMigrationResult(configIndex, MigrationStatus::Failed, outResult.message);
				}
				else {
					const wstring stamp = FolderRewindFormat::MakeLocalTimestampString();
					const wstring backupRemote = FolderRewindFormat::AppendRemotePath(config.rcloneRemotePath,
						{ L"_minebackup", L"migration-backups", L"1.15", stamp, L"history.json" });
					CloudCommandResult snapshotResult = ExecuteCommandWithRetry(config, configIndex,
						BuildRcloneCopyToCommand(config, remoteHistoryPath, backupRemote), "CLOUD_STATUS_ANALYZING", 24);
					if (!snapshotResult.success) {
						outResult = snapshotResult;
						outResult.message = L"Could not create the remote 1.15 history snapshot; migration was aborted.";
						MigrationCoordinator::RecordCloudMigrationResult(configIndex, MigrationStatus::Failed, outResult.message);
					}
					else {
						nlohmann::json converted = nlohmann::json::array();
						for (const auto& entry : entries) {
							const Config* owner = nullptr;
							for (const auto& [candidateIndex, candidate] : g_appState.configs)
								if (_wcsicmp(candidate.configId.c_str(), entry.configId.c_str()) == 0) { owner = &candidate; break; }
							if (!owner) { outResult.success = false; break; }
							converted.push_back(FolderRewindHistoryStore::SerializeHistoryItem(*owner, entry));
						}
						if (outResult.success) {
							ofstream convertedOut(tempPath, ios::binary | ios::trunc);
							convertedOut << converted.dump(2);
							convertedOut.close();
							outResult = ExecuteCommandWithRetry(config, configIndex,
								BuildRcloneCopyToCommand(config, tempPath.wstring(), remoteHistoryPath), "CLOUD_STATUS_ANALYZING", 28);
							if (outResult.success) MigrationCoordinator::RecordCloudMigrationResult(configIndex, MigrationStatus::Succeeded,
								L"Legacy cloud history migrated; archive objects were left in place.", backupRemote);
							else MigrationCoordinator::RecordCloudMigrationResult(configIndex, MigrationStatus::Failed,
								L"Uploading converted cloud history failed; the recovery snapshot is intact.", backupRemote);
						}
					}
				}
			}
		}
		catch (...) {
			outResult.success = false;
			outResult.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_FAILED"));
		}

		error_code ec;
		filesystem::remove(tempPath, ec);
		return entries;
	}

	ActiveManifestLoadStatus TryLoadActiveManifestNoLock(const Config& config, int configIndex,
		CloudActiveHistoryManifest& outManifest, CloudCommandResult& outResult) {
		const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_cloud_active_history", L".json");
		CloudCommandResult result = ExecuteCommandWithRetry(
			config,
			configIndex,
			BuildRcloneCopyToCommand(config, BuildActiveManifestRemotePath(config), tempPath.wstring()),
			"CLOUD_STATUS_ANALYZING",
			30);
		if (!result.success) {
			if (!IsRemoteObjectMissing(result)) {
				outResult = result;
				error_code ec; filesystem::remove(tempPath, ec);
				return ActiveManifestLoadStatus::Failed;
			}
			// 1.15 stored the manifest under _minebackup. Read and convert it lazily.
			const wstring legacyRemote = FolderRewindFormat::AppendRemotePath(config.rcloneRemotePath,
				{ utf8_to_wstring(config.name), L"_minebackup", L"active-history.json" });
			result = ExecuteCommandWithRetry(config, configIndex,
				BuildRcloneCopyToCommand(config, legacyRemote, tempPath.wstring()), "CLOUD_STATUS_ANALYZING", 30);
			if (!result.success) {
				outResult = result;
				error_code ec; filesystem::remove(tempPath, ec);
				return IsRemoteObjectMissing(result) ? ActiveManifestLoadStatus::NotFound : ActiveManifestLoadStatus::Failed;
			}
			ifstream legacyIn(tempPath, ios::binary);
			nlohmann::json legacy = nlohmann::json::parse(legacyIn, nullptr, false);
			legacyIn.close();
			if (!legacy.is_object() || !legacy.contains("entries") || !legacy["entries"].is_array()) {
				outResult.success = false; outResult.message = L"Legacy active-history manifest is malformed.";
				error_code ec; filesystem::remove(tempPath, ec); return ActiveManifestLoadStatus::Failed;
			}
			CloudActiveHistoryManifest converted;
			converted.configId = config.configId;
			converted.configName = utf8_to_wstring(config.name);
			converted.updatedAtUtc = legacy.value("updatedAtUtc", string{}).empty()
				? FolderRewindFormat::MakeUtcTimestampString() : utf8_to_wstring(legacy.value("updatedAtUtc", string{}));
			for (const auto& item : legacy["entries"]) {
				if (!item.is_object()) continue;
				CloudActiveHistoryEntry entry;
				entry.folderPath = utf8_to_wstring(item.value("worldPath", string{}));
				entry.folderName = utf8_to_wstring(item.value("worldName", string{}));
				entry.fileName = utf8_to_wstring(item.value("backupFile", string{}));
				entry.timestamp = utf8_to_wstring(item.value("timestamp", string{}));
				if (!entry.folderName.empty() && !entry.fileName.empty()) converted.entries.push_back(std::move(entry));
			}
			const wstring stamp = FolderRewindFormat::MakeLocalTimestampString();
			const wstring backupRemote = FolderRewindFormat::AppendRemotePath(config.rcloneRemotePath,
				{ L"_minebackup", L"migration-backups", L"1.15", stamp, utf8_to_wstring(config.name), L"active-history.json" });
			CloudCommandResult snapshot = ExecuteCommandWithRetry(config, configIndex,
				BuildRcloneCopyToCommand(config, legacyRemote, backupRemote), "CLOUD_STATUS_ANALYZING", 31);
			if (!snapshot.success) { outResult = snapshot; error_code ec; filesystem::remove(tempPath, ec); return ActiveManifestLoadStatus::Failed; }
			ofstream convertedOut(tempPath, ios::binary | ios::trunc);
			convertedOut << SerializeManifest(converted).dump(2);
			convertedOut.close();
			CloudCommandResult upload = ExecuteCommandWithRetry(config, configIndex,
				BuildRcloneCopyToCommand(config, tempPath.wstring(), BuildActiveManifestRemotePath(config)), "CLOUD_STATUS_ANALYZING", 32);
			if (!upload.success) { outResult = upload; error_code ec; filesystem::remove(tempPath, ec); return ActiveManifestLoadStatus::Failed; }
			outManifest = std::move(converted);
			outResult.success = true; outResult.exitCode = 0;
			error_code ec; filesystem::remove(tempPath, ec); return ActiveManifestLoadStatus::Loaded;

		}

		bool ok = false;
		try {
			ifstream in(tempPath, ios::binary);
			nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
			if (!root.is_discarded()) {
				ok = TryParseManifest(root, outManifest);
				const bool looksLegacyManifest = root.is_object()
					&& root.find("entries") != root.end()
					&& root.find("Entries") == root.end();
				if (looksLegacyManifest) {
					ok = false;
				}
			}
		}
		catch (...) {
			ok = false;
		}

		error_code ec;
		filesystem::remove(tempPath, ec);
		if (!ok) {
			outResult.success = false;
			outResult.message = L"FolderRewind active-history manifest is malformed.";
			return ActiveManifestLoadStatus::Failed;
		}
		outResult.success = true; outResult.exitCode = 0;
		return ActiveManifestLoadStatus::Loaded;
	}

	bool HasLocalBackupOrMetadataInternal(const Config& config, const HistoryEntry& entry) {
		const HistoryCloudPaths paths = BuildHistoryPaths(config, entry);
		if (!filesystem::exists(paths.archiveLocalPath)) {
			return false;
		}
		if (IsIncrementalBackupType(entry.backupType) || IsIncrementalBackupType(entry.backupFile)) {
			return filesystem::exists(paths.metadataStateLocalPath) && filesystem::exists(paths.metadataRecordLocalPath);
		}
		return true;
	}
}

using namespace CloudSyncInternal;

bool CanUseCloudActions(const Config& config) {
	if (!config.cloudSyncEnabled) return false;
	CloudCommandResult result;
	return EnsureCloudConfigured(config, result);
}

wstring GetCloudActionsUnavailableReason(const Config& config) {
	if (!config.cloudSyncEnabled) return utf8_to_wstring(L("CLOUD_SYNC_DISABLED_REASON"));
	CloudCommandResult result;
	if (EnsureCloudConfigured(config, result)) return {};
	if (!result.detail.empty()) return result.message + L"\n" + result.detail;
	return result.message;
}

bool HasHistoryCloudCopy(const HistoryEntry& entry) {
	return entry.isCloudArchived && !entry.cloudArchiveRemotePath.empty();
}

bool HasLocalBackupOrMetadata(const Config& config, const HistoryEntry& entry) {
	return HasLocalBackupOrMetadataInternal(config, entry);
}

int ResolveConfigIndexForCloud(const Config& config) {
	{
		lock_guard<mutex> lock(g_appState.configsMutex);
		for (const auto& pair : g_appState.configs) {
			const Config& candidate = pair.second;
			if (candidate.backupPath == config.backupPath
				&& candidate.saveRoot == config.saveRoot
				&& candidate.name == config.name) {
				return pair.first;
			}
		}
		for (const auto& pair : g_appState.configs) {
			const Config& candidate = pair.second;
			if (candidate.backupPath == config.backupPath
				&& candidate.saveRoot == config.saveRoot) {
				return pair.first;
			}
		}
	}
	return -1;
}

bool QueueUploadAfterBackup(const Config& config, int configIndex, const MyFolder& folder, const wstring& archiveFile, const wstring& comment) {
	(void)comment;
	if (!config.cloudSyncEnabled) {
		return false;
	}

	HistoryEntry historyEntry;
	if (!TryGetHistoryEntry(configIndex, folder.name, archiveFile, historyEntry)) {
		historyEntry.worldName = folder.name;
		historyEntry.worldPath = folder.path;
		historyEntry.backupFile = archiveFile;
		historyEntry.backupType = L"";
	}

	const Config configCopy = config;
	const int configIndexCopy = configIndex;
	const HistoryEntry entryCopy = historyEntry;

	return TaskCoordinator::Instance().Submit(L"cloud-upload-after-backup",
		{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
		[configCopy, configIndexCopy, entryCopy](stop_token) {
		unique_lock<mutex> lock(g_cloudMutex);
		CloudOperationScope operation(configIndexCopy, utf8_to_wstring(L("CLOUD_STATUS_PREPARING")));
		CloudCommandResult result = UploadHistoryEntryNoLock(configCopy, configIndexCopy, entryCopy);
		operation.Finish(result);
	});
}

bool QueueConfigurationHistorySyncAfterLocalChange(const Config& config, int configIndex, const char* reason) {
	if (!config.cloudSyncEnabled || !CanUseCloudActions(config)) {
		return false;
	}

	const Config configCopy = config;
	const string reasonCopy = reason ? reason : "";
	return TaskCoordinator::Instance().Submit(L"cloud-history-sync",
		{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
		[configCopy, configIndex, reasonCopy](stop_token) {
		unique_lock<mutex> lock(g_cloudMutex);
		CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_UPLOADING_HISTORY")));
		CloudCommandResult result = UploadConfigurationHistorySnapshotNoLock(configCopy, configIndex);
		if (result.success && !reasonCopy.empty()) {
			MB_LOG_I18N_INFO(minebackup::logging::LogCategory::Cloud,
				"cloud.history.background_sync_completed",
				"CLOUD_BACKGROUND_HISTORY_SYNC_DONE", reasonCopy.c_str());
		}
		operation.Finish(result);
	});
}
