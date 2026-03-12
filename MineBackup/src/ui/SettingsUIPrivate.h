#pragma once

#include "Globals.h"
#include "MainUI.h"
#include "ConfigManager.h"
#include "TaskSystem.h"
#include "i18n.h"
#include "imgui-all.h"
#include "text_to_text.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include "Platform_win.h"
#include <windows.h>
#elif defined(__APPLE__)
#include "Platform_macos.h"
#else
#include "Platform_linux.h"
#endif

inline float CalcButtonWidth(const char* text, float minWidth = 80.0f, float padding = 20.0f) {
	float textWidth = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2 + padding;
	return (std::max)(textWidth, minWidth);
}

inline float CalcPairButtonWidth(const char* text1, const char* text2, float minWidth = 100.0f, float padding = 20.0f) {
	float w1 = ImGui::CalcTextSize(text1).x + ImGui::GetStyle().FramePadding.x * 2 + padding;
	float w2 = ImGui::CalcTextSize(text2).x + ImGui::GetStyle().FramePadding.x * 2 + padding;
	return (std::max)((std::max)(w1, w2), minWidth);
}

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
void DrawWorldManagement(Config& cfg);
void DrawBackupBehavior(Config& cfg);
void DrawBlacklistSettings(Config& cfg);
void DrawRestoreBehavior(Config& cfg);
void DrawAppearanceSettings(Config& cfg);
void DrawCloudSyncSettings(Config& cfg);
void DrawUnifiedTaskManager(SpecialConfig& spCfg);
void DrawServiceSettings(SpecialConfig& spCfg);
void DrawSpecialConfigSettings(SpecialConfig& spCfg);