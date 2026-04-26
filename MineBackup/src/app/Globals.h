#pragma once
#ifndef GLOBALS_H
#define GLOBALS_H

// 全局变量集中声明
// 定义在 MineBackup.cpp 中，其他文件通过 #include "Globals.h" 访问

#include "AppState.h"
#include "imgui.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

// 前向声明
struct GLFWwindow;
struct ImVec4;

struct AppWindowState {
	GLFWwindow* handle = nullptr;
	int width = 1280;
	int height = 800;
	float uiScale = 1.0f;
	ImVec4 clearColor = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
};

struct AppUpdateState {
	std::atomic<bool> updateCheckDone{ false };
	std::atomic<bool> newVersionAvailable{ false };
	std::atomic<bool> noticeCheckDone{ false };
	std::atomic<bool> newNoticeAvailable{ false };
	std::string latestVersion;
	std::string releaseNotes;
	std::string noticeContent;
	std::string noticeUpdatedAt;
	std::string noticeLastSeenVersion;
};

struct AppUiState {
	std::wstring fontPath;
	bool showSettings = false;
	bool showHistoryWindow = false;
	bool specialSetting = false;
	int closeAction = 0;
	bool rememberCloseAction = false;
	bool showCloseConfirmDialog = false;
	std::wstring worldToFocusInHistory;
};

struct AppSettingsState {
	bool silence = false;
	bool safeDelete = true;
	bool checkForUpdates = true;
	bool receiveNotices = true;
	bool stopAutoBackupOnExit = false;
	bool runOnStartup = false;
	bool silentStartupToTray = false;
	bool autoScanForWorlds = false;
	bool autoLogEnabled = true;
	bool enableKnotLink = true;
	std::atomic<bool> coreValidationPending{ false };
	std::atomic<bool> coreValidationPassed{ false };
	int hotKeyBackupId = 'S';
	int hotKeyRestoreId = 'Z';
	int lastIntervalMinutes = 15;
	std::vector<std::wstring> restoreWhitelist;
};

struct CoreValidationRuntimeState {
	std::atomic<bool> running{ false };
};

struct SpecialTaskRuntimeState {
	std::atomic<bool> tasksRunning{ false };
	std::atomic<bool> tasksComplete{ false };
	std::thread exitWatcherThread;
	std::atomic<bool> stopExitWatcher{ false };
};

struct AppGlobalState {
	std::string currentVersion = "1.15.0";
	AppWindowState window;
	AppUpdateState update;
	AppUiState ui;
	AppSettingsState settings;
	SpecialTaskRuntimeState specialTasks;
	CoreValidationRuntimeState coreValidation;
};

extern AppGlobalState g_globals;

inline GLFWwindow*& wc = g_globals.window.handle;
inline std::string& CURRENT_VERSION = g_globals.currentVersion;

inline std::atomic<bool>& g_UpdateCheckDone = g_globals.update.updateCheckDone;
inline std::atomic<bool>& g_NewVersionAvailable = g_globals.update.newVersionAvailable;
inline std::atomic<bool>& g_NoticeCheckDone = g_globals.update.noticeCheckDone;
inline std::atomic<bool>& g_NewNoticeAvailable = g_globals.update.newNoticeAvailable;
inline std::string& g_LatestVersionStr = g_globals.update.latestVersion;
inline std::string& g_ReleaseNotes = g_globals.update.releaseNotes;
inline std::string& g_NoticeContent = g_globals.update.noticeContent;
inline std::string& g_NoticeUpdatedAt = g_globals.update.noticeUpdatedAt;
inline std::string& g_NoticeLastSeenVersion = g_globals.update.noticeLastSeenVersion;

inline int& g_windowWidth = g_globals.window.width;
inline int& g_windowHeight = g_globals.window.height;
inline float& g_uiScale = g_globals.window.uiScale;
inline ImVec4& clear_color = g_globals.window.clearColor;

inline int& last_interval = g_globals.settings.lastIntervalMinutes;
inline std::wstring& Fontss = g_globals.ui.fontPath;
inline bool& showSettings = g_globals.ui.showSettings;
inline bool& showHistoryWindow = g_globals.ui.showHistoryWindow;
inline bool& specialSetting = g_globals.ui.specialSetting;
inline int& g_closeAction = g_globals.ui.closeAction;
inline bool& g_rememberCloseAction = g_globals.ui.rememberCloseAction;
inline bool& g_showCloseConfirmDialog = g_globals.ui.showCloseConfirmDialog;
inline std::wstring& g_worldToFocusInHistory = g_globals.ui.worldToFocusInHistory;

inline bool& isSilence = g_globals.settings.silence;
inline bool& isSafeDelete = g_globals.settings.safeDelete;
inline bool& g_CheckForUpdates = g_globals.settings.checkForUpdates;
inline bool& g_ReceiveNotices = g_globals.settings.receiveNotices;
inline bool& g_StopAutoBackupOnExit = g_globals.settings.stopAutoBackupOnExit;
inline bool& g_RunOnStartup = g_globals.settings.runOnStartup;
inline bool& g_SilentStartupToTray = g_globals.settings.silentStartupToTray;
inline bool& g_AutoScanForWorlds = g_globals.settings.autoScanForWorlds;
inline bool& g_autoLogEnabled = g_globals.settings.autoLogEnabled;
inline bool& g_enableKnotLink = g_globals.settings.enableKnotLink;
inline std::atomic<bool>& g_CoreValidationPending = g_globals.settings.coreValidationPending;
inline std::atomic<bool>& g_CoreValidationPassed = g_globals.settings.coreValidationPassed;
inline int& g_hotKeyBackupId = g_globals.settings.hotKeyBackupId;
inline int& g_hotKeyRestoreId = g_globals.settings.hotKeyRestoreId;
inline std::vector<std::wstring>& restoreWhitelist = g_globals.settings.restoreWhitelist;

inline std::atomic<bool>& specialTasksRunning = g_globals.specialTasks.tasksRunning;
inline std::atomic<bool>& specialTasksComplete = g_globals.specialTasks.tasksComplete;
inline std::thread& g_exitWatcherThread = g_globals.specialTasks.exitWatcherThread;
inline std::atomic<bool>& g_stopExitWatcher = g_globals.specialTasks.stopExitWatcher;
inline std::atomic<bool>& g_CoreValidationRunning = g_globals.coreValidation.running;

// i18n
extern const char* lang_codes[2];
extern const char* langs[2];

#endif // GLOBALS_H
