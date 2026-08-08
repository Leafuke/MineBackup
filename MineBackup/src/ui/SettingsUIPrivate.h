#pragma once

#include "Globals.h"
#include "MainUI.h"
#include "ConfigManager.h"
#include "legacy/LegacyServiceCleanup.h"
#include "i18n.h"
#include "imgui-all.h"
#include "text_to_text.h"
#include "DesktopServices.h"
#include "SpecialTaskDocument.h"
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

inline void GetSpecialConfigCompressionLevelRange(const SpecialConfig& spCfg, int& minLevel, int& maxLevel) {
	minLevel = 1;
	maxLevel = 9;

	auto widenByConfigMethod = [&](const std::wstring& configId) {
		auto it = std::find_if(g_appState.configs.begin(), g_appState.configs.end(),
			[&](const auto& item) { return item.second.configId == configId; });
		if (it == g_appState.configs.end()) return;
		int methodMin = 1;
		int methodMax = 9;
		GetCompressionLevelRange(it->second.zipMethod, methodMin, methodMax);
		if (methodMax > maxLevel) {
			maxLevel = methodMax;
		}
	};

	for (const auto& task : spCfg.specialTasks) {
		if (task.type != SpecialTaskType::Backup || !task.enabled) continue;
		widenByConfigMethod(task.target.configId);
	}
}

void DrawConfigManagementPanel();
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
void DrawUnifiedTaskManager(SpecialConfig& spCfg);
void DrawServiceSettings(SpecialConfig& spCfg);
enum class SpecialSettingsPage {
	Overview,
	Tasks,
	Backup,
	LegacyCleanup
};
void DrawSpecialConfigSettings(SpecialConfig& spCfg, SpecialSettingsPage page);
