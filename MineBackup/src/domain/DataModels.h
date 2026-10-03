#pragma once
#ifndef DATA_MODELS_H
#define DATA_MODELS_H

// 核心数据模型定义：Config、HistoryEntry 等
// 所有业务数据结构集中定义在此处

#include <chrono>
#include <vector>
#include <string>
#include <map>
#include <ctime>

namespace BackupPolicy {
inline constexpr int MinimumMode = 0;
inline constexpr int MaximumMode = 3;
inline constexpr int MinimumSmartCount = 0; // unlimited
inline constexpr int MaximumSmartCount = 100000;
constexpr bool IsValidMode(int value) { return value >= MinimumMode && value <= MaximumMode; }
constexpr bool IsValidSmartCount(int value) { return value >= MinimumSmartCount && value <= MaximumSmartCount; }
constexpr bool IsValid(int mode, int smartCount) { return IsValidMode(mode) && IsValidSmartCount(smartCount); }
}

// 结构体们
struct Config {
    bool operator==(const Config&) const = default;
	std::wstring saveRoot;
	std::vector<std::pair<std::wstring, std::wstring>> worlds; // {name, desc}
	std::wstring backupPath;
	std::wstring zipPath;
	std::wstring zipFormat = L"7z";
	std::wstring fontPath;
	std::wstring zipMethod = L"LZMA2";
	int backupMode = 1;
	int zipLevel = 5;
	int keepCount = 0;
	bool backupBefore = false;
	int theme = 1;
	std::string name;
	std::wstring configId;
	// Runtime-only: the loaded 1.15 configuration did not persist ConfigId.
	bool legacyConfigIdGenerated = false;
	// Imported portable profiles cannot run destructive or automated work until local paths are bound.
	bool pendingLocalBinding = false;
	int cpuThreads = 0;
	bool useLowPriority = false;
	bool skipIfUnchanged = true;
	int maxSmartBackupsPerFull = 5;
	bool backupOnGameStart = false;
	std::vector<std::wstring> blacklist;
	bool cloudSyncEnabled = false;
	std::wstring rclonePath;
	std::wstring rcloneRemotePath;
	int cloudSyncMode = 0; // 0: 仅同步历史, 1: 同步历史和备份包
	std::wstring cloudWorkingDirectory;
	int cloudTimeoutSeconds = 600;
	int cloudRetryCount = 0;
	bool cloudSyncHistoryAfterUpload = true;
	bool cloudAutoDownloadBeforeRestore = true;
	std::wstring cloudLastRunUtc;
	int cloudLastExitCode = 0;
	std::wstring cloudLastErrorMessage;
	std::wstring snapshotPath;
	std::wstring othersPath;
	bool enableWEIntegration = false;
	std::wstring weSnapshotPath = L"";
};

enum class CloudSyncMode {
	HistoryOnly = 0,
	HistoryAndBackups = 1
};

struct CloudCommandResult {
	bool success = false;
	int exitCode = -1;
	bool timedOut = false;
	std::wstring message;
	std::wstring detail;
};

struct HistoryEntry {
	std::wstring configId;
	std::wstring timestamp_str;
	std::wstring worldPath;
	std::wstring worldName;
	std::wstring backupFile;
	std::wstring backupType;
	bool isPartialBackup = false;
	std::wstring comment;
	bool isImportant = false;
	bool isCloudArchived = false;
	std::wstring cloudArchivedAtUtc;
	std::wstring cloudArchiveRemotePath;
	std::wstring cloudMetadataRecordRemotePath;
	std::wstring cloudMetadataStateRemotePath;
};

enum class MigrationStatus {
	NotNeeded = 0,
	Pending,
	Succeeded,
	Degraded,
	Failed
};

struct MigrationUnitResult {
	std::wstring unitId;
	MigrationStatus status = MigrationStatus::NotNeeded;
	std::wstring message;
	std::wstring snapshotPath;
	int migratedItems = 0;
	int skippedItems = 0;
};

struct MigrationReport {
	MigrationStatus status = MigrationStatus::NotNeeded;
	std::wstring updatedAtUtc;
	std::vector<MigrationUnitResult> units;
};

struct CloudHistoryAnalysisResult {
	bool success = false;
	std::wstring message;
	int totalRemoteEntries = 0;
	int matchedEntries = 0;
	int importableEntries = 0;
	int unmappedEntries = 0;
	int ambiguousEntries = 0;
	std::vector<HistoryEntry> mappedItems;
};

struct CloudDownloadFailure {
    std::wstring configId, worldPath, worldName, backupFile;
    int exitCode = -1;
    bool timedOut = false;
    std::wstring error;
};
struct CloudSyncResult {
	bool success = false;
	std::wstring message;
	int importedHistoryCount = 0;
	int duplicateHistoryCount = 0;
	int recoveredBackupCount = 0;
    int failedDownloadCount = 0;
    int exitCode = -1;
    std::vector<CloudDownloadFailure> downloadFailures;
	CloudHistoryAnalysisResult analysis;
};

struct CloudActiveHistoryEntry {
	std::wstring folderPath;
	std::wstring folderName;
	std::wstring fileName;
	std::wstring timestamp;
	std::wstring worldPath;
	std::wstring worldName;
	std::wstring backupFile;
};

struct CloudActiveHistoryManifest {
	std::wstring configId;
	std::wstring configName;
	std::wstring updatedAtUtc;
	std::vector<CloudActiveHistoryEntry> entries;
};

#endif // DATA_MODELS_H
