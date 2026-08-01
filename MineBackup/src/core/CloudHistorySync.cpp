#include "CloudSyncService.h"
#include "CloudSyncInternal.h"
#include "CloudHistoryAnalysis.h"

#include "AppPaths.h"
#include "FolderRewindFormat.h"
#include "HistoryManager.h"
#include "MigrationCoordinator.h"
#include "TaskCoordinator.h"
#include "i18n.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <vector>

using namespace std;
using namespace CloudSyncInternal;

CloudHistoryAnalysisResult AnalyzeCloudHistory(const Config& config, int configIndex) {
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_ANALYZING")));

	CloudCommandResult downloadResult;
	vector<HistoryEntry> remoteEntries = LoadRemoteHistoryEntriesNoLock(config, configIndex, downloadResult);
	if (!downloadResult.success) {
		CloudHistoryAnalysisResult analysis;
		analysis.success = false;
		analysis.message = downloadResult.message;
		operation.Finish(downloadResult);
		return analysis;
	}

	CloudActiveHistoryManifest activeManifest;
	CloudCommandResult manifestResult;
	const ActiveManifestLoadStatus manifestStatus = TryLoadActiveManifestNoLock(config, configIndex, activeManifest, manifestResult);
	if (manifestStatus == ActiveManifestLoadStatus::Failed) {
		CloudHistoryAnalysisResult analysis;
		analysis.success = false;
		analysis.message = manifestResult.message;
		operation.Finish(manifestResult);
		return analysis;
	}
	const optional<CloudActiveHistoryManifest> manifest =
		manifestStatus == ActiveManifestLoadStatus::Loaded
			? optional<CloudActiveHistoryManifest>(std::move(activeManifest))
			: nullopt;
	CloudHistoryAnalysisResult analysis = AnalyzeRemoteHistory(
		config,
		GetHistoryEntriesForConfig(configIndex),
		remoteEntries,
		manifest);
	analysis.message = MineFormatMessage(
		"CLOUD_ANALYSIS_SUMMARY",
		analysis.totalRemoteEntries,
		analysis.matchedEntries,
		analysis.importableEntries,
		analysis.unmappedEntries,
		analysis.ambiguousEntries);

	CloudCommandResult result;
	result.success = true;
	result.exitCode = 0;
	result.message = analysis.message;
	operation.Finish(result);
	return analysis;
}

CloudSyncResult SyncConfigFromCloud(const Config& config, int configIndex, CloudSyncMode mode) {
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_SYNCING")));

	CloudSyncResult syncResult;
	CloudCommandResult downloadResult;
	vector<HistoryEntry> remoteEntries = LoadRemoteHistoryEntriesNoLock(config, configIndex, downloadResult);

	if (!downloadResult.success) {
		syncResult.success = false;
		syncResult.message = downloadResult.message;
		operation.Finish(downloadResult);
		return syncResult;
	}

	CloudActiveHistoryManifest activeManifest;
	CloudCommandResult manifestResult;
	const ActiveManifestLoadStatus manifestStatus =
		TryLoadActiveManifestNoLock(config, configIndex, activeManifest, manifestResult);
	if (manifestStatus == ActiveManifestLoadStatus::Failed) {
		syncResult.success = false;
		syncResult.message = manifestResult.message;
		operation.Finish(manifestResult);
		return syncResult;
	}

	// 同步必须复用同一份远端快照，避免公开分析入口再次下载历史和活动清单。
	const optional<CloudActiveHistoryManifest> manifest =
		manifestStatus == ActiveManifestLoadStatus::Loaded
			? optional<CloudActiveHistoryManifest>(std::move(activeManifest))
			: nullopt;
	syncResult.analysis = AnalyzeRemoteHistory(
		config,
		GetHistoryEntriesForConfig(configIndex),
		remoteEntries,
		manifest);
	syncResult.analysis.message = MineFormatMessage(
		"CLOUD_ANALYSIS_SUMMARY",
		syncResult.analysis.totalRemoteEntries,
		syncResult.analysis.matchedEntries,
		syncResult.analysis.importableEntries,
		syncResult.analysis.unmappedEntries,
		syncResult.analysis.ambiguousEntries);
	if (!syncResult.analysis.success) {
		syncResult.success = false;
		syncResult.message = syncResult.analysis.message;
		operation.Finish(syncResult.message, false, false);
		return syncResult;
	}

	int duplicates = 0;
	int imported = 0;
	for (const auto& entry : syncResult.analysis.mappedItems) {
		if (UpsertHistoryEntry(configIndex, entry, false)) {
			imported++;
		}
		else {
			duplicates++;
		}
	}
	if (imported > 0) {
		SaveHistory();
	}

	int recoveredCount = 0;
	if (mode == CloudSyncMode::HistoryAndBackups) {
		for (const auto& entry : syncResult.analysis.mappedItems) {
			if (HasLocalBackupOrMetadata(config, entry)) {
				continue;
			}
			CloudCommandResult itemResult = DownloadHistoryEntryNoLock(config, configIndex, entry);
			if (itemResult.success) {
				recoveredCount++;
			}
		}
	}

	syncResult.success = true;
	syncResult.importedHistoryCount = imported;
	syncResult.duplicateHistoryCount = duplicates;
	syncResult.recoveredBackupCount = recoveredCount;
	syncResult.message = (mode == CloudSyncMode::HistoryAndBackups)
		? MineFormatMessage("CLOUD_SYNC_DOWNLOADED_SUMMARY", imported, duplicates, recoveredCount)
		: MineFormatMessage("CLOUD_SYNC_HISTORY_SUMMARY", imported, duplicates);

	CloudCommandResult result;
	result.success = true;
	result.exitCode = 0;
	result.message = syncResult.message;
	operation.Finish(result);
	return syncResult;
}

CloudCommandResult UploadHistoryEntry(const Config& config, int configIndex, const HistoryEntry& entry) {
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_PREPARING")));
	const MigrationUnitResult localMigration = MigrationCoordinator::EnsureWorldMigrated(config, configIndex, entry.worldName, entry.worldPath);
	if (localMigration.status == MigrationStatus::Failed || localMigration.status == MigrationStatus::Degraded) {
		CloudCommandResult blocked;
		blocked.success = false;
		blocked.message = L"Cloud upload requires complete local metadata migration: " + localMigration.message;
		operation.Finish(blocked.message, false, false);

		return blocked;
	}
	CloudCommandResult result = UploadHistoryEntryNoLock(config, configIndex, entry);
	operation.Finish(result);
	return result;
}

CloudCommandResult DownloadHistoryEntry(const Config& config, int configIndex, const HistoryEntry& entry) {
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_PREPARING")));
	CloudCommandResult result = DownloadHistoryEntryNoLock(config, configIndex, entry);
	operation.Finish(result);
	return result;
}

CloudCommandResult UploadWorldBackupFolderToCloud(const Config& config, int configIndex, const wstring& worldName) {
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_UPLOADING_ARCHIVE")));

	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		operation.Finish(configError);
		return configError;
	}

	const FolderRewindFormat::StoragePaths storagePaths = ResolveStoragePaths(config, worldName, (filesystem::path(config.saveRoot) / worldName).wstring());
	const filesystem::path backupDir = storagePaths.backupSubDir;
	if (!filesystem::exists(backupDir)) {
		CloudCommandResult result = MakeConfigErrorResult("CLOUD_LOCAL_ARCHIVE_MISSING", backupDir.wstring());
		operation.Finish(result);
		return result;
	}

	const wstring remoteWorldRoot = FolderRewindFormat::AppendRemotePath(
		FolderRewindFormat::BuildConfigCloudRoot(config),
		{ storagePaths.folderName });
	CloudCommandResult result = ExecuteCommandWithRetry(
		config,
		configIndex,
		BuildRcloneCopyCommand(config, backupDir.wstring(), remoteWorldRoot),
		"CLOUD_STATUS_UPLOADING_ARCHIVE",
		40);
	if (!result.success) {
		result.message = MineFormatMessage("CLOUD_UPLOAD_FOLDER_FAILED", wstring_to_utf8(worldName).c_str());
		operation.Finish(result);
		return result;
	}

	wstring warningMessage;
	const filesystem::path metadataDir = storagePaths.metadataDir;
	if (filesystem::exists(metadataDir)) {
		CloudCommandResult metadataResult = ExecuteCommandWithRetry(
			config,
			configIndex,
			BuildRcloneCopyCommand(config, metadataDir.wstring(), FolderRewindFormat::AppendRemotePath(remoteWorldRoot, { FolderRewindFormat::kMetadataRootDirName })),
			"CLOUD_STATUS_UPLOADING_METADATA",
			70);
		if (!metadataResult.success) {
			warningMessage = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
		}
	}

	// 目录上传后，逐条标记已有本地文件的云端路径，后续即使只删本地文件也能从云端找回。
	for (const auto& entry : GetHistoryEntriesForWorld(configIndex, worldName)) {
		HistoryCloudPaths paths = BuildHistoryPaths(config, entry);
		if (!filesystem::exists(paths.archiveLocalPath)) continue;
		UpdateHistoryCloudState(
			configIndex,
			entry.worldName,
			entry.backupFile,
			true,
			FolderRewindFormat::MakeUtcTimestampString(),
			paths.archiveRemotePath,
			paths.metadataRecordRemotePath,
			paths.metadataStateRemotePath);
	}

	CloudCommandResult historyResult = UploadConfigurationHistorySnapshotNoLock(config, configIndex);
	if (!historyResult.success) {
		warningMessage = historyResult.message;
	}

	result.success = true;
	result.exitCode = 0;
	result.message = warningMessage.empty()
		? MineFormatMessage("CLOUD_UPLOAD_FOLDER_SUCCEEDED", wstring_to_utf8(worldName).c_str())
		: warningMessage;
	operation.Finish(result);
	return result;
}

bool EnsureRestoreChainAvailable(const Config& config, int configIndex, const HistoryEntry& targetEntry) {
	if (!config.cloudAutoDownloadBeforeRestore || !CanUseCloudActions(config)) {
		return false;
	}

	unique_lock<mutex> lock(g_cloudMutex);

	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_ARCHIVE")));

	CloudCommandResult remoteLoadResult;
	vector<HistoryEntry> remoteEntries = LoadRemoteHistoryEntriesNoLock(config, configIndex, remoteLoadResult);
	if (remoteLoadResult.success) {
		CloudActiveHistoryManifest activeManifest;
		CloudCommandResult manifestResult;
		const ActiveManifestLoadStatus manifestStatus = TryLoadActiveManifestNoLock(config, configIndex, activeManifest, manifestResult);
		if (manifestStatus == ActiveManifestLoadStatus::Failed) {
			operation.Finish(manifestResult);
			return false;
		}
		const bool hasActiveManifest = manifestStatus == ActiveManifestLoadStatus::Loaded;
		for (const auto& remoteEntry : remoteEntries) {
			if (!BelongsToConfiguration(config, remoteEntry)) continue;
			if (hasActiveManifest && !ManifestContainsHistoryItem(activeManifest, remoteEntry)) continue;
			if (remoteEntry.worldName == targetEntry.worldName) {
				UpsertHistoryEntry(configIndex, remoteEntry, false);
			}
		}
		SaveHistory();
	}

	vector<HistoryEntry> worldEntries = GetHistoryEntriesForWorld(configIndex, targetEntry.worldName);
	if (worldEntries.empty()) {
		operation.Finish(utf8_to_wstring(L("CLOUD_RESTORE_CHAIN_SKIPPED")), false, false);
		return false;
	}

	auto it = find_if(worldEntries.begin(), worldEntries.end(), [&](const HistoryEntry& entry) {
		return entry.backupFile == targetEntry.backupFile;
	});
	if (it == worldEntries.end()) {
		operation.Finish(utf8_to_wstring(L("CLOUD_RESTORE_CHAIN_SKIPPED")), false, false);
		return false;
	}

	vector<HistoryEntry> requiredEntries;
	for (auto rit = make_reverse_iterator(it + 1); rit != worldEntries.rend(); ++rit) {
		requiredEntries.push_back(*rit);
		if (IsFullLikeBackupType(rit->backupType) || IsFullLikeBackupType(rit->backupFile)) {
			break;
		}
	}
	reverse(requiredEntries.begin(), requiredEntries.end());

	bool downloadedAny = false;
	for (const auto& entry : requiredEntries) {
		if (HasLocalBackupOrMetadata(config, entry)) {
			continue;
		}
		CloudCommandResult result = DownloadHistoryEntryNoLock(config, configIndex, entry);
		if (!result.success) {
			operation.Finish(result);
			return false;
		}
		downloadedAny = true;
	}

	CloudCommandResult result;
	result.success = true;
	result.exitCode = 0;
	result.message = downloadedAny
		? utf8_to_wstring(L("CLOUD_RESTORE_CHAIN_READY"))
		: utf8_to_wstring(L("CLOUD_RESTORE_CHAIN_ALREADY_READY"));
	operation.Finish(result);
	return true;
}

CloudCommandResult UploadConfigurationHistorySnapshot(const Config& config, int configIndex) {
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_UPLOADING_HISTORY")));
	CloudCommandResult result = UploadConfigurationHistorySnapshotNoLock(config, configIndex);

	operation.Finish(result);
	return result;
}

CloudCommandResult ExportHistoryToCloud(const Config& config, int configIndex) {
	return UploadConfigurationHistorySnapshot(config, configIndex);
}

CloudCommandResult ImportHistoryFromCloud(const Config& config, int configIndex, bool mergeExisting) {
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_HISTORY")));

	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		operation.Finish(configError);
		return configError;
	}

	const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_cloud_history_import", L".json");
	CloudCommandResult result = ExecuteCommandWithRetry(
		config,
		configIndex,
		BuildRcloneCopyToCommand(config, FolderRewindFormat::BuildGlobalHistoryRemotePath(config), tempPath.wstring()),
		"CLOUD_STATUS_DOWNLOADING_HISTORY",
		70);

	if (result.success) {
		vector<HistoryEntry> remoteEntries;
		try {
			ifstream in(tempPath, ios::binary);
			nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
			if (!TryParseCloudHistoryArray(root, remoteEntries)) {
				result.success = false;
			}
		}
		catch (...) {
			result.success = false;
		}

		if (result.success) {
			if (!mergeExisting) {
				g_appState.g_history[configIndex].clear();
			}
			bool changed = false;
			for (const auto& entry : remoteEntries) {
				if (!BelongsToConfiguration(config, entry)) {
					continue;
				}
				changed = UpsertHistoryEntry(configIndex, entry, false) || changed;
			}
			if (changed || !mergeExisting) {
				SaveHistory();
			}
			result.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_SUCCEEDED"));
		}
		else {
			result.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_FAILED"));
		}
	}
	else {
		result.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_FAILED"));
	}

	error_code ec;
	filesystem::remove(tempPath, ec);
	operation.Finish(result);
	return result;
}

