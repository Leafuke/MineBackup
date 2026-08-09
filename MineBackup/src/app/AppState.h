#pragma once
#ifndef APP_STATE_H
#define APP_STATE_H

// AppState: 全局应用状态
// 数据模型定义在 DataModels.h，跨平台兼容层在 PlatformCompat.h

#include "DataModels.h"
#include "JobModels.h"
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

struct CloudTaskRuntimeState {
	std::atomic<bool> busy{ false };
	std::atomic<int> progress{ 0 };
	int activeConfigIndex = -1;
	std::wstring statusText;
	std::wstring lastMessage;
	std::mutex mutex;
};

struct AppState {

	bool done = false;

	// UI State
	bool showMainApp = false;


	// Data
	int currentConfigIndex = 1;
	std::map<int, Config> configs;
	JobDocument jobs;

	std::map<std::pair<int, int>, AutoBackupTask> g_active_auto_backups; // Key: {configIdx, worldIdx}

	std::mutex configsMutex;			// 用于保护全局配置的互斥锁
	std::mutex task_mutex;		// 专门用于保护 g_active_auto_backups
	bool isRespond = false;
	std::atomic<HotRestoreState> hotkeyRestoreState = HotRestoreState::IDLE;

	KnotLinkModInfo knotLinkMod;
	CloudTaskRuntimeState cloudTask;
};

extern AppState g_appState;
#endif
