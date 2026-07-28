#pragma once
#ifndef GLOBALS_H
#define GLOBALS_H

// 全局变量集中声明
// 定义在 MineBackup.cpp 中，其他文件通过 #include "Globals.h" 访问

#include "AppState.h"
#include "Logging.h"
#include "MineBackupVersion.h"
#include "imgui.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

// 前向声明
struct GLFWwindow;
struct ImVec4;

enum class ThemeId : int {
	ImGuiDark = 0,
	ImGuiLight = 1,
	ImGuiClassic = 2,
	WindowsLight = 3,
	WindowsDark = 4,
	NordLight = 5,
	NordDark = 6,
	Custom = 7
};

inline bool IsValidThemeId(int value) {
	return value >= static_cast<int>(ThemeId::ImGuiDark)
		&& value <= static_cast<int>(ThemeId::Custom);
}

struct AppWindowState {
	GLFWwindow* handle = nullptr;
	int width = 1280;
	int height = 800;
	ImVec4 clearColor = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
};

struct AppAppearanceState {
	int theme = static_cast<int>(ThemeId::ImGuiLight);
	int lastValidTheme = static_cast<int>(ThemeId::ImGuiLight);
	std::wstring fontPath;
	std::string customThemeError;
	float userScale = 1.0f;
	int schema = 1;
	bool userScaleV2 = true;
	bool pendingScaleMigration = false;
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
	bool showSettings = false;
	bool restartRequired = false;
	bool restartBannerDismissed = false;
	bool showHistoryWindow = false;
	bool specialSetting = false;
	int closeAction = 0;
	bool rememberCloseAction = false;
	bool showCloseConfirmDialog = false;
	std::wstring worldToFocusInHistory;
};

struct AppSettingsState {
	bool safeDelete = true;
	bool checkForUpdates = true;
	bool receiveNotices = true;
	bool stopAutoBackupOnExit = false;
	bool runOnStartup = false;
	bool silentStartupToTray = false;
	bool autoScanForWorlds = false;
	minebackup::logging::LogFileLevel logFileLevel = minebackup::logging::LogFileLevel::Info;
	minebackup::logging::LogLevel logViewLevel = minebackup::logging::LogLevel::Info;
	bool logViewAutoTail = true;
	bool logViewShowTime = false;
	bool logViewShowCategory = false;
	bool enableKnotLink = true;
	bool autoStartKnotLinkServer = true;
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
};

struct ExternalToolRuntimeState {
	bool rcloneInstallRunning = false;
	bool rcloneInstallSucceeded = false;
	std::wstring rcloneInstallMessage;
};

struct AppGlobalState {
	std::string currentVersion = MINEBACKUP_VERSION_STRING;
	AppWindowState window;
	AppAppearanceState appearance;
	AppUpdateState update;
	AppUiState ui;
	AppSettingsState settings;
	SpecialTaskRuntimeState specialTasks;
	CoreValidationRuntimeState coreValidation;
	ExternalToolRuntimeState externalTools;
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
inline float& g_uiScale = g_globals.appearance.userScale;
inline int& g_theme = g_globals.appearance.theme;
inline int& g_lastValidTheme = g_globals.appearance.lastValidTheme;
inline std::string& g_customThemeError = g_globals.appearance.customThemeError;
inline int& g_appearanceSchema = g_globals.appearance.schema;
inline bool& g_uiScaleV2 = g_globals.appearance.userScaleV2;
inline bool& g_uiScaleMigrationPending = g_globals.appearance.pendingScaleMigration;
inline ImVec4& clear_color = g_globals.window.clearColor;

inline int& last_interval = g_globals.settings.lastIntervalMinutes;
inline std::wstring& Fontss = g_globals.appearance.fontPath;
inline bool& showSettings = g_globals.ui.showSettings;
inline bool& g_restartRequired = g_globals.ui.restartRequired;
inline bool& g_restartBannerDismissed = g_globals.ui.restartBannerDismissed;
inline bool& showHistoryWindow = g_globals.ui.showHistoryWindow;
inline bool& specialSetting = g_globals.ui.specialSetting;
inline int& g_closeAction = g_globals.ui.closeAction;
inline bool& g_rememberCloseAction = g_globals.ui.rememberCloseAction;
inline bool& g_showCloseConfirmDialog = g_globals.ui.showCloseConfirmDialog;
inline std::wstring& g_worldToFocusInHistory = g_globals.ui.worldToFocusInHistory;

inline bool& isSafeDelete = g_globals.settings.safeDelete;
inline bool& g_CheckForUpdates = g_globals.settings.checkForUpdates;
inline bool& g_ReceiveNotices = g_globals.settings.receiveNotices;
inline bool& g_StopAutoBackupOnExit = g_globals.settings.stopAutoBackupOnExit;
inline bool& g_RunOnStartup = g_globals.settings.runOnStartup;
inline bool& g_SilentStartupToTray = g_globals.settings.silentStartupToTray;
inline bool& g_AutoScanForWorlds = g_globals.settings.autoScanForWorlds;
inline minebackup::logging::LogFileLevel& g_logFileLevel = g_globals.settings.logFileLevel;
inline minebackup::logging::LogLevel& g_logViewLevel = g_globals.settings.logViewLevel;
inline bool& g_logViewAutoTail = g_globals.settings.logViewAutoTail;
inline bool& g_logViewShowTime = g_globals.settings.logViewShowTime;
inline bool& g_logViewShowCategory = g_globals.settings.logViewShowCategory;
inline bool& g_enableKnotLink = g_globals.settings.enableKnotLink;
inline bool& g_autoStartKnotLinkServer = g_globals.settings.autoStartKnotLinkServer;
inline std::atomic<bool>& g_CoreValidationPending = g_globals.settings.coreValidationPending;
inline std::atomic<bool>& g_CoreValidationPassed = g_globals.settings.coreValidationPassed;
inline int& g_hotKeyBackupId = g_globals.settings.hotKeyBackupId;
inline int& g_hotKeyRestoreId = g_globals.settings.hotKeyRestoreId;
inline std::vector<std::wstring>& restoreWhitelist = g_globals.settings.restoreWhitelist;

inline std::atomic<bool>& specialTasksRunning = g_globals.specialTasks.tasksRunning;
inline std::atomic<bool>& specialTasksComplete = g_globals.specialTasks.tasksComplete;
inline std::atomic<bool>& g_CoreValidationRunning = g_globals.coreValidation.running;
inline bool& g_RcloneInstallRunning = g_globals.externalTools.rcloneInstallRunning;
inline bool& g_RcloneInstallSucceeded = g_globals.externalTools.rcloneInstallSucceeded;
inline std::wstring& g_RcloneInstallMessage = g_globals.externalTools.rcloneInstallMessage;

// i18n
extern const char* lang_codes[2];
extern const char* langs[2];

#endif // GLOBALS_H
