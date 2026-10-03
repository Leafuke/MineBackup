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
	VSCodeDark = 7,
	SolarizedLight = 8,
	SolarizedDark = 9,
	SystemAuto = 10,
	Custom = 11
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
	int theme = static_cast<int>(ThemeId::NordLight);
	int lastValidTheme = static_cast<int>(ThemeId::NordLight);
	int systemThemeLight = static_cast<int>(ThemeId::WindowsLight);
	int systemThemeDark = static_cast<int>(ThemeId::WindowsDark);
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

struct KnotLinkRuntimeState {
    std::atomic<bool> startupStatusReady{ false };
    std::atomic<bool> startupNeedsUpdate{ false };
    std::string startupVersion;
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
	bool onboardingActive = false;
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
	std::wstring defaultBackupRootPath;
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

struct ExternalToolRuntimeState {
	bool rcloneInstallRunning = false;
	bool rcloneInstallSucceeded = false;
	std::wstring rcloneInstallMessage;
	bool knotLinkInstallRunning = false;
	bool knotLinkInstallSucceeded = false;
	std::wstring knotLinkInstallMessage;
};

struct AppGlobalState {
    std::string currentVersion = MINEBACKUP_VERSION_STRING;
    AppWindowState window;
    AppAppearanceState appearance;
    AppUpdateState update;
    KnotLinkRuntimeState knotlink;
    AppUiState ui;
    AppSettingsState settings;
    CoreValidationRuntimeState coreValidation;
    ExternalToolRuntimeState externalTools;
};

AppWindowState& WindowState();
AppAppearanceState& AppearanceState();
AppUpdateState& UpdateState();
KnotLinkRuntimeState& KnotLinkState();
AppUiState& UiState();
AppSettingsState& SettingsState();
CoreValidationRuntimeState& CoreValidationState();
ExternalToolRuntimeState& ExternalToolState();
const std::string& ApplicationVersion();

// i18n
extern const char* lang_codes[2];
extern const char* langs[2];

#endif // GLOBALS_H
