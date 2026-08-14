#include "BackupManager.h"
#include "BackupManagerInternal.h"

#include "AppPaths.h"
#include "ChainSafeRetention.h"
#include "CloudSyncService.h"
#include "ConfigManager.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "Globals.h"
#include "HistoryManager.h"
#include "Logging.h"
#include "MigrationCoordinator.h"
#include "PlatformCompat.h"
#include "text_to_text.h"
#include "i18n.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <set>
#include <sstream>

using namespace std;
using namespace BackupManagerInternal;

#define BACKUP_INFO(...) MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Backup, "backup.progress", __VA_ARGS__)
#define BACKUP_WARNING(...) MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Backup, "backup.warning", __VA_ARGS__)
#define BACKUP_ERROR(...) MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Backup, "backup.error", __VA_ARGS__)

namespace {

vector<wstring> ToSortedVector(const set<wstring>& values) {
	return {values.begin(), values.end()};
}

bool TryRepairMetadataAfterSafeDelete(
	const Config& config,
	const wstring& worldName,
	const wstring& deletedBackupFile,
	const wstring& mergedOldFile,
	const wstring& mergedFinalFile,
	const wstring& mergedBackupType,
	string& errorMessage) {
	errorMessage.clear();
	const filesystem::path metadataDirectory = GetMetadataDirectory(config, worldName);

	FolderRewindFormat::ChangeRecord deletedRecord;
	FolderRewindFormat::ChangeRecord mergedRecord;
	if (!FolderRewindMetadataStore::LoadRecord(
			metadataDirectory,
			deletedBackupFile,
			deletedRecord)) {
		errorMessage = "Cannot load deleted FolderRewind metadata record.";
		return false;
	}
	if (!FolderRewindMetadataStore::LoadRecord(
			metadataDirectory,
			mergedOldFile,
			mergedRecord)) {
		errorMessage = "Cannot load merged target FolderRewind metadata record.";
		return false;
	}

	set<wstring> fullAfterDeleted(
		deletedRecord.fullFileList.begin(),
		deletedRecord.fullFileList.end());
	set<wstring> fullBeforeDeleted = fullAfterDeleted;
	for (const auto& path : deletedRecord.addedFiles) fullBeforeDeleted.erase(path);
	for (const auto& path : deletedRecord.deletedFiles) fullBeforeDeleted.insert(path);
	const set<wstring> fullAfterMerged(
		mergedRecord.fullFileList.begin(),
		mergedRecord.fullFileList.end());

	set<wstring> mergedAdded;
	set<wstring> mergedDeleted;
	set_difference(
		fullAfterMerged.begin(),
		fullAfterMerged.end(),
		fullBeforeDeleted.begin(),
		fullBeforeDeleted.end(),
		inserter(mergedAdded, mergedAdded.end()));
	set_difference(
		fullBeforeDeleted.begin(),
		fullBeforeDeleted.end(),
		fullAfterMerged.begin(),
		fullAfterMerged.end(),
		inserter(mergedDeleted, mergedDeleted.end()));

	set<wstring> modifiedCandidates(
		deletedRecord.modifiedFiles.begin(),
		deletedRecord.modifiedFiles.end());
	modifiedCandidates.insert(
		mergedRecord.modifiedFiles.begin(),
		mergedRecord.modifiedFiles.end());
	for (const auto& path : mergedRecord.addedFiles) {
		if (fullBeforeDeleted.contains(path) && fullAfterMerged.contains(path)) {
			modifiedCandidates.insert(path);
		}
	}

	set<wstring> mergedModified;
	for (const auto& path : modifiedCandidates) {
		if (fullBeforeDeleted.contains(path)
			&& fullAfterMerged.contains(path)
			&& !mergedAdded.contains(path)
			&& !mergedDeleted.contains(path)) {
			mergedModified.insert(path);
		}
	}

	FolderRewindFormat::ChangeRecord repaired = mergedRecord;
	repaired.archiveFileName = mergedFinalFile;
	repaired.backupType = mergedBackupType;
	repaired.createdAtUtc = mergedRecord.createdAtUtc.empty()
		? FolderRewindFormat::MakeUtcTimestampString()
		: mergedRecord.createdAtUtc;
	if (FolderRewindFormat::IsSmartBackupType(mergedBackupType)) {
		repaired.previousBackupFileName = deletedRecord.previousBackupFileName;
		repaired.basedOnFullBackup = deletedRecord.basedOnFullBackup.empty()
			? mergedRecord.basedOnFullBackup
			: deletedRecord.basedOnFullBackup;
		repaired.addedFiles = ToSortedVector(mergedAdded);
		repaired.deletedFiles = ToSortedVector(mergedDeleted);
		repaired.modifiedFiles = ToSortedVector(mergedModified);
	}
	else {
		repaired.previousBackupFileName.clear();
		repaired.basedOnFullBackup = mergedFinalFile;
		repaired.addedFiles = ToSortedVector(fullAfterMerged);
		repaired.deletedFiles.clear();
		repaired.modifiedFiles.clear();
	}
	repaired.fullFileList = ToSortedVector(fullAfterMerged);

	if (!FolderRewindMetadataStore::SaveRecord(metadataDirectory, repaired)) {
		errorMessage = "Cannot save repaired FolderRewind metadata record.";
		return false;
	}
	FolderRewindMetadataStore::DeleteRecord(metadataDirectory, deletedBackupFile);
	if (mergedOldFile != mergedFinalFile) {
		FolderRewindMetadataStore::DeleteRecord(metadataDirectory, mergedOldFile);
	}

	auto loaded = FolderRewindMetadataStore::Load(metadataDirectory);
	if (loaded.recordLoadFailed) {
		errorMessage = "Cannot load FolderRewind metadata records after repair.";
		return false;
	}
	bool repairSucceeded = true;
	for (auto& [ignored, value] : loaded.records) {
		FolderRewindFormat::ChangeRecord record = value;
		if (_wcsicmp(record.archiveFileName.c_str(), repaired.archiveFileName.c_str()) == 0) {
			record = repaired;
		}
		if (_wcsicmp(record.previousBackupFileName.c_str(), deletedBackupFile.c_str()) == 0
			|| (mergedOldFile != mergedFinalFile
				&& _wcsicmp(record.previousBackupFileName.c_str(), mergedOldFile.c_str()) == 0)) {
			record.previousBackupFileName = mergedFinalFile;
		}
		if (_wcsicmp(record.basedOnFullBackup.c_str(), deletedBackupFile.c_str()) == 0
			|| (mergedOldFile != mergedFinalFile
				&& _wcsicmp(record.basedOnFullBackup.c_str(), mergedOldFile.c_str()) == 0)) {
			record.basedOnFullBackup = mergedFinalFile;
		}
		repairSucceeded =
			FolderRewindMetadataStore::SaveRecord(metadataDirectory, record)
			&& repairSucceeded;
	}

	FolderRewindFormat::MetadataState state;
	if (FolderRewindMetadataStore::LoadState(metadataDirectory, state)) {
		if (_wcsicmp(state.lastBackupFileName.c_str(), deletedBackupFile.c_str()) == 0
			|| _wcsicmp(state.lastBackupFileName.c_str(), mergedOldFile.c_str()) == 0) {
			state.lastBackupFileName = mergedFinalFile;
		}
		if (_wcsicmp(state.basedOnFullBackup.c_str(), deletedBackupFile.c_str()) == 0
			|| (mergedOldFile != mergedFinalFile
				&& _wcsicmp(state.basedOnFullBackup.c_str(), mergedOldFile.c_str()) == 0)) {
			state.basedOnFullBackup = mergedFinalFile;
		}
		repairSucceeded =
			FolderRewindMetadataStore::SaveState(metadataDirectory, state)
			&& repairSucceeded;
	}
	if (!repairSucceeded) {
		errorMessage = "Cannot save FolderRewind metadata after safe-delete repair.";
	}
	return repairSucceeded;
}

bool DeleteLocalArchiveOnly(const Config& config, const HistoryEntry& entry) {
	const filesystem::path archive =
		JoinPath(config.backupPath, entry.worldName) / entry.backupFile;
	try {
		if (!filesystem::exists(archive)) {
			BACKUP_ERROR(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(entry.backupFile).c_str());
			return false;
		}
		filesystem::remove(archive);
		BACKUP_INFO("  - %s OK", wstring_to_utf8(archive.filename().wstring()).c_str());
		return true;
	}
	catch (const filesystem::filesystem_error& error) {
		BACKUP_ERROR(
			L("LOG_ERROR_DELETE_BACKUP"),
			wstring_to_utf8(archive.filename().wstring()).c_str(),
			error.what());
		return false;
	}
}

} // namespace

void DoSafeDeleteBackupShared(
	const Config& config,
	const HistoryEntry& entry,
	int configIndex);

void BackupManagerInternal::LimitBackupFiles(
	const Config& config,
	const int& configIndex,
	const wstring& folderPath,
	int limit) {
	if (limit <= 0) return;
	vector<filesystem::directory_entry> files;
	try {
		if (!filesystem::is_directory(folderPath)) return;
		for (const auto& entry : filesystem::directory_iterator(folderPath)) {
			if (entry.is_regular_file()) files.push_back(entry);
		}
	}
	catch (const filesystem::filesystem_error& error) {
		BACKUP_ERROR(L("LOG_ERROR_SCAN_BACKUP_DIR"), error.what());
		return;
	}
	if (static_cast<int>(files.size()) <= limit) return;

	const vector<HistoryEntry> history = GetHistoryEntriesForConfig(configIndex);
	const bool historyAvailable = !history.empty();
	sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
		return left.last_write_time() < right.last_write_time();
	});

	vector<filesystem::directory_entry> deletable;
	for (const auto& file : files) {
		bool important = false;
		if (historyAvailable) {
			for (const auto& entry : history) {
				if (entry.worldName == file.path().parent_path().filename().wstring()
					&& entry.backupFile == file.path().filename().wstring()) {
					important = entry.isImportant;
					if (important) {
						BACKUP_INFO(
							L("LOG_INFO_BACKUP_MARKED_IMPORTANT"),
							wstring_to_utf8(file.path().filename().wstring()).c_str());
					}
					break;
				}
			}
		}
		if (!important) deletable.push_back(file);
	}
	if (static_cast<int>(files.size() - deletable.size()) >= limit) {
		BACKUP_INFO("Cannot delete more files; remaining backups are marked as important.");
		return;
	}

	const size_t deleteCount = static_cast<size_t>(max(
		0,
		static_cast<int>(files.size()) - limit));
	for (size_t index = 0; index < deleteCount && index < deletable.size(); ++index) {
		const auto& file = deletable[index];
		try {
			if (FolderRewindFormat::IsSmartBackupType(file.path().filename().wstring())) {
				// 记录实际选择的可删除文件，而不是原始文件列表中的同下标元素。
				BACKUP_WARNING(
					L("LOG_WARNING_DELETE_SMART_BACKUP"),
					wstring_to_utf8(file.path().filename().wstring()).c_str());
			}

			bool handledThroughHistory = false;
			if (historyAvailable) {
				for (const auto& entry : history) {
					if (entry.worldName == file.path().parent_path().filename().wstring()
						&& entry.backupFile == file.path().filename().wstring()) {
						if (isSafeDelete) {
							DoSafeDeleteBackupShared(config, entry, configIndex);
						}
						else {
							int mutableConfigIndex = configIndex;
							DoDeleteBackup(config, entry, mutableConfigIndex);
						}
						handledThroughHistory = true;
						break;
					}
				}
			}
			if (!isSafeDelete && !handledThroughHistory) {
				filesystem::remove(file);
				InvalidateBackupMetadata(
					config,
					file.path().parent_path().filename().wstring(),
					file.path().filename().wstring());
				RemoveHistoryEntry(configIndex, file.path().filename().wstring());
			}
			BACKUP_INFO(
				L("LOG_DELETE_OLD_BACKUP"),
				wstring_to_utf8(file.path().filename().wstring()).c_str());
		}
		catch (const filesystem::filesystem_error& error) {
			BACKUP_ERROR(L("LOG_ERROR_DELETE_BACKUP"), error.what());
		}
	}
}

void DeleteBackupWithMode(
	const Config& config,
	const HistoryEntry& entry,
	int configIndex,
	BackupDeleteMode mode,
	bool useSafeDelete) {
	minebackup::logging::ScopedLogContext context{{
		"operation_id", wstring_to_utf8(FolderRewindFormat::GenerateGuidString())},
		{"config_id", wstring_to_utf8(config.configId)},
		{"world", wstring_to_utf8(entry.worldName)}};
	if (config.pendingLocalBinding) {
		BACKUP_WARNING("This imported configuration is waiting for local path binding.");
		return;
	}
	if (mode == BackupDeleteMode::HistoryOnly) {
		RemoveHistoryEntry(configIndex, entry.worldName, entry.backupFile);
		QueueConfigurationHistorySyncAfterLocalChange(config, configIndex, "history deletion");
		return;
	}

	const auto migration = MigrationCoordinator::EnsureWorldMigrated(
		config,
		configIndex,
		entry.worldName,
		entry.worldPath);
	if (migration.status == MigrationStatus::Failed
		|| migration.status == MigrationStatus::Degraded) {
		BACKUP_ERROR(
			"Local archive deletion is blocked until metadata migration succeeds: %s",
			wstring_to_utf8(migration.message).c_str());
		return;
	}
	if (mode == BackupDeleteMode::LocalArchiveOnly) {
		if (DeleteLocalArchiveOnly(config, entry)) {
			InvalidateBackupMetadata(config, entry.worldName, entry.backupFile);
		}
		return;
	}
	if (useSafeDelete
		&& (FolderRewindFormat::IsSmartBackupType(entry.backupType)
			|| FolderRewindFormat::IsSmartBackupType(entry.backupFile))) {
		DoSafeDeleteBackupShared(config, entry, configIndex);
	}
	else {
		DoDeleteBackup(config, entry, configIndex);
	}
}

void DoDeleteBackup(const Config& config, const HistoryEntry& entry, int& configIndex) {
	BACKUP_INFO(L("LOG_PRE_TO_DELETE"), wstring_to_utf8(entry.backupFile).c_str());
	const filesystem::path archive =
		JoinPath(config.backupPath, entry.worldName) / entry.backupFile;
	try {
		if (filesystem::exists(archive)) {
			filesystem::remove(archive);
			BACKUP_INFO("  - %s OK", wstring_to_utf8(archive.filename().wstring()).c_str());
		}
		else {
			BACKUP_ERROR(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(entry.backupFile).c_str());
		}
		InvalidateBackupMetadata(config, entry.worldName, archive.filename().wstring());
		RemoveHistoryEntry(configIndex, entry.worldName, archive.filename().wstring());
	}
	catch (const filesystem::filesystem_error& error) {
		BACKUP_ERROR(
			L("LOG_ERROR_DELETE_BACKUP"),
			wstring_to_utf8(archive.filename().wstring()).c_str(),
			error.what());
	}
	QueueConfigurationHistorySyncAfterLocalChange(config, configIndex, "backup deletion");
}

void DoSafeDeleteBackupShared(
	const Config& config,
	const HistoryEntry& entry,
	int configIndex) {
	const auto migration = MigrationCoordinator::EnsureWorldMigrated(
		config, configIndex, entry.worldName, entry.worldPath);
	if (migration.status == MigrationStatus::Failed
		|| migration.status == MigrationStatus::Degraded) {
		BACKUP_WARNING(
			"Safe retention requires complete metadata migration: %s",
			wstring_to_utf8(migration.message).c_str());
		return;
	}
	FolderRewindFormat::StoragePaths storage;
	if (!FolderRewindFormat::TryResolveStoragePaths(
			config.backupPath, entry.worldName, entry.worldPath, storage)) {
		BACKUP_WARNING("Safe retention could not resolve the world storage path.");
		return;
	}
	ArchiveRunner archiveRunner = ArchiveRunner::Resolve(
		config.zipPath, GetAppPaths());
	ChainSafeRetention::Request request;
	request.config = config;
	request.entry = entry;
	request.history = GetHistoryEntriesForConfig(configIndex);
	request.backupDirectory = storage.backupSubDir;
	request.metadataDirectory = storage.metadataDir;
	request.paths = GetAppPaths();
	request.archiveRunner = &archiveRunner;
	request.commitHistory = [configIndex](vector<HistoryEntry> updated) {
		return ReplaceHistoryEntriesForConfig(configIndex, std::move(updated));
	};
	const auto result = ChainSafeRetention::Remove(std::move(request));
	if (result.warning) {
		BACKUP_WARNING("Safe retention kept %s: %s",
			wstring_to_utf8(entry.backupFile).c_str(), result.detail.c_str());
	}
	if (result.changed) {
		QueueConfigurationHistorySyncAfterLocalChange(config, configIndex, "safe retention");
	}
}

void DoSafeDeleteBackup(const Config& config, const HistoryEntry& entry, int configIndex) {
	BACKUP_INFO(L("LOG_SAFE_DELETE_START"), wstring_to_utf8(entry.backupFile).c_str());
	const auto migration = MigrationCoordinator::EnsureWorldMigrated(
		config,
		configIndex,
		entry.worldName,
		entry.worldPath);
	if (migration.status == MigrationStatus::Failed
		|| migration.status == MigrationStatus::Degraded) {
		BACKUP_ERROR(
			"Safe delete requires a complete metadata migration: %s",
			wstring_to_utf8(migration.message).c_str());
		return;
	}
	if (entry.isImportant) {
		BACKUP_WARNING(
			L("LOG_SAFE_DELETE_ABORT_IMPORTANT"),
			wstring_to_utf8(entry.backupFile).c_str());
		return;
	}

	const filesystem::path backupDirectory = JoinPath(config.backupPath, entry.worldName);
	const filesystem::path archiveToDelete = backupDirectory / entry.backupFile;
	vector<HistoryEntry> historyEntries = GetHistoryEntriesForConfig(configIndex);
	const HistoryEntry* nextRaw = nullptr;
	vector<const HistoryEntry*> worldHistory;
	for (const auto& value : historyEntries) {
		if (value.worldName == entry.worldName) worldHistory.push_back(&value);
	}
	sort(worldHistory.begin(), worldHistory.end(), [](const auto* left, const auto* right) {
		return left->timestamp_str < right->timestamp_str;
	});
	for (size_t index = 0; index < worldHistory.size(); ++index) {
		if (worldHistory[index]->backupFile == entry.backupFile
			&& index + 1 < worldHistory.size()) {
			nextRaw = worldHistory[index + 1];
			break;
		}
	}
	if (!nextRaw
		|| FolderRewindFormat::IsFullLikeBackupType(nextRaw->backupType)
		|| FolderRewindFormat::IsFullLikeBackupType(nextRaw->backupFile)) {
		BACKUP_INFO(L("LOG_SAFE_DELETE_END_OF_CHAIN"));
		DoDeleteBackup(config, entry, configIndex);
		return;
	}
	if (nextRaw->isImportant) {
		BACKUP_WARNING(
			L("LOG_SAFE_DELETE_ABORT_IMPORTANT_TARGET"),
			wstring_to_utf8(nextRaw->backupFile).c_str());
		return;
	}

	const HistoryEntry next = *nextRaw;
	const filesystem::path mergeTarget = backupDirectory / next.backupFile;
	BACKUP_INFO(
		L("LOG_SAFE_DELETE_MERGE_INFO"),
		wstring_to_utf8(entry.backupFile).c_str(),
		wstring_to_utf8(next.backupFile).c_str());
	if (!filesystem::exists(archiveToDelete) || !filesystem::exists(mergeTarget)) {
		const wstring missing = !filesystem::exists(archiveToDelete)
			? entry.backupFile
			: next.backupFile;
		BACKUP_ERROR(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(missing).c_str());
		DoDeleteBackup(config, entry, configIndex);
		return;
	}

	const auto originalModificationTime = filesystem::last_write_time(mergeTarget);
	const int compressionLevel = NormalizeCompressionLevel(config.zipMethod, config.zipLevel);
	filesystem::path tempBase =
		config.snapshotPath.size() >= 2 && filesystem::exists(config.snapshotPath)
		? filesystem::path(NormalizeSeparators(config.snapshotPath))
		: GetAppPaths().runtimeRoot;
	const auto suffix = to_wstring(chrono::steady_clock::now().time_since_epoch().count());
	const filesystem::path tempRoot = tempBase / (L"MineBackup_Merge_" + suffix);
	const filesystem::path workspace = tempRoot / L"merge_workspace";
	const filesystem::path rebuilt = tempRoot / (L"rebuilt." + config.zipFormat);
	const filesystem::path originalTarget = tempRoot / (L"target_backup." + config.zipFormat);

	bool targetReplaced = false;
	filesystem::path finalArchive = mergeTarget;
	wstring finalType = next.backupType;
	wstring finalName = next.backupFile;
	try {
		filesystem::create_directories(workspace);
		filesystem::copy_file(
			mergeTarget,
			originalTarget,
			filesystem::copy_options::overwrite_existing);
		BACKUP_INFO(L("LOG_SAFE_DELETE_STEP_1"));
		if (!RunInternalProcess(MakeInternalProcess(
				config.zipPath,
				{L"x", archiveToDelete.wstring(), L"-o" + workspace.wstring(), L"-y"},
				{},
				config.useLowPriority))) {
			throw runtime_error("Failed to extract deleted archive.");
		}
		BACKUP_INFO(L("LOG_SAFE_DELETE_STEP_2"));
		if (!RunInternalProcess(MakeInternalProcess(
				config.zipPath,
				{L"x", mergeTarget.wstring(), L"-o" + workspace.wstring(), L"-y"},
				{},
				config.useLowPriority))) {
			throw runtime_error("Failed to extract target archive.");
		}

		error_code ec;
		filesystem::remove_all(
			workspace / FolderRewindFormat::kInternalRestoreMarkerDirectoryName,
			ec);
		auto arguments = SevenZipCreateArguments(config, compressionLevel, rebuilt);
		arguments.push_back(L"*");
		if (!RunInternalProcess(MakeInternalProcess(
				config.zipPath,
				std::move(arguments),
				workspace,
				config.useLowPriority))) {
			throw runtime_error("Failed to rebuild merged archive.");
		}

		ec.clear();
		filesystem::remove(mergeTarget, ec);
		if (ec) throw runtime_error("Failed to replace original target archive.");
		filesystem::rename(rebuilt, mergeTarget, ec);
		if (ec) {
			ec.clear();
			filesystem::copy_file(
				rebuilt,
				mergeTarget,
				filesystem::copy_options::overwrite_existing,
				ec);
			if (ec) throw runtime_error("Failed to deploy rebuilt target archive.");
			filesystem::remove(rebuilt, ec);
		}
		targetReplaced = true;

		if (FolderRewindFormat::IsFullLikeBackupType(entry.backupType)
			|| FolderRewindFormat::IsFullLikeBackupType(entry.backupFile)) {
			BACKUP_INFO(L("LOG_SAFE_DELETE_STEP_3"));
			finalType = L"Full";
			finalName = next.backupFile;
			const size_t smartMarker = finalName.find(L"[Smart]");
			if (smartMarker != wstring::npos) finalName.replace(smartMarker, 7, L"[Full]");
			const filesystem::path renamed = backupDirectory / finalName;
			if (renamed != mergeTarget) {
				if (filesystem::exists(renamed)) {
					throw runtime_error(
						"Cannot rename merged archive because destination filename already exists.");
				}
				filesystem::rename(mergeTarget, renamed);
				finalArchive = renamed;
				BACKUP_INFO(L("LOG_SAFE_DELETE_RENAMED"), wstring_to_utf8(finalName).c_str());
			}
		}
		else {
			BACKUP_INFO(L("LOG_SAFE_DELETE_STEP_3_SKIP"));
		}

		ec.clear();
		filesystem::last_write_time(finalArchive, originalModificationTime, ec);
		if (ec) {
			BACKUP_WARNING(
				"Failed to preserve merged archive timestamp: %s",
				ec.message().c_str());
		}
		BACKUP_INFO(L("LOG_SAFE_DELETE_STEP_4"));
		filesystem::remove(archiveToDelete);
		erase_if(historyEntries, [&](const HistoryEntry& value) {
			return value.worldName == entry.worldName
				&& value.backupFile == entry.backupFile;
		});
		for (auto& value : historyEntries) {
			if (value.worldName == next.worldName && value.backupFile == next.backupFile) {
				value.backupFile = finalName;
				value.backupType = finalType;
				break;
			}
		}
		(void)ReplaceHistoryEntriesForConfig(configIndex, std::move(historyEntries));
		QueueConfigurationHistorySyncAfterLocalChange(config, configIndex, "safe delete");

		string metadataError;
		if (!TryRepairMetadataAfterSafeDelete(
				config,
				entry.worldName,
				entry.backupFile,
				next.backupFile,
				finalName,
				finalType,
				metadataError)) {
			BACKUP_WARNING(
				"Failed to repair metadata after safe-delete (%s). Falling back to metadata invalidation.",
				metadataError.c_str());
			InvalidateBackupMetadata(
				config,
				entry.worldName,
				entry.backupFile,
				next.backupFile,
				finalName);
		}
		filesystem::remove_all(tempRoot, ec);
		BACKUP_INFO(L("LOG_SAFE_DELETE_SUCCESS"));
	}
	catch (const exception& error) {
		if (targetReplaced) {
			error_code restoreError;
			filesystem::remove(finalArchive, restoreError);
			if (finalArchive != mergeTarget) filesystem::remove(mergeTarget, restoreError);
			restoreError.clear();
			filesystem::copy_file(
				originalTarget,
				mergeTarget,
				filesystem::copy_options::overwrite_existing,
				restoreError);
			if (restoreError) {
				BACKUP_WARNING(
					"Failed to restore original archive after safe-delete failure: %s",
					restoreError.message().c_str());
			}
			else {
				filesystem::last_write_time(
					mergeTarget,
					originalModificationTime,
					restoreError);
			}
		}
		BACKUP_ERROR(L("LOG_SAFE_DELETE_FATAL_ERROR"), error.what());
		error_code ec;
		filesystem::remove_all(tempRoot, ec);
	}
}
