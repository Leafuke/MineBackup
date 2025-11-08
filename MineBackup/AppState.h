#pragma once
#ifndef APP_STATE_H
#define APP_STATE_H
#include <vector>
#include <string>
#include <map>
#include <atomic>
#include <iostream>
#include <thread>
#include <mutex>
#ifndef CONSTANT1
#define CONSTANT1 256
#define CONSTANT2 512
#define MINEBACKUP_HOTKEY_ID 1
#define MINERESTORE_HOTKEY_ID 2
#endif

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
	bool hotBackup = false;
	bool backupBefore = false;
	int theme = 1;
	int folderNameType = 0;
	std::string name;
	int cpuThreads = 0; // 0 for auto/default
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
	int schedMonth = 0, schedDay = 0, schedHour = 0, schedMinute = 0; // 0 意味着“每一”
};
struct SpecialConfig {
	bool autoExecute = false;
	std::vector<std::wstring> commands;
	std::vector<AutomatedTask> tasks; // REPLACED: a more capable task structure
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

enum class HotRestoreState {
	IDLE,              // 空闲状态
	WAITING_FOR_MOD,   // 已发送请求，正在等待模组响应
	RESTORING,         // 模组已响应，正在执行还原
};

//struct WorldStateCache {
//	std::wstring lastOpenTime;
//	std::wstring lastBackupTime;
//	bool needsBackup;
//	std::filesystem::file_time_type lastChecked;
//};

struct AppState {

	bool done = false;

    // UI State
    bool showMainApp = false;
	bool specialConfigMode = false; // 用来开启简单UI


    // Data
	int currentConfigIndex = 1, realConfigIndex = -1; // 如果realConfigIndex不为-1，说明是特殊配置
	std::map<int, Config> configs;
	std::map<int, std::vector<HistoryEntry>> g_history;
	std::map<int, SpecialConfig> specialConfigs;

	std::map<std::pair<int, int>, AutoBackupTask> g_active_auto_backups; // Key: {configIdx, worldIdx}
	//std::map<std::pair<int, int>, WorldStateCache> worldCache; // Key: {configIdx, worldIdx}



	std::mutex configsMutex;			// 用于保护全局配置的互斥锁
	std::mutex task_mutex;		// 专门用于保护 g_active_auto_backups
	bool isRespond = false;
	std::atomic<HotRestoreState> hotkeyRestoreState = HotRestoreState::IDLE;
    // Settings
    bool g_CheckForUpdates = true;
};

// Declare a single global instance
extern AppState g_appState;
#endif