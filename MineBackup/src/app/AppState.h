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
#include <functional>
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

struct ConfigStateSnapshot {
    std::map<int, Config> configs;
    int selectedIndex = 1;
};
ConfigStateSnapshot SnapshotConfigState();
bool ModifyConfigById(const std::wstring& id, const std::function<void(Config&)>& mutation);
bool DeleteConfigById(const std::wstring& id);
int SelectedConfigIndex();
void SelectConfigIndex(int index);

// One UI-thread scope per frame. Widgets edit values; Flush merges only changed
// fields by stable identity and never resurrects a concurrently deleted profile.
class UiConfigDraft {
public:
    UiConfigDraft();
    ~UiConfigDraft();
    UiConfigDraft(const UiConfigDraft&) = delete;
    UiConfigDraft& operator=(const UiConfigDraft&) = delete;
    void Flush();
    void ObserveInserted(int index, const Config& config);
    std::map<int, Config>& Configs() { return edited_.configs; }
    int& Selection() { return edited_.selectedIndex; }
private:
    ConfigStateSnapshot baseline_, edited_;
    UiConfigDraft* previous_ = nullptr;
};
std::map<int, Config>& UiConfigs();
int& UiSelectedConfigIndex();
void FlushUiConfigDraft();
void ObserveUiConfigInserted(int index, const Config& config);
#endif
