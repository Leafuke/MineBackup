#pragma once
#ifndef UI_HELPERS_H
#define UI_HELPERS_H

// 共享的 UI 辅助内联函数
// 被多个 UI 文件使用（WizardUI, MainUI, WorldListUI, WorldDetailUI, HistoryUI）

#include "imgui-all.h"
#include <algorithm>

inline float CalcButtonWidth(const char* text, float minWidth = 80.0f, float padding = 20.0f) {
	float textWidth = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2 + padding;
	return (std::max)(textWidth, minWidth);
}

inline float CalcPairButtonWidth(const char* text1, const char* text2, float minWidth = 100.0f, float padding = 20.0f) {
	float w1 = ImGui::CalcTextSize(text1).x + ImGui::GetStyle().FramePadding.x * 2 + padding;
	float w2 = ImGui::CalcTextSize(text2).x + ImGui::GetStyle().FramePadding.x * 2 + padding;
	return (std::max)((std::max)(w1, w2), minWidth);
}

#endif // UI_HELPERS_H
