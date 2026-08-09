#pragma once
#ifndef DATA_MODELS_H
#define DATA_MODELS_H

// 核心数据模型定义：Config、HistoryEntry 等
// 所有业务数据结构集中定义在此处

#include <chrono>
#include <cstdio>
#include <vector>
#include <string>
#include <map>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <ctime>
#include <tuple>

// 结构体们
struct Config {
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

struct CloudSyncResult {
	bool success = false;
	std::wstring message;
	int importedHistoryCount = 0;
	int duplicateHistoryCount = 0;
	int recoveredBackupCount = 0;
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

struct AutoBackupTask {
	std::wstring taskName;
};

struct MyFolder {
	std::wstring path;		// 世界文件夹路径
	std::wstring name;		// 世界名（文件夹名）
	std::wstring desc;		// 描述
	Config config;			// 所属配置
	int configIndex = -1;	// 所属配置索引
	int worldIndex = -1;	// 世界索引
};

enum class HotRestoreState {
	IDLE,              // 空闲状态
	WAITING_FOR_MOD,   // 已发送请求，正在等待模组响应
	RESTORING,         // 模组已响应，正在执行还原
};

// KnotLink 联动模组状态信息
// 用于跟踪联动模组的检测结果和通信状态
struct KnotLinkModInfo {
	std::atomic<bool> modDetected{false};       // 是否检测到联动模组
	std::string modVersion;                      // 模组版本号
	std::atomic<bool> versionCompatible{false}; // 模组版本是否兼容

	// 最低要求的模组版本号
	static constexpr const char* MIN_MOD_VERSION = "3.0.0";

	// 异步响应同步机制
	std::mutex mtx;
	std::condition_variable cv;

	// 响应标志 (受 mtx 保护)
	bool handshakeReceived = false;            // 收到握手响应
	bool worldSaveComplete = false;            // 模组已完成世界保存 (用于热备份)
	bool worldSaveAndExitComplete = false;     // 模组已完成世界保存并退出 (用于热还原)
	bool rejoinResponseReceived = false;       // 收到重进世界结果
	bool rejoinSuccess = false;                // 重进世界是否成功

	// 重置单次操作的标志 (在每次操作前调用)
	void resetForOperation() {
		std::lock_guard<std::mutex> lock(mtx);
		handshakeReceived = false;
		worldSaveComplete = false;
		worldSaveAndExitComplete = false;
		rejoinResponseReceived = false;
		rejoinSuccess = false;
	}

	// 完全重置
	void resetDetection() {
		modDetected = false;
		modVersion.clear();
		versionCompatible = false;
		resetForOperation();
	}

	// 版本比较
	static bool IsVersionCompatible(const std::string& current, const std::string& required) {
		auto parseVersion = [](const std::string& value, std::tuple<int, int, int>& parsed) {
			int major = 0;
			int minor = 0;
			int patch = 0;
			char trailing = '\0';
			if (std::sscanf(value.c_str(), "%d.%d.%d%c", &major, &minor, &patch, &trailing) != 3 ||
				major < 0 || minor < 0 || patch < 0) {
				return false;
			}
			parsed = { major, minor, patch };
			return true;
		};
		std::tuple<int, int, int> currentVersion;
		std::tuple<int, int, int> requiredVersion;
		return parseVersion(current, currentVersion) &&
			parseVersion(required, requiredVersion) &&
			currentVersion >= requiredVersion;
	}

	// 通知指定标志并唤醒等待线程
	void notifyFlag(bool KnotLinkModInfo::* flag, bool value = true) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			this->*flag = value;
		}
		cv.notify_all();
	}

	// 等待指定标志变为 true，带超时
	bool waitForFlag(bool KnotLinkModInfo::* flag, std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> lock(mtx);
		return cv.wait_for(lock, timeout, [this, flag]() { return this->*flag; });
	}
};

#endif // DATA_MODELS_H
