#pragma once
#ifndef _CLOUD_SYNC_SERVICE_H
#define _CLOUD_SYNC_SERVICE_H

#include "AppState.h"
#include "Console.h"

bool CanUseCloudActions(const Config& config);
bool HasHistoryCloudCopy(const HistoryEntry& entry);
bool HasLocalBackupOrMetadata(const Config& config, const HistoryEntry& entry);
bool QueueConfigurationHistorySyncAfterLocalChange(const Config& config, int configIndex, const char* reason, Console& console);
bool QueueUploadAfterBackup(const Config& config, int configIndex, const MyFolder& folder, const std::wstring& archiveFile, const std::wstring& comment, Console& console);
CloudHistoryAnalysisResult AnalyzeCloudHistory(const Config& config, int configIndex, Console& console);
CloudSyncResult SyncConfigFromCloud(const Config& config, int configIndex, CloudSyncMode mode, Console& console);
CloudCommandResult UploadHistoryEntry(const Config& config, int configIndex, const HistoryEntry& entry, Console& console);
CloudCommandResult DownloadHistoryEntry(const Config& config, int configIndex, const HistoryEntry& entry, Console& console);
CloudCommandResult UploadWorldBackupFolderToCloud(const Config& config, int configIndex, const std::wstring& worldName, Console& console);
bool EnsureRestoreChainAvailable(const Config& config, int configIndex, const HistoryEntry& targetEntry, Console& console);
CloudCommandResult UploadConfigurationHistorySnapshot(const Config& config, int configIndex, Console& console);
CloudCommandResult ExportConfigToCloud(const Config& config, Console& console);
CloudCommandResult ImportConfigFromCloud(const Config& config, Console& console);
CloudCommandResult ExportHistoryToCloud(const Config& config, int configIndex, Console& console);
CloudCommandResult ImportHistoryFromCloud(const Config& config, int configIndex, bool mergeExisting, Console& console);
int ResolveConfigIndexForCloud(const Config& config);

#endif // _CLOUD_SYNC_SERVICE_H
