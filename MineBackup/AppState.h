#pragma once
#ifndef APP_STATE_H
#define APP_STATE_H

// AppState: 全局应用状态
// 数据模型定义在 DataModels.h，跨平台兼容层在 PlatformCompat.h

#include "DataModels.h"
#include <vector>
#include <string>
#include <map>
#include <atomic>
#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>
#include <ctime>
#include <sys/stat.h>

struct AppState {

	bool done = false;

	// UI State
	bool showMainApp = false;
	bool specialConfigMode = false; // 用来开启简单UI


	// Data
	int currentConfigIndex = 1;
	std::atomic<int> realConfigIndex{-1}; // 如果realConfigIndex不为-1，说明是特殊配置
	std::map<int, Config> configs;
	std::map<int, std::vector<HistoryEntry>> g_history;
	std::map<int, SpecialConfig> specialConfigs;

	std::map<std::pair<int, int>, AutoBackupTask> g_active_auto_backups; // Key: {configIdx, worldIdx}

	std::mutex configsMutex;			// 用于保护全局配置的互斥锁
	std::mutex task_mutex;		// 专门用于保护 g_active_auto_backups
	bool isRespond = false;
	std::atomic<HotRestoreState> hotkeyRestoreState = HotRestoreState::IDLE;

	KnotLinkModInfo knotLinkMod;

	// Settings
	bool g_CheckForUpdates = true;
};

// Declare a single global instance
extern AppState g_appState;
#endif
