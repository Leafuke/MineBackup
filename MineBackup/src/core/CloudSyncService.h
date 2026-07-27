#pragma once
#ifndef _CLOUD_SYNC_SERVICE_H
#define _CLOUD_SYNC_SERVICE_H

#include "AppState.h"
#include "PortableConfigDocument.h"

#include <map>
#include <string>

struct PortableConfigTransferPreparation {
	CloudCommandResult result;
	std::string payload;
	PortableConfigMergePreview preview;
};

bool CanUseCloudActions(const Config& config);
bool HasHistoryCloudCopy(const HistoryEntry& entry);
bool HasLocalBackupOrMetadata(const Config& config, const HistoryEntry& entry);
bool QueueConfigurationHistorySyncAfterLocalChange(const Config& config, int configIndex, const char* reason);
bool QueueUploadAfterBackup(const Config& config, int configIndex, const MyFolder& folder, const std::wstring& archiveFile, const std::wstring& comment);
CloudHistoryAnalysisResult AnalyzeCloudHistory(const Config& config, int configIndex);
CloudSyncResult SyncConfigFromCloud(const Config& config, int configIndex, CloudSyncMode mode);
CloudCommandResult UploadHistoryEntry(const Config& config, int configIndex, const HistoryEntry& entry);
CloudCommandResult DownloadHistoryEntry(const Config& config, int configIndex, const HistoryEntry& entry);
CloudCommandResult UploadWorldBackupFolderToCloud(const Config& config, int configIndex, const std::wstring& worldName);
bool EnsureRestoreChainAvailable(const Config& config, int configIndex, const HistoryEntry& targetEntry);
CloudCommandResult UploadConfigurationHistorySnapshot(const Config& config, int configIndex);
PortableConfigTransferPreparation PreparePortableConfigUpload(
	const Config& cloudConfig,
	const std::map<int, Config>& localConfigs);
PortableConfigTransferPreparation PreparePortableConfigImport(
	const Config& cloudConfig,
	const std::map<int, Config>& localConfigs);
#if MINEBACKUP_ENABLE_V15_MIGRATION
PortableConfigTransferPreparation PrepareLegacyPortableConfigImport(
	const Config& cloudConfig,
	const std::map<int, Config>& localConfigs);
#endif
CloudCommandResult CommitPortableConfigUpload(
	const Config& cloudConfig,
	const std::string& payload);
CloudCommandResult ExportHistoryToCloud(const Config& config, int configIndex);
CloudCommandResult ImportHistoryFromCloud(const Config& config, int configIndex, bool mergeExisting);
int ResolveConfigIndexForCloud(const Config& config);

#endif // _CLOUD_SYNC_SERVICE_H
