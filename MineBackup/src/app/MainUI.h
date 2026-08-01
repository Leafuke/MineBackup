#pragma once
#ifndef MAIN_UI_H
#define MAIN_UI_H

#include "AppPaths.h"
#include "DesktopServices.h"
#include "WorldListModel.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

struct MainUiFrameContext {
	DesktopServices* desktopServices = nullptr;
	GLFWwindow* window = nullptr;
	const AppPaths* paths = nullptr;
	std::function<std::vector<GlobalHotkeyBinding>()> currentGlobalHotkeys;
};

// WizardUI.cpp — 首次启动配置向导
void ShowConfigWizard(bool& showConfigWizard, bool& errorShow, bool sevenZipExtracted,
	const std::wstring& sevenZipTemporaryPath);

// 主界面与世界列表按帧渲染，显式上下文避免重新引入跨翻译单元全局别名。
void DrawMainUiFrame(const MainUiFrameContext& context);
void DrawWorldListUiFrame(const MainUiFrameContext& context);
void ReleaseMainUiResources();
void ReleaseWorldListUiResources();

// WorldListUI.cpp — 世界列表模型
std::vector<DisplayWorld> BuildDisplayWorldsForSelection();

// HistoryUI.cpp — 历史窗口
void ShowHistoryWindow(int configIndex,
	const std::optional<std::wstring>& initialWorld = std::nullopt);

// SpecialMode.cpp — 特殊模式执行
void RunSpecialMode(int configId);

#endif // MAIN_UI_H
