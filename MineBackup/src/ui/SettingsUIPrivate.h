#pragma once

#include "Globals.h"
#include "MainUI.h"
#include "ConfigManager.h"
#include "TaskSystem.h"
#include "i18n.h"
#include "imgui-all.h"
#include "text_to_text.h"
#include "DesktopServices.h"
#include "SpecialConfigPolicy.h"
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

	auto widenByConfigMethod = [&](int configIndex) {
		auto it = g_appState.configs.find(configIndex);
		if (it == g_appState.configs.end()) return;
		int methodMin = 1;
		int methodMax = 9;
		GetCompressionLevelRange(it->second.zipMethod, methodMin, methodMax);
		if (methodMax > maxLevel) {
			maxLevel = methodMax;
		}
	};

	if (!spCfg.unifiedTasks.empty()) {
		for (const auto& task : spCfg.unifiedTasks) {
			if (task.type != TaskTypeV2::Backup || !task.enabled) continue;
			widenByConfigMethod(task.configIndex);
		}
		return;
	}

	for (const auto& task : spCfg.tasks) {
		widenByConfigMethod(task.configIndex);
	}
}

void DrawConfigManagementPanel();
void DrawPathSettings(Config& cfg);
void DrawModIntegrationSettings(Config& cfg);
void DrawWorldManagement(Config& cfg);
void DrawBackupBehavior(Config& cfg);
void DrawBlacklistSettings(Config& cfg);
void DrawRestoreBehavior(Config& cfg);
void DrawAppearanceSettings(Config& cfg);
void DrawCloudSyncSettings(Config& cfg);
bool IsWEIntegrationPathValidForSave(const Config& cfg);
void DrawUnifiedTaskManager(SpecialConfig& spCfg);
void DrawServiceSettings(SpecialConfig& spCfg);
void DrawSpecialConfigSettings(SpecialConfig& spCfg);
