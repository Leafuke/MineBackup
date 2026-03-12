#pragma once
#ifndef MAIN_UI_H
#define MAIN_UI_H

// 从 MineBackup.cpp 拆分出的 UI 模块函数声明

#include "AppState.h"
#include <vector>
#include <string>

// 前向声明
struct Console;

// WizardUI.cpp — 首次启动配置向导
void ShowConfigWizard(bool& showConfigWizard, bool& errorShow, bool sevenZipExtracted,
	const std::wstring& g_7zTempPath);

// MainUI.cpp — 菜单栏、弹窗、Dock 布局、配置切换器
// (在主循环中被直接调用的部分，将直接集成在 MineBackup.cpp 的主循环调用中)

// WorldListUI.cpp — 世界列表面板
std::vector<DisplayWorld> BuildDisplayWorldsForSelection();
int ImGuiKeyToVK(ImGuiKey key);

// HistoryUI.cpp — 历史窗口
void ShowHistoryWindow(int& tempCurrentConfigIndex);

// SpecialMode.cpp — 特殊模式执行
void RunSpecialMode(int configId);

// MineBackup.cpp 中保留的函数
void ApplyTheme(const int& theme);
bool LoadTextureFromFileGL(const char* filename, unsigned int* out_texture, int* out_width, int* out_height);

#endif // MAIN_UI_H
