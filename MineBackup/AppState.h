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

// �ṹ����
struct Config {
	int backupMode;
	std::wstring saveRoot;
	std::vector<std::pair<std::wstring, std::wstring>> worlds; // {name, desc}
	std::wstring backupPath;
	std::wstring zipPath;
	std::wstring zipFormat = L"7z";
	std::wstring fontPath;
	std::wstring zipMethod = L"LZMA2";
	int zipLevel;
	int keepCount;
	bool hotBackup;
	bool backupBefore;
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
};
struct AutomatedTask {
	int configIndex = -1;
	int worldIndex = -1;
	int backupType = 0; // 0: ����, 1: ���, 2: �ƻ�
	int intervalMinutes = 15;
	int schedMonth = 0, schedDay = 0, schedHour = 0, schedMinute = 0; // 0 ��ζ�š�ÿһ��
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
	std::atomic<bool> stop_flag{ false }; // ԭ�Ӳ���ֵ�����ڰ�ȫ��֪ͨ�߳�ֹͣ
};
struct DisplayWorld { // һ���µĽṹ�壬�� UI ����ֱ�Ӷ�ȡ configs[currentConfigIndex].worlds����ʹ�� DisplayWorld
	std::wstring name;      // ���������ļ�������
	std::wstring desc;      // ����
	int baseConfigIndex = -1; // ��Դ���� id
	int baseWorldIndex = -1;  // ��Դ��������������
	Config effectiveConfig;   // �ϲ�������ã�������
};

struct WorldStateCache {
	wstring lastOpenTime;
	wstring lastBackupTime;
	bool needsBackup;
	filesystem::file_time_type lastChecked;
};

struct AppState {

	bool done = false;

    // UI State
    bool showMainApp = false;
    bool showSettings = false;
    bool showHistoryWindow = false;
	bool specialConfigMode = false; // ����������UI


    // Data
	int currentConfigIndex = 1, realConfigIndex = -1; // ���realConfigIndex��Ϊ-1��˵������������
	std::map<int, Config> configs;
	std::map<int, std::vector<HistoryEntry>> g_history;
	std::map<int, SpecialConfig> specialConfigs;

	std::map<std::pair<int, int>, AutoBackupTask> g_active_auto_backups; // Key: {configIdx, worldIdx}
	std::map<std::pair<int, int>, WorldStateCache> worldCache; // Key: {configIdx, worldIdx}



	std::mutex configsMutex;			// ���ڱ���ȫ�����õĻ�����
	std::mutex task_mutex;		// ר�����ڱ��� g_active_auto_backups
	bool isRespond = false;
    // Settings
    bool g_CheckForUpdates = true;
};

// Declare a single global instance
extern AppState g_appState;
#endif