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
void NormalizeSelection() {
    if (!g_appState.configs.contains(g_appState.currentConfigIndex))
        g_appState.currentConfigIndex = g_appState.configs.empty() ? 1 : g_appState.configs.begin()->first;
}
}
ConfigStateSnapshot SnapshotConfigState() {
    std::lock_guard lock(g_appState.configsMutex);
    return {g_appState.configs, g_appState.currentConfigIndex};
}
bool ModifyConfigById(const std::wstring& id, const std::function<void(Config&)>& mutation) {
    std::lock_guard lock(g_appState.configsMutex);
    for (auto& [index, config] : g_appState.configs) {
        if (config.configId == id) { mutation(config); return true; }
    }
    return false;
}
bool DeleteConfigById(const std::wstring& id) {
    std::lock_guard lock(g_appState.configsMutex);
    for (auto it = g_appState.configs.begin(); it != g_appState.configs.end(); ++it) {
        if (it->second.configId == id) { g_appState.configs.erase(it); NormalizeSelection(); return true; }
    }
    return false;
}
int SelectedConfigIndex() { return SnapshotConfigState().selectedIndex; }
void SelectConfigIndex(int index) {
    std::lock_guard lock(g_appState.configsMutex);
    if (g_appState.configs.contains(index)) {
        g_appState.currentConfigIndex = index;
        if (activeDraft) activeDraft->Selection() = index;
    }
}
UiConfigDraft::UiConfigDraft() : baseline_(SnapshotConfigState()), edited_(baseline_), previous_(activeDraft) {
    activeDraft = this;
}
UiConfigDraft::~UiConfigDraft() { Flush(); activeDraft = previous_; }
void UiConfigDraft::ObserveInserted(int index, const Config& config) {
    baseline_.configs[index] = config;
    edited_.configs[index] = config;
}
void UiConfigDraft::Flush() {
    std::lock_guard lock(g_appState.configsMutex);
    for (const auto& [index, original] : baseline_.configs) {
        const auto draft = edited_.configs.find(index);
        auto current = std::find_if(g_appState.configs.begin(), g_appState.configs.end(),
            [&](const auto& item) { return item.second.configId == original.configId; });
        if (current == g_appState.configs.end()) continue;
        if (draft == edited_.configs.end()) g_appState.configs.erase(current);
        else if (draft->second.configId == original.configId) MergeFields(current->second, original, draft->second);
    }
    for (const auto& [index, config] : edited_.configs) {
        if (!baseline_.configs.contains(index)) g_appState.configs.emplace(index, config);
    }
    if (edited_.selectedIndex != baseline_.selectedIndex) g_appState.currentConfigIndex = edited_.selectedIndex;
    NormalizeSelection();
    if (!g_appState.configs.contains(edited_.selectedIndex)) edited_.selectedIndex = g_appState.currentConfigIndex;
    // Keep widget references valid until the frame ends; only advance the baseline.
    baseline_ = edited_;
}
std::map<int, Config>& UiConfigs() {
    if (!activeDraft) throw std::logic_error("UI configuration access requires a frame draft");
    return activeDraft->Configs();
}
int& UiSelectedConfigIndex() {
    if (!activeDraft) throw std::logic_error("UI selection access requires a frame draft");
    return activeDraft->Selection();
}
void FlushUiConfigDraft() { if (activeDraft) activeDraft->Flush(); }
void ObserveUiConfigInserted(int index, const Config& config) { if (activeDraft) activeDraft->ObserveInserted(index, config); }
