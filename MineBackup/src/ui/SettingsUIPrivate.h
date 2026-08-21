#pragma once

#include "Globals.h"
#include "MainUI.h"
#include "ConfigManager.h"
#include "i18n.h"
#include "imgui-all.h"
#include "text_to_text.h"
#include "DesktopServices.h"
#include "JobDocument.h"
#include "FolderRewindFormat.h"
#include "UIHelpers.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>

#include "PlatformCompat.h"

#ifdef _WIN32
#include <windows.h>
#endif

inline void GetCompressionLevelRange(const std::wstring& method, int& minLevel, int& maxLevel) {
	minLevel = 1;
	maxLevel = 9;
	if (_wcsicmp(method.c_str(), L"zstd") == 0) {
		maxLevel = 22;
	}
}

inline void ClampCompressionLevel(const std::wstring& method, int& level) {
	int minLevel = 1;
	int maxLevel = 9;
	GetCompressionLevelRange(method, minLevel, maxLevel);
	if (level < minLevel) level = minLevel;
	if (level > maxLevel) level = maxLevel;
}

void DrawConfigManagementPanel();
void DrawApplicationSettings();
void NotifySettingsPersistenceCompleted();
void SuppressSettingsAutoSaveForCurrentFrame();
void DrawPathSettings(Config& cfg);
void DrawSystemIntegrationSettings();
void DrawWorldEditSettings(Config& cfg);
void DrawWorldManagement(Config& cfg);
void DrawBackupBehavior(Config& cfg);
void DrawBlacklistSettings(Config& cfg);
void DrawRestoreBehavior(Config& cfg);
void DrawAppearanceSettings(Config& cfg);
void DrawCloudSyncSettings(Config& cfg);
bool IsWEIntegrationPathValidForSave(const Config& cfg);
void DrawJobSettings();
