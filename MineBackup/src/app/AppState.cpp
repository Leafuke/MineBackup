#include "AppState.h"
#include <algorithm>
#include <stdexcept>
AppState g_appState;
namespace {
thread_local UiConfigDraft* activeDraft = nullptr;
void MergeFields(Config& current, const Config& baseline, const Config& edited) {
    if (edited.saveRoot != baseline.saveRoot) current.saveRoot = edited.saveRoot;
    if (edited.worlds != baseline.worlds) current.worlds = edited.worlds;
    if (edited.backupPath != baseline.backupPath) current.backupPath = edited.backupPath;
    if (edited.zipPath != baseline.zipPath) current.zipPath = edited.zipPath;
    if (edited.zipFormat != baseline.zipFormat) current.zipFormat = edited.zipFormat;
    if (edited.fontPath != baseline.fontPath) current.fontPath = edited.fontPath;
    if (edited.zipMethod != baseline.zipMethod) current.zipMethod = edited.zipMethod;
    if (edited.backupMode != baseline.backupMode) current.backupMode = edited.backupMode;
    if (edited.zipLevel != baseline.zipLevel) current.zipLevel = edited.zipLevel;
    if (edited.keepCount != baseline.keepCount) current.keepCount = edited.keepCount;
    if (edited.backupBefore != baseline.backupBefore) current.backupBefore = edited.backupBefore;
    if (edited.theme != baseline.theme) current.theme = edited.theme;
    if (edited.name != baseline.name) current.name = edited.name;
    if (edited.legacyConfigIdGenerated != baseline.legacyConfigIdGenerated) current.legacyConfigIdGenerated = edited.legacyConfigIdGenerated;
    if (edited.pendingLocalBinding != baseline.pendingLocalBinding) current.pendingLocalBinding = edited.pendingLocalBinding;
    if (edited.cpuThreads != baseline.cpuThreads) current.cpuThreads = edited.cpuThreads;
    if (edited.useLowPriority != baseline.useLowPriority) current.useLowPriority = edited.useLowPriority;
    if (edited.skipIfUnchanged != baseline.skipIfUnchanged) current.skipIfUnchanged = edited.skipIfUnchanged;
    if (edited.maxSmartBackupsPerFull != baseline.maxSmartBackupsPerFull) current.maxSmartBackupsPerFull = edited.maxSmartBackupsPerFull;
    if (edited.backupOnGameStart != baseline.backupOnGameStart) current.backupOnGameStart = edited.backupOnGameStart;
    if (edited.blacklist != baseline.blacklist) current.blacklist = edited.blacklist;
    if (edited.cloudSyncEnabled != baseline.cloudSyncEnabled) current.cloudSyncEnabled = edited.cloudSyncEnabled;
    if (edited.rclonePath != baseline.rclonePath) current.rclonePath = edited.rclonePath;
    if (edited.rcloneRemotePath != baseline.rcloneRemotePath) current.rcloneRemotePath = edited.rcloneRemotePath;
    if (edited.cloudSyncMode != baseline.cloudSyncMode) current.cloudSyncMode = edited.cloudSyncMode;
    if (edited.cloudWorkingDirectory != baseline.cloudWorkingDirectory) current.cloudWorkingDirectory = edited.cloudWorkingDirectory;
    if (edited.cloudTimeoutSeconds != baseline.cloudTimeoutSeconds) current.cloudTimeoutSeconds = edited.cloudTimeoutSeconds;
    if (edited.cloudRetryCount != baseline.cloudRetryCount) current.cloudRetryCount = edited.cloudRetryCount;
    if (edited.cloudSyncHistoryAfterUpload != baseline.cloudSyncHistoryAfterUpload) current.cloudSyncHistoryAfterUpload = edited.cloudSyncHistoryAfterUpload;
    if (edited.cloudAutoDownloadBeforeRestore != baseline.cloudAutoDownloadBeforeRestore) current.cloudAutoDownloadBeforeRestore = edited.cloudAutoDownloadBeforeRestore;
    if (edited.cloudLastRunUtc != baseline.cloudLastRunUtc) current.cloudLastRunUtc = edited.cloudLastRunUtc;
    if (edited.cloudLastExitCode != baseline.cloudLastExitCode) current.cloudLastExitCode = edited.cloudLastExitCode;
    if (edited.cloudLastErrorMessage != baseline.cloudLastErrorMessage) current.cloudLastErrorMessage = edited.cloudLastErrorMessage;
    if (edited.snapshotPath != baseline.snapshotPath) current.snapshotPath = edited.snapshotPath;
    if (edited.othersPath != baseline.othersPath) current.othersPath = edited.othersPath;
    if (edited.enableWEIntegration != baseline.enableWEIntegration) current.enableWEIntegration = edited.enableWEIntegration;
    if (edited.weSnapshotPath != baseline.weSnapshotPath) current.weSnapshotPath = edited.weSnapshotPath;
}
}

void DesktopConfigState::Changed() {
    ++revision_;
    cached_.reset();
    byId_.clear();
    for (const auto& [index, config] : configs_) byId_.emplace(config.configId, index);
    if (!configs_.contains(selected_)) selected_ = configs_.empty() ? 1 : configs_.begin()->first;
}
DesktopConfigState::WriteAccess::~WriteAccess() { if (changed_) owner_.Changed(); }
std::shared_ptr<const std::map<int, Config>> DesktopConfigState::Read() const {
    std::lock_guard lock(mutex_);
    if (!cached_) cached_ = std::make_shared<const std::map<int, Config>>(configs_);
    return cached_;
}
ConfigStateSnapshot DesktopConfigState::Snapshot() const {
    std::lock_guard lock(mutex_);
    return {configs_, selected_};
}
JobDocument DesktopConfigState::SnapshotJobs() const {
    std::lock_guard lock(mutex_);
    return jobs_;
}
int DesktopConfigState::Selection() const { std::lock_guard lock(mutex_); return selected_; }
bool DesktopConfigState::Contains(int index) const { std::lock_guard lock(mutex_); return configs_.contains(index); }
void DesktopConfigState::Select(int index) {
    std::lock_guard lock(mutex_);
    if (configs_.contains(index)) selected_ = index;
}
bool DesktopConfigState::Modify(const std::wstring& id, const std::function<void(Config&)>& mutation) {
    std::lock_guard lock(mutex_);
    const auto found = byId_.find(id);
    if (found == byId_.end()) return false;
    mutation(configs_.at(found->second));
    Changed();
    return true;
}
bool DesktopConfigState::Delete(const std::wstring& id) {
    std::lock_guard lock(mutex_);
    const auto found = byId_.find(id);
    if (found == byId_.end()) return false;
    configs_.erase(found->second);
    Changed();
    return true;
}
ConfigStateSnapshot SnapshotConfigState() { return g_appState.configuration.Snapshot(); }
bool ModifyConfigById(const std::wstring& id, const std::function<void(Config&)>& mutation) {
    return g_appState.configuration.Modify(id, mutation);
}
bool DeleteConfigById(const std::wstring& id) { return g_appState.configuration.Delete(id); }
int SelectedConfigIndex() { return g_appState.configuration.Selection(); }
void SelectConfigIndex(int index) {
    g_appState.configuration.Select(index);
    if (activeDraft) activeDraft->Selection() = SelectedConfigIndex();
}
UiConfigDraft::UiConfigDraft() : view_(g_appState.configuration.Read()),
    selected_(SelectedConfigIndex()), baselineSelection_(selected_), previous_(activeDraft) { activeDraft = this; }
UiConfigDraft::~UiConfigDraft() { Flush(); activeDraft = previous_; }
void UiConfigDraft::Refresh() {
    retainedViews_.push_back(view_);
    view_ = g_appState.configuration.Read();
}
Config& UiConfigDraft::Edit(int index) {
    auto found = drafts_.find(index);
    if (found == drafts_.end()) {
        const auto& config = view_->at(index);
        found = drafts_.emplace(index, Draft{config, config}).first;
    }
    return found->second.edited;
}
void UiConfigDraft::ObserveInserted(int, const Config&) { Refresh(); }
void UiConfigDraft::Delete(int index) {
    const auto found = view_->find(index);
    if (found == view_->end()) return;
    DeleteConfigById(found->second.configId);
    Refresh();
    selected_ = SelectedConfigIndex();
    baselineSelection_ = selected_;
}
void UiConfigDraft::Flush() {
    bool changed = false;
    for (auto& [index, draft] : drafts_) {
        if (draft.baseline == draft.edited) continue;
        if (draft.baseline.configId == draft.edited.configId) {
            changed |= ModifyConfigById(draft.baseline.configId, [&](Config& current) {
                MergeFields(current, draft.baseline, draft.edited);
            });
        }
        draft.baseline = draft.edited;
    }
    if (selected_ != baselineSelection_) g_appState.configuration.Select(selected_);
    selected_ = SelectedConfigIndex();
    baselineSelection_ = selected_;
    if (changed) Refresh();
}
const std::map<int, Config>& UiConfigView() {
    if (!activeDraft) throw std::logic_error("UI configuration access requires a frame draft");
    return activeDraft->View();
}
Config& EditUiConfig(int index) {
    if (!activeDraft) throw std::logic_error("UI configuration editing requires a frame draft");
    return activeDraft->Edit(index);
}
void DeleteUiConfig(int index) {
    if (!activeDraft) throw std::logic_error("UI configuration deletion requires a frame draft");
    activeDraft->Delete(index);
}
int& UiSelectedConfigIndex() {
    if (!activeDraft) throw std::logic_error("UI selection access requires a frame draft");
    return activeDraft->Selection();
}
void FlushUiConfigDraft() { if (activeDraft) activeDraft->Flush(); }
void ObserveUiConfigInserted(int index, const Config& config) { if (activeDraft) activeDraft->ObserveInserted(index, config); }
