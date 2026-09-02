#include "MainUI.h"
#include "ApplicationActions.h"
#include "AppearanceRuntime.h"
#include "Globals.h"
#include "ImGuiRuntime.h"
#include "MainUiController.h"
#include "SettingsUI.h"
#include "SettingsUIHotkeys.h"
#include "UIHelpers.h"
#include "ThemePalette.h"
#include "imgui-all.h"
#include "i18n.h"
#include "AppState.h"
#include "AppPaths.h"
#include "DesktopServices.h"
#include "CommandConsole.h"
#include "ConfigManager.h"
#include "text_to_text.h"
#include "HistoryManager.h"
#include "BackupManager.h"
#include "CloudSyncService.h"
#include "CoreValidation.h"
#include "MigrationCoordinator.h"
#include "MigrationReportUI.h"
#include "FileName.h"
#include "GameSessionManager.h"
#include "LogPanel.h"
#include "Logging.h"
#include "RemoteContentService.h"
#include "PlatformCompat.h"
#include "TaskCoordinator.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace std;

namespace {
	auto& WorldIconTextures()
	{
		return GetMainUiController().worldList.iconCache.textures;
	}

	void EnsureWorldIconLoaded(const filesystem::path& worldFolder)
	{
		auto& textures = WorldIconTextures();
		const wstring iconKey = worldFolder.wstring();
		if (textures.contains(iconKey)) {
			return;
		}

		// 失败结果同样缓存，避免每帧重复探测不存在的图标文件。
		textures[iconKey] = 0;
		const filesystem::path javaIcon = worldFolder / L"icon.png";
		const filesystem::path bedrockIcon = worldFolder / L"world_icon.jpeg";
		const filesystem::path* sourcePath = nullptr;
		if (filesystem::exists(javaIcon)) {
			sourcePath = &javaIcon;
		}
		else if (filesystem::exists(bedrockIcon)) {
			sourcePath = &bedrockIcon;
		}
		if (sourcePath == nullptr) {
			return;
		}

#ifdef _WIN32
		const string loadPath = utf8_to_gbk(wstring_to_utf8(sourcePath->wstring()));
#else
		const string loadPath = wstring_to_utf8(sourcePath->wstring());
#endif
		unsigned int textureId = 0;
		int textureWidth = 0;
		int textureHeight = 0;
		if (LoadTextureFromFileGL(loadPath.c_str(), &textureId, &textureWidth, &textureHeight) &&
			textureId > 0) {
			textures[iconKey] = textureId;
		}
	}
}

std::vector<DisplayWorld> BuildDisplayWorldsForSelection()
{
	std::lock_guard<std::mutex> lock(g_appState.configsMutex);
	return BuildDisplayWorlds(g_appState.configs, g_appState.currentConfigIndex);
}

void DrawWorldListUiFrame(const MainUiFrameContext& context)
{
	auto* desktopServices = context.desktopServices;
	const AppPaths& paths = *context.paths;
	auto& g_worldIconTextures = WorldIconTextures();
MainUiController& mainUi = GetMainUiController();
WorldListController& worldUi = mainUi.worldList;
auto& showAboutWindow = mainUi.showAboutWindow;
auto& showImportConfigConfirm = mainUi.showImportConfigConfirm;
auto& showImportHistoryConfirm = mainUi.showImportHistoryConfirm;
auto& pendingImportPath = mainUi.pendingImportPath;
auto& waitingForHotkey = mainUi.waitingForHotkey;
auto& whichFunc = mainUi.selectedNoticeAction;
auto& open_update_popup = mainUi.openUpdatePopup;
auto& notice_popup_opened = mainUi.noticePopupOpened;
auto& notice_snoozed_this_session = mainUi.noticeSnoozedThisSession;
auto& tempRememberChoice = mainUi.rememberNoticeChoice;
auto& first_time_layout = mainUi.firstDockLayout;
auto& selectedWorldIndex = worldUi.selectedWorldIndex;
auto& displayWorlds = worldUi.displayWorlds;
auto& cachedConfigIndex = worldUi.cachedConfigIndex;
auto& cachedSpecialSetting = worldUi.cachedSpecialSetting;
auto& cachedWorldCount = worldUi.cachedWorldCount;
auto& lastDisplayWorldsRefresh = worldUi.lastDisplayWorldsRefresh;
auto& cachedOpenTimes = worldUi.cachedOpenTimes;
auto& cachedBackupTimes = worldUi.cachedBackupTimes;
auto& cachedNeedsBackup = worldUi.cachedNeedsBackup;
auto& lastTimeCacheRefresh = worldUi.lastTimeCacheRefresh;
auto& cachedTaskRunning = worldUi.cachedTaskRunning;
auto& showAddConfigPopup = worldUi.showAddConfigPopup;
auto& showDeleteConfigPopup = worldUi.showDeleteConfigPopup;
auto& tempExportConfig = worldUi.temporaryExportConfig;
auto& selectedBlacklistItem = worldUi.selectedBlacklistItem;
auto& selectedFormat = worldUi.selectedFormat;
char (&backupComment)[1024] = worldUi.backupComment;
char (&new_config_name)[128] = worldUi.newConfigName;
char (&mods_comment)[256] = worldUi.modsComment;
char (&buf)[1024] = worldUi.pathBuffer;
char (&others_comment)[1024] = worldUi.othersComment;
char (&outputPathBuf)[260] = worldUi.outputPath;
char (&descBuf)[2048] = worldUi.description;
char (&blacklistAddItemBuf)[1024] = worldUi.blacklistItem;
	ImGuiViewport* viewport = ImGui::GetMainViewport();
// 获取当前配置
if (!g_appState.configs.count(g_appState.currentConfigIndex)) { // 找不到，说明应该对应的是特殊配置
	specialSetting = true;
}

float totalW = ImGui::GetContentRegionAvail().x;
float leftW = totalW * 0.32f;
float midW = totalW * 0.25f;
float rightW = totalW * 0.42f;
// 缓存 DisplayWorlds，避免每帧重建（深拷贝 Config + mutex lock）
auto now_dw = chrono::steady_clock::now();
bool needsRebuild = (cachedConfigIndex != g_appState.currentConfigIndex)
	|| (cachedSpecialSetting != specialSetting)
	|| (chrono::duration_cast<chrono::milliseconds>(now_dw - lastDisplayWorldsRefresh).count() > 2000);
{
	// 配置变了或者两秒没更新了，并且当前配置是普通配置
	lock_guard<mutex> lock(g_appState.configsMutex);
	if (!specialSetting && g_appState.configs.count(g_appState.currentConfigIndex)) {
		if (g_appState.configs[g_appState.currentConfigIndex].worlds.size() != cachedWorldCount)
			needsRebuild = true;
	}
}
if (needsRebuild) {
	displayWorlds = BuildDisplayWorldsForSelection();
	cachedConfigIndex = g_appState.currentConfigIndex;
	cachedSpecialSetting = specialSetting;
	cachedWorldCount = displayWorlds.size();
	lastDisplayWorldsRefresh = now_dw;
}
int worldCount = (int)displayWorlds.size();

// 缓存 GetLastOpenTime / GetLastBackupTime，每5秒刷新一次
auto now_tc = chrono::steady_clock::now();
bool refreshTimeCache = chrono::duration_cast<chrono::seconds>(now_tc - lastTimeCacheRefresh).count() >= 5;
if (refreshTimeCache || needsRebuild) {
	cachedOpenTimes.clear();
	cachedBackupTimes.clear();
	cachedNeedsBackup.clear();
	for (int i = 0; i < worldCount; ++i) {
		const auto& dw_t = displayWorlds[i];
		wstring wf = JoinPath(dw_t.effectiveConfig.saveRoot, dw_t.name).wstring();
		wstring bf = JoinPath(dw_t.effectiveConfig.backupPath, dw_t.name).wstring();
		wstring ot = GetLastOpenTime(wf);
		wstring bt = GetLastBackupTime(bf);
		cachedOpenTimes[wf] = ot;
		cachedBackupTimes[bf] = bt;
		cachedNeedsBackup[wf] = (ot > bt);
	}
	lastTimeCacheRefresh = now_tc;
}

// 一次性获取所有任务运行状态
{
	lock_guard<mutex> taskLock(g_appState.task_mutex);
	cachedTaskRunning.clear();
	for (int i = 0; i < worldCount; ++i) {
		auto key = make_pair(displayWorlds[i].baseConfigIndex, i);
		cachedTaskRunning[key] = g_appState.g_active_auto_backups.count(key) > 0;
	}
}


if (ImGui::Begin(L("WORLD_LIST"))) {
	ImGui::SeparatorText(L("QUICK_CONFIG_SWITCHER"));
	ImGui::SetNextItemWidth(-1);
	string current_config_label = "None";
	if (g_appState.configs.count(g_appState.currentConfigIndex)) {
		current_config_label = "[No." + to_string(g_appState.currentConfigIndex) + "] " + g_appState.configs[g_appState.currentConfigIndex].name;
	}

	if (ImGui::BeginCombo("##ConfigSwitcher", current_config_label.c_str())) {
		// 普通配置
		for (auto const& [idx, val] : g_appState.configs) {
			const bool is_selected = (g_appState.currentConfigIndex == idx);
			string label = "[No." + to_string(idx) + "] " + val.name;

			if (ImGui::Selectable(label.c_str(), is_selected)) {
				g_appState.currentConfigIndex = idx;
				specialSetting = false;
			}
			if (is_selected) {
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::Separator();
		if (ImGui::Selectable(L("BUTTON_ADD_CONFIG"))) {
			showAddConfigPopup = true;
		}

		if (ImGui::Selectable(L("BUTTON_DELETE_CONFIG"))) {
			if (g_appState.configs.size() > 1) { // 至少保留一个
				showDeleteConfigPopup = true;
			}
		}


		ImGui::EndCombo();
	}

	// 删除配置弹窗
	if (showDeleteConfigPopup)
		ImGui::OpenPopup(L("CONFIRM_DELETE_TITLE"));
	ImGui::SetNextWindowViewport(viewport->ID);
	if (ImGui::BeginPopupModal(L("CONFIRM_DELETE_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		showDeleteConfigPopup = false;
		ImGui::Text(L("CONFIRM_DELETE_MSG"), g_appState.currentConfigIndex, g_appState.configs[g_appState.currentConfigIndex].name.c_str());
		ImGui::Separator();
		float delConfirmBtnWidth = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"));

		if (ImGui::Button(L("BUTTON_OK"), ImVec2(delConfirmBtnWidth, 0))) {
			g_appState.configs.erase(g_appState.currentConfigIndex);
			g_appState.currentConfigIndex = g_appState.configs.begin()->first;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(delConfirmBtnWidth, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	// 添加新配置弹窗

	if (showAddConfigPopup)
		ImGui::OpenPopup(L("ADD_NEW_CONFIG_POPUP_TITLE"));


	ImGui::SetNextWindowViewport(viewport->ID);
	if (ImGui::BeginPopupModal(L("ADD_NEW_CONFIG_POPUP_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		showAddConfigPopup = false;

		ImGui::TextWrapped("%s", L("CONFIG_TYPE_NORMAL_DESC"));

		ImGui::InputText(L("NEW_CONFIG_NAME_LABEL"), new_config_name, IM_ARRAYSIZE(new_config_name));
		ImGui::Separator();

		float createBtnWidth = CalcPairButtonWidth(L("CREATE_BUTTON"), L("BUTTON_CANCEL"));
		if (ImGui::Button(L("CREATE_BUTTON"), ImVec2(createBtnWidth, 0))) {
			if (strlen(new_config_name) > 0) {
				int new_index = CreateNewNormalConfig(new_config_name);
				// 继承当前配置（如果有），但保留路径为空
				if (g_appState.configs.count(g_appState.currentConfigIndex)) {
					g_appState.configs[new_index] = g_appState.configs[g_appState.currentConfigIndex];
					AssignFreshNormalConfigId(new_index);
					g_appState.configs[new_index].name = new_config_name;
					g_appState.configs[new_index].saveRoot.clear();
					g_appState.configs[new_index].backupPath.clear();
					g_appState.configs[new_index].worlds.clear();
					EnsureDefaultBackupBlacklist(g_appState.configs[new_index].blacklist);
					EnsureDefaultRestoreWhitelist();
				}
				g_appState.currentConfigIndex = new_index;
				specialSetting = false;
				showSettings = true; // Open detailed settings for the new config
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(createBtnWidth, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	ImGui::SeparatorText(L("WORLD_LIST"));

	// Preserve eager icon loading while allowing Dear ImGui to skip
	// constructing rows outside the visible child-window range.
	for (const auto& world : displayWorlds) {
		EnsureWorldIconLoaded(JoinPath(world.effectiveConfig.saveRoot, world.name));
	}

	ImGui::BeginChild("WorldListChild", ImVec2(0, 0), true);

	ImGuiListClipper worldClipper;
	worldClipper.Begin(worldCount);
	if (selectedWorldIndex >= 0 && selectedWorldIndex < worldCount) {
		worldClipper.IncludeItemByIndex(selectedWorldIndex);
	}
	while (worldClipper.Step()) {
	for (int i = worldClipper.DisplayStart; i < worldClipper.DisplayEnd; ++i) {
		const auto& dw = displayWorlds[i];
		ImGui::PushID(i);
		bool is_selected = (selectedWorldIndex == i);

		// worldFolder / backupFolder 基于 effectiveConfig - 使用跨平台路径拼接
		wstring worldFolder = JoinPath(dw.effectiveConfig.saveRoot, dw.name).wstring();
		wstring backupFolder = JoinPath(dw.effectiveConfig.backupPath, dw.name).wstring();

		// --- 左侧图标区 ---
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		float iconSz = ImGui::GetTextLineHeightWithSpacing() * 2.5f;
		ImVec2 icon_pos = ImGui::GetCursorScreenPos();
		ImVec2 icon_end_pos = ImVec2(icon_pos.x + iconSz, icon_pos.y + iconSz);

		// 绘制占位符和边框
		draw_list->AddRectFilled(icon_pos, icon_end_pos, IM_COL32(50, 50, 50, 200), 4.0f);
		draw_list->AddRect(icon_pos, icon_end_pos, IM_COL32(200, 200, 200, 200), 4.0f);


		wstring iconKey = worldFolder;

		// 渲染逻辑
		GLuint current_texture = g_worldIconTextures[iconKey];
		if (current_texture > 0) {
			ImGui::Image(ImTextureRef(static_cast<ImTextureID>(current_texture)), ImVec2(iconSz, iconSz));
		}
		else {
			const char* placeholder_icon = ICON_FA_FOLDER;
			ImVec2 text_size = ImGui::CalcTextSize(placeholder_icon);
			ImVec2 text_pos = ImVec2(icon_pos.x + (iconSz - text_size.x) * 0.5f, icon_pos.y + (iconSz - text_size.y) * 0.5f);
			draw_list->AddText(text_pos, IM_COL32(200, 200, 200, 255), placeholder_icon);
		}


		// 将光标移过图标区域
		ImGui::Dummy(ImVec2(iconSz, iconSz));

		ImGui::SetCursorScreenPos(icon_pos);
		ImGui::InvisibleButton("##icon_button", ImVec2(iconSz, iconSz));
		// 点击更换图标
		if (ImGui::IsItemClicked()) {
			wstring sel = desktopServices->SelectFile().path.wstring();
			if (!sel.empty()) {
				// 覆盖原 icon.png - 使用跨平台路径拼接
				wstring destPath = JoinPath(worldFolder, L"icon.png").wstring();
				CopyFileW(sel.c_str(), destPath.c_str(), FALSE);
				// 释放旧纹理并重新加载
				if (current_texture) {
					glDeleteTextures(1, &current_texture);
				}
				GLuint newTextureId = 0;
				int tex_w = 0, tex_h = 0;
#ifdef _WIN32
				LoadTextureFromFileGL(utf8_to_gbk(wstring_to_utf8(destPath)).c_str(), &newTextureId, &tex_w, &tex_h);
#else
				LoadTextureFromFileGL(wstring_to_utf8(destPath).c_str(), &newTextureId, &tex_w, &tex_h);
#endif
				g_worldIconTextures[iconKey] = newTextureId;
			}
		}

		ImGui::SameLine();
		// --- 状态逻辑 (使用预计算缓存，避免每帧每项加锁和文件IO)
		bool is_task_running = cachedTaskRunning[make_pair(displayWorlds[i].baseConfigIndex, i)];
		bool needs_backup = false;
		{
			auto it = cachedNeedsBackup.find(worldFolder);
			if (it != cachedNeedsBackup.end()) needs_backup = it->second;
		}

		// 整个区域作为一个可选项
		// ImGuiSelectableFlags_AllowItemOverlap 允许我们在可选项上面绘制其他控件
		if (ImGui::Selectable("##world_selectable", is_selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2.5f))) {
			selectedWorldIndex = i;
		}

		ImVec2 p_min = ImGui::GetItemRectMin();
		ImVec2 p_max = ImGui::GetItemRectMax();

		// --- 卡片背景和高亮 ---
		if (ImGui::IsItemHovered()) {
			draw_list->AddRectFilled(p_min, p_max, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
		}
		else if (is_selected) {
			draw_list->AddRectFilled(p_min, p_max, ImGui::GetColorU32(ImGuiCol_FrameBgActive, 0.5f), 4.0f);
		}

		if (is_selected) {
			draw_list->AddRect(p_min, p_max, ImGui::GetColorU32(ImGuiCol_ButtonActive), 4.0f, 2.0f);
		}

		// 我们在可选项的相同位置开始绘制我们的自定义内容
		ImGui::SameLine();
		ImGui::BeginGroup(); // 将所有内容组合在一起

		// --- 第一行：世界名和描述 ---
		string name_utf8 = wstring_to_utf8(dw.name);
		string desc_utf8 = wstring_to_utf8(dw.desc);
		const float worldTextWidth =
			(max)(ImGui::GetContentRegionAvail().x - 48.0f * g_uiScale, 1.0f);
		TextEllipsisWithTooltip(name_utf8.c_str(), worldTextWidth);


		const ImVec4 disabledColor =
			ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
		if (desc_utf8.empty()) {
			TextEllipsisWithTooltip(

				L("CARD_WORLD_NO_DESC"),
				worldTextWidth,
				&disabledColor);
		}
		else {
			TextEllipsisWithTooltip(
				desc_utf8.c_str(),
				worldTextWidth,
				&disabledColor);
		}

		ImGui::EndGroup();

		// --- 右侧的状态图标 ---
		float icon_pane_width = 40.0f;
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - icon_pane_width);
		ImGui::BeginGroup();
		ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 0.25f)); // 垂直居中一点
		if (is_task_running) {
			ImGui::PushStyleColor(ImGuiCol_Text, ThemePalette::GetStatusColor(ThemePalette::StatusColor::Info)); // 蓝色
			ImGui::Text(ICON_FA_ROTATE); // 旋转图标，表示正在运行

			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TOOLTIP_AUTOBACKUP_RUNNING"));

			ImGui::PopStyleColor();
		}
		else if (needs_backup) {
			ImGui::PushStyleColor(ImGuiCol_Text, ThemePalette::GetStatusColor(ThemePalette::StatusColor::Warning)); // 黄色
			ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION); // 警告图标
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TOOLTIP_NEEDS_BACKUP"));
			ImGui::PopStyleColor();
		}
		else {
			ImGui::PushStyleColor(ImGuiCol_Text, ThemePalette::GetStatusColor(ThemePalette::StatusColor::Success)); // 绿色
			ImGui::Text(ICON_FA_CIRCLE_CHECK); // 对勾图标
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TOOLTIP_UP_TO_DATE"));
			ImGui::PopStyleColor();
		}
		ImGui::EndGroup();


		ImGui::PopID();
		ImGui::Separator();
	}
	}

	ImGui::EndChild(); // 结束 WorldListChild

}
ImGui::End();

if (ImGui::Begin(L("WORLD_DETAILS_PANE_TITLE"))) {
	if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size()) {
		ImGui::SameLine();
		{
			ImGui::SeparatorText(L("CURRENT_CONFIG_INFO"));

			ImGui::Text("%s: %s", L("SAVES_PATH_LABEL"), wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot).c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot).c_str());
			ImGui::Text("%s: %s", L("BACKUP_PATH_LABEL"), wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath).c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath).c_str());

			ImGui::SeparatorText(L("WORLD_DETAILS_PANE_TITLE"));
			ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
			ImGui::Text("%s", wstring_to_utf8(displayWorlds[selectedWorldIndex].name).c_str());
			ImGui::PopTextWrapPos();
			ImGui::Separator();

			// -- 详细信息 --
			wstring worldFolder = JoinPath(displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot, displayWorlds[selectedWorldIndex].name).wstring();
			wstring backupFolder = JoinPath(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath, displayWorlds[selectedWorldIndex].name).wstring();
			{
				auto otIt = cachedOpenTimes.find(worldFolder);
				auto btIt = cachedBackupTimes.find(backupFolder);
				wstring openTimeStr = (otIt != cachedOpenTimes.end()) ? otIt->second : GetLastOpenTime(worldFolder);
				wstring backupTimeStr = (btIt != cachedBackupTimes.end()) ? btIt->second : GetLastBackupTime(backupFolder);
				ImGui::Text("%s: %s", L("TABLE_LAST_OPEN"), wstring_to_utf8(openTimeStr).c_str());
				ImGui::Text("%s: %s", L("TABLE_LAST_BACKUP"), wstring_to_utf8(backupTimeStr).c_str());
			}

			ImGui::Separator();

			// -- 注释输入框 --if (ImGui::InputText(L("WORLD_DESC"), desc, CONSTANT2))
			char buffer[CONSTANT1] = "";
			// 增加检查，确保 selectedWorldIndex 仍然有效
			if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size()) {
				const auto& dw = displayWorlds[selectedWorldIndex];
				wstring desc = dw.desc;
				strncpy_s(buffer, wstring_to_utf8(desc).c_str(), sizeof(buffer));
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::InputTextWithHint("##backup_desc", L("HINT_BACKUP_DESC"), buffer, IM_ARRAYSIZE(buffer), ImGuiInputTextFlags_EnterReturnsTrue);

				// 在写入前，再次进行完整的检查
				if (g_appState.configs.count(dw.baseConfigIndex)) {
					Config& cfg = g_appState.configs.at(dw.baseConfigIndex);
					if (dw.baseWorldIndex >= 0 && dw.baseWorldIndex < cfg.worlds.size()) {
						if (desc.find(L"\"") != wstring::npos || desc.find(L":") != wstring::npos || desc.find(L"\\") != wstring::npos || desc.find(L"/") != wstring::npos || desc.find(L">") != wstring::npos || desc.find(L"<") != wstring::npos || desc.find(L"|") != wstring::npos || desc.find(L"?") != wstring::npos || desc.find(L"*") != wstring::npos) {
							memset(buffer, '\0', sizeof(buffer));
							cfg.worlds[dw.baseWorldIndex].second = L"";
						}
						else {
							cfg.worlds[dw.baseWorldIndex].second = utf8_to_wstring(buffer);
						}
					}
				}
			}
			else {
				// 如果索引无效，显示一个禁用的占位输入框
				strcpy_s(buffer, "N/A");
				ImGui::BeginDisabled();
				ImGui::InputTextWithHint("##backup_desc", L("HINT_BACKUP_DESC"), buffer, IM_ARRAYSIZE(buffer));
				ImGui::EndDisabled();
			}

			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			ImGui::InputTextWithHint("##backup_comment", L("HINT_BACKUP_COMMENT"), backupComment, IM_ARRAYSIZE(backupComment), ImGuiInputTextFlags_EnterReturnsTrue);

			// -- 主要操作按钮 --
			float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
			if (ImGui::Button(L("BUTTON_BACKUP_SELECTED"), ImVec2(button_width, 0))) {
				MyFolder world = { JoinPath(displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot, displayWorlds[selectedWorldIndex].name).wstring(), displayWorlds[selectedWorldIndex].name, displayWorlds[selectedWorldIndex].desc, displayWorlds[selectedWorldIndex].effectiveConfig, displayWorlds[selectedWorldIndex].baseConfigIndex, selectedWorldIndex };
				TaskCoordinator::Instance().Submit(L"manual-backup",
					{TaskCoordinator::WorldResourceKey(world.config.configId, world.path)},
					[world, comment = utf8_to_wstring(backupComment)](stop_token) { DoBackup(world, comment); });
				strcpy_s(backupComment, "");
			}
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_AUTO_BACKUP_SELECTED"), ImVec2(button_width, 0))) {
				ImGui::OpenPopup(L("AUTOBACKUP_SETTINGS"));
			}

			if (ImGui::Button(L("HISTORY_BUTTON"), ImVec2(-1, 0))) {
				g_worldToFocusInHistory = displayWorlds[selectedWorldIndex].name; // 设置要聚焦的世界
				showHistoryWindow = true; // 打开历史窗口
			}
			if (ImGui::Button(L("BUTTON_HIDE_WORLD"), ImVec2(-1, 0))) {
				// 先做最小范围的本地检查并拷贝要操作的 DisplayWorld（displayWorlds 是本地变量）
				if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size()) {
					DisplayWorld dw_copy = displayWorlds[selectedWorldIndex]; // 做一个值拷贝，之后在锁内用索引去改 g_appState.configs

					bool did_change = false;

					// 在修改全局 g_appState.configs 前加锁，防止其它线程并发读/写导致崩溃
					{
						lock_guard<mutex> cfg_lock(g_appState.configsMutex);

						auto it = g_appState.configs.find(dw_copy.baseConfigIndex);
						if (it != g_appState.configs.end()) {
							Config& cfg = it->second;
							if (dw_copy.baseWorldIndex >= 0 && dw_copy.baseWorldIndex < (int)cfg.worlds.size()) {
								cfg.worlds[dw_copy.baseWorldIndex].second = L"#";
								did_change = true;
							}
						}
					} // 解锁 g_appState.configsMutex
				}
			}

			if (ImGui::Button(L("BUTTON_PIN_WORLD"), ImVec2(-1, 0))) {
				// 检查索引是否有效且不是第一个
				if (selectedWorldIndex > 0 && selectedWorldIndex < displayWorlds.size()) {
					DisplayWorld& dw = displayWorlds[selectedWorldIndex];
					int configIdx = dw.baseConfigIndex;
					int worldIdx = dw.baseWorldIndex;

					// 确保我们操作的是普通配置中的世界列表
					if (!specialSetting && g_appState.configs.count(configIdx)) {
						Config& cfg = g_appState.configs[configIdx];
						if (worldIdx < cfg.worlds.size()) {
							// 存储要移动的世界
							pair<wstring, wstring> worldToMove = cfg.worlds[worldIdx];

							// 从原位置删除
							cfg.worlds.erase(cfg.worlds.begin() + worldIdx);

							// 插入到列表顶部
							cfg.worlds.insert(cfg.worlds.begin(), worldToMove);

							// 更新选中项为新的顶部项
							selectedWorldIndex = 0;
						}
					}
				}
			}
			if (ImGui::Button(L("OPEN_BACKUP_FOLDER"), ImVec2(-1, 0))) {
				wstring path = JoinPath(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath, displayWorlds[selectedWorldIndex].name).wstring();
				if (filesystem::exists(path)) {
					(void)desktopServices->OpenFolder(path);
				}
				else {
					(void)desktopServices->OpenFolder(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath);
				}
			}
			if (ImGui::Button(L("OPEN_SAVEROOT_FOLDER"), ImVec2(-1, 0))) {
				wstring path = JoinPath(displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot, displayWorlds[selectedWorldIndex].name).wstring();
				(void)desktopServices->OpenFolder(path);
			}

			// 模组备份
			if (ImGui::Button(L("BUTTON_BACKUP_MODS"), ImVec2(-1, 0))) {
				if (selectedWorldIndex != -1) {
					ImGui::OpenPopup(L("CONFIRM_BACKUP_OTHERS_TITLE"));
				}
			}

			ImGui::SetNextWindowViewport(viewport->ID);
			if (ImGui::BeginPopupModal(L("CONFIRM_BACKUP_OTHERS_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {

				ImGui::TextUnformatted(L("CONFIRM_BACKUP_OTHERS_MSG"));
				ImGui::InputText(L("HINT_BACKUP_COMMENT"), mods_comment, IM_ARRAYSIZE(mods_comment));
				ImGui::Separator();

				float modsConfirmBtnWidth = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"));
				if (ImGui::Button(L("BUTTON_OK"), ImVec2(modsConfirmBtnWidth, 0))) {
					if (g_appState.configs.count(g_appState.currentConfigIndex)) {
						filesystem::path tempPath = displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot;
						filesystem::path modsPath = tempPath.parent_path() / "mods";
						if (!filesystem::exists(modsPath) && filesystem::exists(tempPath / "mods")) { // 服务器的模组可能放在world同级文件夹下
							modsPath = tempPath / "mods";
						}
						const Config configCopy = g_appState.configs[g_appState.currentConfigIndex];
						TaskCoordinator::Instance().Submit(L"mods-backup",
							{TaskCoordinator::WorldResourceKey(configCopy.configId, modsPath)},
							[configCopy, modsPath, comment = utf8_to_wstring(mods_comment)](stop_token) {
								DoOthersBackup(configCopy, modsPath, comment);
							});
						strcpy_s(mods_comment, "");
					}
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();


				if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(modsConfirmBtnWidth, 0))) {
					strcpy_s(mods_comment, "");
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}

			// 其他备份
			float availWidth = ImGui::GetContentRegionAvail().x;
			float btnWidth = ImGui::CalcTextSize(L("BUTTON_BACKUP_OTHERS")).x + ImGui::GetStyle().FramePadding.x * 2;
			const string otherBackupPopupTitle = string(L("BACKUP_OTHER_POPUP_TITLE")) + "###OtherBackup";
			if (ImGui::Button(L("BUTTON_BACKUP_OTHERS"), ImVec2(btnWidth, 0))) {
				if (selectedWorldIndex != -1) {
					ImGui::OpenPopup(otherBackupPopupTitle.c_str());
				}
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth((availWidth - btnWidth) * 0.97f);
			// 可以输入需要备份的其他内容的路径，比如 D:\Games\g_appState.configs
			strcpy_s(buf, wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.othersPath).c_str());
			if (ImGui::InputTextWithHint("##OTHERS", L("HINT_BACKUP_WHAT"), buf, IM_ARRAYSIZE(buf))) {
				displayWorlds[selectedWorldIndex].effectiveConfig.othersPath = utf8_to_wstring(buf);
				g_appState.configs[displayWorlds[selectedWorldIndex].baseConfigIndex].othersPath = displayWorlds[selectedWorldIndex].effectiveConfig.othersPath;
			}

			ImGui::SetNextWindowViewport(viewport->ID);
			if (ImGui::BeginPopupModal(otherBackupPopupTitle.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::TextUnformatted(L("CONFIRM_BACKUP_OTHERS_MSG"));
				ImGui::InputText(L("HINT_BACKUP_COMMENT"), others_comment, IM_ARRAYSIZE(others_comment));
				ImGui::Separator();

				float othersConfirmBtnWidth = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"));
				if (ImGui::Button(L("BUTTON_OK"), ImVec2(othersConfirmBtnWidth, 0))) {
					const Config configCopy = displayWorlds[selectedWorldIndex].effectiveConfig;
					const wstring othersPath = utf8_to_wstring(buf);
					TaskCoordinator::Instance().Submit(L"other-path-backup",
						{TaskCoordinator::WorldResourceKey(configCopy.configId, othersPath)},
						[configCopy, othersPath, comment = utf8_to_wstring(others_comment)](stop_token) {
							DoOthersBackup(configCopy, othersPath, comment);
						});
					strcpy_s(others_comment, "");
					SaveConfigs(); // 保存一下路径
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(othersConfirmBtnWidth, 0))) {
					strcpy_s(others_comment, "");
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}


			if (ImGui::Button(L("CLOUD_SYNC_BUTTOM"), ImVec2(-1, 0))) {
				const int baseConfigIndex = displayWorlds[selectedWorldIndex].baseConfigIndex;
				const Config configCopy = g_appState.configs[baseConfigIndex];
				const wstring worldName = displayWorlds[selectedWorldIndex].name;
				if (CanUseCloudActions(configCopy)) {
					TaskCoordinator::Instance().Submit(L"manual-cloud-upload",
						{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
						[configCopy, baseConfigIndex, worldName](stop_token) {
						UploadWorldBackupFolderToCloud(configCopy, baseConfigIndex, worldName);
					});
				}
				else {
					MB_LOG_I18N_WARNING(minebackup::logging::LogCategory::Cloud,
						"cloud.configuration.invalid", "CLOUD_SYNC_INVALID");
				}
			}

			// 导出分享
			if (ImGui::Button(L("BUTTON_EXPORT_FOR_SHARING"), ImVec2(-1, 0))) {
				if (selectedWorldIndex != -1) {
					ImGui::OpenPopup(L("EXPORT_WINDOW_TITLE"));
				}
			}
			ImGui::SetNextWindowViewport(viewport->ID);
			if (ImGui::BeginPopupModal(L("EXPORT_WINDOW_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
				// 弹窗工作副本由 WorldListController 持有。

				// 弹窗首次打开时，进行初始化
				if (ImGui::IsWindowAppearing()) {
					const auto& dw = displayWorlds[selectedWorldIndex];
					tempExportConfig = dw.effectiveConfig; // 复制当前配置作为基础

					// 默认导出到配置档数据目录，不依赖启动工作目录。
					const auto exportRoot = paths.dataRoot / L"exports";
					error_code exportDirectoryError;
					filesystem::create_directories(exportRoot, exportDirectoryError);
					wstring cleanWorldName = SanitizeFileName(dw.name);
					wstring finalPath = (exportRoot / (cleanWorldName + L"_shared." + tempExportConfig.zipFormat)).wstring();
					strncpy_s(outputPathBuf, wstring_to_utf8(finalPath).c_str(), sizeof(outputPathBuf));

					// 预设默认黑名单
					tempExportConfig.blacklist.clear();
					tempExportConfig.blacklist.push_back(L"playerdata");
					tempExportConfig.blacklist.push_back(L"stats");
					tempExportConfig.blacklist.push_back(L"advancements");
					tempExportConfig.blacklist.push_back(L"session.lock");
					tempExportConfig.blacklist.push_back(L"level.dat_old");


					// 清空上次的输入
					memset(descBuf, 0, sizeof(descBuf));
					memset(blacklistAddItemBuf, 0, sizeof(blacklistAddItemBuf));
					selectedBlacklistItem = -1;
				}


				// --- UI 渲染 ---
				ImGui::SeparatorText(L("GROUP_EXPORT_OPTIONS"));
				ImGui::InputText(L("LABEL_EXPORT_PATH"), outputPathBuf, sizeof(outputPathBuf));
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_BROWSE"))) {
					const auto selectedFolder = desktopServices->SelectFolder();
					if (!selectedFolder.path.empty()) {
						const auto destination = selectedFolder.path
							/ (displayWorlds[selectedWorldIndex].name + L"_shared." + tempExportConfig.zipFormat);
						strcpy_s(outputPathBuf, MAX_PATH,
							wstring_to_utf8(destination.wstring()).c_str());
					}
				}

				if (ImGui::RadioButton("7z", &selectedFormat, 0)) { tempExportConfig.zipFormat = L"7z"; } ImGui::SameLine();
				if (ImGui::RadioButton("zip", &selectedFormat, 1)) { tempExportConfig.zipFormat = L"zip"; }

				ImGui::SeparatorText(L("GROUP_EXPORT_BLACKLIST"));
				ImGui::BeginChild("BlacklistChild", ImVec2(0, 150), true);
				for (int i = 0; i < tempExportConfig.blacklist.size(); ++i) {
					if (ImGui::Selectable(wstring_to_utf8(tempExportConfig.blacklist[i]).c_str(), selectedBlacklistItem == i)) {
						selectedBlacklistItem = i;
					}
				}
				ImGui::EndChild();

				if (ImGui::Button(L("BUTTON_REMOVE_SELECTED")) && selectedBlacklistItem != -1) {
					tempExportConfig.blacklist.erase(tempExportConfig.blacklist.begin() + selectedBlacklistItem);
					selectedBlacklistItem = -1;
				}
				ImGui::InputTextWithHint("##AddItem", L("HINT_ADD_BLACKLIST_ITEM"), blacklistAddItemBuf, sizeof(blacklistAddItemBuf));
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_ADD")) && strlen(blacklistAddItemBuf) > 0) {
					tempExportConfig.blacklist.push_back(utf8_to_wstring(blacklistAddItemBuf));
					memset(blacklistAddItemBuf, 0, sizeof(blacklistAddItemBuf));
				}


				ImGui::SeparatorText(L("GROUP_EXPORT_DESCRIPTION"));
				ImGui::InputTextMultiline("##Desc", descBuf, sizeof(descBuf), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4), ImGuiInputTextFlags_AllowTabInput);

				ImGui::Separator();
				float exportBtnWidth = CalcPairButtonWidth(L("BUTTON_EXPORT"), L("BUTTON_CANCEL"));
				if (ImGui::Button(L("BUTTON_EXPORT"), ImVec2(exportBtnWidth, 0))) {
					const auto& dw = displayWorlds[selectedWorldIndex];

					wstring worldFullPath = JoinPath(dw.effectiveConfig.saveRoot, dw.name).wstring();
					const Config exportConfig = tempExportConfig;

					TaskCoordinator::Instance().Submit(L"export-for-sharing",
						{TaskCoordinator::WorldResourceKey(exportConfig.configId, worldFullPath)},
						[exportConfig, worldName = dw.name, worldFullPath,
						 outputPath = utf8_to_wstring(outputPathBuf), description = utf8_to_wstring(descBuf)](stop_token) {
							DoExportForSharing(exportConfig, worldName, worldFullPath, outputPath, description);
						});

					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(exportBtnWidth, 0))) {
					ImGui::CloseCurrentPopup();
				}

				ImGui::EndPopup();
			}


		}


		// 自动备份弹窗
		ImGui::SetNextWindowViewport(viewport->ID);
		if (ImGui::BeginPopupModal(L("AUTOBACKUP_SETTINGS"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			bool is_task_running = false;
			pair<int, int> taskKey = { -1,-1 };
			vector<DisplayWorld> localDisplayWorlds = displayWorlds; // 供显示使用，避免每帧重建
			{
				lock_guard<mutex> lock(g_appState.task_mutex);
				if (selectedWorldIndex >= 0) {
					if (selectedWorldIndex < (int)localDisplayWorlds.size()) {
						taskKey = { localDisplayWorlds[selectedWorldIndex].baseConfigIndex, localDisplayWorlds[selectedWorldIndex].baseWorldIndex };
						is_task_running = (g_appState.g_active_auto_backups.count(taskKey) > 0);
					}
				}
			}

			if (is_task_running) {
				ImGui::Text(L("AUTOBACKUP_RUNNING"), wstring_to_utf8(localDisplayWorlds[selectedWorldIndex].name).c_str());
				ImGui::Separator();
				if (ImGui::Button(L("BUTTON_STOP_AUTOBACKUP"), ImVec2(CalcButtonWidth(L("BUTTON_STOP_AUTOBACKUP")), 0))) {
					wstring taskName;
					{
						lock_guard<mutex> lock(g_appState.task_mutex);
						auto it = g_appState.g_active_auto_backups.find(taskKey);


						if (it != g_appState.g_active_auto_backups.end()) {
							taskName = it->second.taskName;
							g_appState.g_active_auto_backups.erase(it);
						}
					}
					TaskCoordinator::Instance().RequestStop(taskName);
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(CalcButtonWidth(L("BUTTON_CANCEL")), 0))) {
					ImGui::CloseCurrentPopup();
				}
			}
			else {
				if (selectedWorldIndex < 0 || selectedWorldIndex >= (int)localDisplayWorlds.size()) {
					ImGui::TextDisabled("%s", L("PROMPT_SELECT_WORLD"));
					if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(CalcButtonWidth(L("BUTTON_CANCEL")), 0))) {
						ImGui::CloseCurrentPopup();
					}
				}
				else {
					ImGui::Text(L("AUTOBACKUP_SETUP_FOR"), wstring_to_utf8(localDisplayWorlds[selectedWorldIndex].name).c_str());
					ImGui::Separator();
					ImGui::InputInt(L("INTERVAL_MINUTES"), &last_interval);
					if (last_interval < 1) last_interval = 1;
					float autoBkpBtnWidth = CalcPairButtonWidth(L("BUTTON_START"), L("BUTTON_CANCEL"));
					if (ImGui::Button(L("BUTTON_START"), ImVec2(autoBkpBtnWidth, 0))) {
						// 注册并启动线程
						lock_guard<mutex> lock(g_appState.task_mutex);
						if (taskKey.first >= 0) {
							AutoBackupTask& task = g_appState.g_active_auto_backups[taskKey];
							task.taskName = TaskCoordinator::AutoBackupTaskName(taskKey.first, taskKey.second);
							const bool started = TaskCoordinator::Instance().Submit(task.taskName, {},
								[taskName = task.taskName, configIndex = taskKey.first, worldIndex = taskKey.second, interval = last_interval](stop_token token) {
									AutoBackupThreadFunction(configIndex, worldIndex, interval, token);
									TaskCoordinator::Instance().PostEvent({L"auto-backup-finished", taskName});
								});
							if (!started) g_appState.g_active_auto_backups.erase(taskKey);

							ImGui::CloseCurrentPopup();
						}
					}
					ImGui::SameLine();
					if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(autoBkpBtnWidth, 0))) {
						ImGui::CloseCurrentPopup();
					}
				}
			}
			ImGui::EndPopup();
		}
	}
	else {
		ImGui::SameLine();
		ImGui::SeparatorText(L("WORLD_DETAILS_PANE_TITLE"));
		ImVec2 window_size = ImGui::GetWindowSize();
		ImVec2 text_size = ImGui::CalcTextSize(L("PROMPT_SELECT_WORLD"));
		ImGui::SetCursorPos(ImVec2((window_size.x - text_size.x) * 0.5f, (window_size.y - text_size.y) * 0.5f));
		ImGui::TextDisabled("%s", L("PROMPT_SELECT_WORLD"));
	}
}
	ImGui::End();
}

void ReleaseWorldListUiResources()
{
	auto& textures = WorldIconTextures();
	for (const auto& [key, texture] : textures) {
		(void)key;
		if (texture > 0) {
			glDeleteTextures(1, &texture);
		}
	}
	textures.clear();
}
