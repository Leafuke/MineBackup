#pragma once
#ifndef DATA_MODELS_H
#define DATA_MODELS_H

// 核心数据模型定义：Config, SpecialConfig, HistoryEntry, DisplayWorld 等
// 所有业务数据结构集中定义在此处

#include "PlatformCompat.h"
#include <vector>
#include <string>
#include <map>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ctime>

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
#ifdef _WIN32
	bool hotBackup = true; // 1.11.3 之后默认开启热备份 (Windows)
#else
	bool hotBackup = false; // Linux/MacOS 默认关闭热备份，因为游戏不锁定文件
#endif
	bool backupBefore = false;
	int theme = 1;
	int folderNameType = 0;
	std::string name;
	int cpuThreads = 0;
	bool useLowPriority = false;
	bool skipIfUnchanged = true;
	int maxSmartBackupsPerFull = 5;
	bool backupOnGameStart = false;
	std::vector<std::wstring> blacklist;
	bool cloudSyncEnabled = false;
	std::wstring rclonePath;
	std::wstring rcloneRemotePath;
	std::wstring snapshotPath;
	std::wstring othersPath;
	bool enableWEIntegration = false;
	std::wstring weSnapshotPath = L"";
};

struct AutomatedTask {
	int configIndex = -1;
	int worldIndex = -1;
	int backupType = 0; // 0: 单次, 1: 间隔, 2: 计划
	int intervalMinutes = 15;
	int schedMonth = 0, schedDay = 0, schedHour = 0, schedMinute = 0; // 0 意味着"每一"
};

// 新的统一任务系统（v2）
enum class TaskTypeV2 {
	Backup,     // 备份任务
	Command,    // CMD命令任务
	Script      // 脚本任务
};

enum class TaskExecMode {
	Sequential,    // 顺序执行（等待上一个任务完成）
	Parallel       // 并行执行（和上一个任务同时进行）
};

enum class TaskTrigger {
	Once,          // 单次执行
	Interval,      // 间隔执行
	Scheduled      // 计划执行
};

struct UnifiedTaskV2 {
	int id = 0;
	std::string name;
	TaskTypeV2 type = TaskTypeV2::Backup;
	TaskExecMode executionMode = TaskExecMode::Sequential;
	TaskTrigger triggerMode = TaskTrigger::Once;
	bool enabled = true;

	// 备份任务相关
	int configIndex = -1;
	int worldIndex = -1;

	// CMD命令相关
	std::wstring command;
	std::wstring workingDirectory;

	// 计划相关
	int intervalMinutes = 15;
	int schedMonth = 0, schedDay = 0, schedHour = 0, schedMinute = 0;

	// 高级选项
	int retryCount = 0;
	int timeoutMinutes = 0;
	bool notifyOnComplete = false;
	bool notifyOnError = true;
};

// 服务模式配置
struct ServiceConfig {
	bool installAsService = false;
	std::wstring serviceName = L"MineBackupService";
	std::wstring serviceDisplayName = L"MineBackup Auto Backup Service";
	std::wstring serviceDescription = L"Automated backup service for Minecraft worlds";
	bool startWithSystem = true;
	bool delayedStart = false;
};

struct SpecialConfig {
	bool autoExecute = false;
	std::vector<std::wstring> commands;              // 旧版兼容：命令列表
	std::vector<AutomatedTask> tasks;                // 旧版兼容：任务列表
	std::vector<UnifiedTaskV2> unifiedTasks;         // 新版统一任务系统
	bool exitAfterExecution = false;
	std::string name;
	int zipLevel = 5;
	int keepCount = 0;
	int cpuThreads = 0;
	int theme = 1;
	bool useLowPriority = true;
	bool hotBackup = false;
	std::vector<std::wstring> blacklist;
	bool runOnStartup = false;
	bool hideWindow = false;
	bool backupOnGameStart = false;
	
	// Windows服务模式
	ServiceConfig serviceConfig;
	bool useServiceMode = false;                     // 是否使用服务模式
};

struct HistoryEntry {
	std::wstring timestamp_str;
	std::wstring worldName;
	std::wstring backupFile;
	std::wstring backupType;
	std::wstring comment;
	bool isImportant = false;
};

struct AutoBackupTask {
	std::thread worker;
	std::atomic<bool> stop_flag{ false }; // 原子布尔值，用于安全地通知线程停止
};

struct DisplayWorld { // 一个新的结构体，让 UI 不再直接读取 configs[currentConfigIndex].worlds，而使用 DisplayWorld
	std::wstring name;      // 世界名（文件夹名）
	std::wstring desc;      // 描述
	int baseConfigIndex = -1; // 来源配置 id
	int baseWorldIndex = -1;  // 来源配置中世界索引
	Config effectiveConfig;   // 合并后的配置（拷贝）
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
	static constexpr const char* MIN_MOD_VERSION = "1.0.0";

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
		auto parseVersion = [](const std::string& v) -> std::tuple<int, int, int> {
			int major = 0, minor = 0, patch = 0;
			std::sscanf(v.c_str(), "%d.%d.%d", &major, &minor, &patch);
			return { major, minor, patch };
		};
		return parseVersion(current) >= parseVersion(required);
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
