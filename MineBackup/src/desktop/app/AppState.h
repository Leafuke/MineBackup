#include "DesktopRuntimeState.h"
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
#include <cstdint>
#include <memory>

struct CloudTaskRuntimeState {
	std::atomic<bool> busy{ false };
	std::atomic<int> progress{ 0 };
	int activeConfigIndex = -1;
	std::wstring statusText;
	std::wstring lastMessage;
	std::mutex mutex;
};

struct ConfigStateSnapshot {
    std::map<int, Config> configs;
    int selectedIndex = 1;
};

class DesktopConfigState {
public:
    class WriteAccess {
    public:
        explicit WriteAccess(DesktopConfigState& owner) : owner_(owner), lock_(owner.mutex_) {}
        ~WriteAccess();
        WriteAccess(const WriteAccess&) = delete;
        std::map<int, Config>& Configs() { changed_ = true; return owner_.configs_; }
        const std::map<int, Config>& ReadConfigs() const { return owner_.configs_; }
        JobDocument& Jobs() { return owner_.jobs_; }
        int& Selection() { return owner_.selected_; }
    private:
        DesktopConfigState& owner_;
        std::unique_lock<std::recursive_mutex> lock_;
        bool changed_ = false;
    };
    WriteAccess Write() { return WriteAccess(*this); }
    std::shared_ptr<const std::map<int, Config>> Read() const;
    ConfigStateSnapshot Snapshot() const;
    JobDocument SnapshotJobs() const;
    int Selection() const;
    bool Contains(int index) const;
    void Select(int index);
    bool Modify(const std::wstring& id, const std::function<void(Config&)>& mutation);
    bool Delete(const std::wstring& id);
private:
    void Changed();
    mutable std::recursive_mutex mutex_;
    std::map<int, Config> configs_;
    std::map<std::wstring, int> byId_;
    JobDocument jobs_;
    int selected_ = 1;
    std::uint64_t revision_ = 0;
    mutable std::shared_ptr<const std::map<int, Config>> cached_;
};

struct AppState {

	bool done = false;
    std::atomic<bool> profileRecoveryRequired{false};

	// UI State
	bool showMainApp = false;


	// Data
    DesktopConfigState configuration;

	std::map<std::pair<std::wstring, std::wstring>, AutoBackupTask> g_active_auto_backups; // Config ID + canonical source path

	std::mutex task_mutex;		// 专门用于保护 g_active_auto_backups
	bool isRespond = false;
	std::atomic<HotRestoreState> hotkeyRestoreState = HotRestoreState::IDLE;

	KnotLinkModInfo knotLinkMod;
	CloudTaskRuntimeState cloudTask;
};

extern AppState g_appState;

ConfigStateSnapshot SnapshotConfigState();
bool ModifyConfigById(const std::wstring& id, const std::function<void(Config&)>& mutation);
bool DeleteConfigById(const std::wstring& id);
int SelectedConfigIndex();
void SelectConfigIndex(int index);

// A frame retains immutable views and only copies configurations explicitly edited.
class UiConfigDraft {
public:
    UiConfigDraft();
    ~UiConfigDraft();
    UiConfigDraft(const UiConfigDraft&) = delete;
    UiConfigDraft& operator=(const UiConfigDraft&) = delete;
    void Flush();
    void ObserveInserted(int index, const Config& config);
    const std::map<int, Config>& View() const { return *view_; }
    Config& Edit(int index);
    void Delete(int index);
    int& Selection() { return selected_; }
    std::size_t EditedCount() const { return drafts_.size(); }
private:
    void Refresh();
    struct Draft { Config baseline; Config edited; };
    std::shared_ptr<const std::map<int, Config>> view_;
    std::vector<std::shared_ptr<const std::map<int, Config>>> retainedViews_;
    std::map<int, Draft> drafts_;
    int selected_ = 1;
    int baselineSelection_ = 1;
    UiConfigDraft* previous_ = nullptr;
};
const std::map<int, Config>& UiConfigView();
Config& EditUiConfig(int index);
void DeleteUiConfig(int index);
int& UiSelectedConfigIndex();
void FlushUiConfigDraft();
void ObserveUiConfigInserted(int index, const Config& config);
#endif
