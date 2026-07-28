// HistoryUI.cpp — 历史记录窗口
// 从 MineBackup.cpp 拆分出，包含 ShowHistoryWindow() 函数

#include "Globals.h"
#include "UIHelpers.h"
#include "imgui-all.h"
#include "imgui_style.h"
#include "i18n.h"
#include "AppState.h"
#include "IconsFontAwesome6.h"
#include "ConfigManager.h"
#include "text_to_text.h"
#include "HistoryManager.h"
#include "BackupManager.h"
#include "CloudSyncService.h"
#include "PlatformCompat.h"
#include "AppPaths.h"
#include "TaskCoordinator.h"
#include "DesktopServices.h"
#include "HistoryViewModel.h"

#include <optional>

using namespace std;

// 前向声明

static void ShowHistoryWindowLegacy(int& tempCurrentConfigIndex) {
	// 使用 (worldName, backupFile) 键值对标识选中项，避免指针悬垂
	static std::wstring sel_world, sel_file;   // 当前选中条目的键
	static std::wstring del_world, del_file;   // 待删除条目的键
	static ImGuiTextFilter filter;
	static char comment_buf[512];
	static string original_comment;
	static bool is_comment_editing = false;
	Config& cfg = g_appState.configs[tempCurrentConfigIndex];
	const float toolbarButtonMinWidth = 96.0f * g_uiScale;
	const float actionButtonMinWidth = 120.0f * g_uiScale;
	const float dialogButtonMinWidth = 132.0f * g_uiScale;
	const float compactButtonMinWidth = 110.0f * g_uiScale;

	// 每帧从键值解析出安全指针（如果条目已被删除/清理，自动失效为 nullptr）
	auto ResolveEntry = [&](const std::wstring& w, const std::wstring& f) -> HistoryEntry* {
		if (w.empty()) return nullptr;
		auto it = g_appState.g_history.find(tempCurrentConfigIndex);
		if (it == g_appState.g_history.end()) return nullptr;
		for (auto& e : it->second) {
			if (e.worldName == w && e.backupFile == f) return &e;
		}
		return nullptr;
	};
	HistoryEntry* selected_entry = ResolveEntry(sel_world, sel_file);
	if (!selected_entry) { sel_world.clear(); sel_file.clear(); }
	HistoryEntry* entry_to_delete = ResolveEntry(del_world, del_file);
	if (!entry_to_delete) { del_world.clear(); del_file.clear(); }

	ImGui::SetNextWindowSize(ImVec2(850, 600), ImGuiCond_FirstUseEver);

	if (!ImGui::Begin(L("HISTORY_WINDOW_TITLE"), &showHistoryWindow, ImGuiWindowFlags_NoDocking)) {
		ImGui::End();
		return;
	}

	// 当窗口关闭或配置改变时，重置选中项
	if (!showHistoryWindow || (selected_entry && g_appState.g_history.find(tempCurrentConfigIndex) == g_appState.g_history.end())) {
		sel_world.clear(); sel_file.clear(); selected_entry = nullptr;
		is_comment_editing = false;
	}

	// --- 顶部工具栏 ---
	filter.Draw(L("HISTORY_SEARCH_HINT"), ImGui::GetContentRegionAvail().x * 0.3f);
	ImGui::SameLine();
	if (ImGui::Button(L("HISTORY_CLEAN_INVALID"), ImVec2(CalcButtonWidth(L("HISTORY_CLEAN_INVALID"), toolbarButtonMinWidth), 0))) {
		ImGui::OpenPopup(L("HISTORY_CONFIRM_CLEAN_TITLE"));
	}
	ImGui::SameLine();
	if (!CanUseCloudActions(cfg)) ImGui::BeginDisabled();
	if (ImGui::Button(L("HISTORY_CLOUD_ANALYZE"), ImVec2(CalcButtonWidth(L("HISTORY_CLOUD_ANALYZE"), toolbarButtonMinWidth), 0))) {
		Config configCopy = cfg;
		const int configIndex = tempCurrentConfigIndex;
		TaskCoordinator::Instance().Submit(L"Analyze cloud history",
			{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configIndex](stop_token) {
			AnalyzeCloudHistory(configCopy, configIndex);
		});
	}
	ImGui::SameLine();
	if (ImGui::Button(L("HISTORY_CLOUD_SYNC_HISTORY"), ImVec2(CalcButtonWidth(L("HISTORY_CLOUD_SYNC_HISTORY"), toolbarButtonMinWidth), 0))) {
		Config configCopy = cfg;
		const int configIndex = tempCurrentConfigIndex;
		TaskCoordinator::Instance().Submit(L"Sync cloud history",
			{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configIndex](stop_token) {
			SyncConfigFromCloud(configCopy, configIndex, CloudSyncMode::HistoryOnly);
		});
	}
	ImGui::SameLine();
	if (ImGui::Button(L("HISTORY_CLOUD_SYNC_ALL"), ImVec2(CalcButtonWidth(L("HISTORY_CLOUD_SYNC_ALL"), toolbarButtonMinWidth), 0))) {
		Config configCopy = cfg;
		const int configIndex = tempCurrentConfigIndex;
		TaskCoordinator::Instance().Submit(L"Sync cloud backups",
			{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configIndex](stop_token) {
			SyncConfigFromCloud(configCopy, configIndex, CloudSyncMode::HistoryAndBackups);
		});
	}
	if (!CanUseCloudActions(cfg)) ImGui::EndDisabled();

	// 清理确认弹窗
	if (ImGui::BeginPopupModal(L("HISTORY_CONFIRM_CLEAN_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(L("HISTORY_CONFIRM_CLEAN_MSG"));
		ImGui::Separator();
		float cleanConfirmBtnWidth = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"), dialogButtonMinWidth);
		if (ImGui::Button(L("BUTTON_OK"), ImVec2(cleanConfirmBtnWidth, 0))) {
			if (g_appState.configs.count(tempCurrentConfigIndex) && g_appState.g_history.count(tempCurrentConfigIndex)) {
				auto& history_vec = g_appState.g_history.at(tempCurrentConfigIndex);
				history_vec.erase(
					remove_if(history_vec.begin(), history_vec.end(),
						[&](const HistoryEntry& entry) {
							return !filesystem::exists(filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / entry.worldName / entry.backupFile);
						}),
					history_vec.end()
				);
				SaveHistory();
				//selected_entry = nullptr; // 清理后重置选择
				is_comment_editing = false;
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(cleanConfirmBtnWidth, 0))) { ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}

	ImGui::Separator();
	{
		lock_guard<mutex> cloudLock(g_appState.cloudTask.mutex);
		if ((g_appState.cloudTask.activeConfigIndex == tempCurrentConfigIndex && (!g_appState.cloudTask.statusText.empty() || !g_appState.cloudTask.lastMessage.empty()))
			|| !g_appState.cloudTask.lastMessage.empty()) {
			ImGui::TextWrapped("%s", wstring_to_utf8(g_appState.cloudTask.statusText).c_str());
			if (!g_appState.cloudTask.lastMessage.empty()) {
				ImGui::TextWrapped("%s", wstring_to_utf8(g_appState.cloudTask.lastMessage).c_str());
			}
			ImGui::Separator();
		}
	}

	// --- 主体布局：左右分栏 ---
	float list_width = ImGui::GetContentRegionAvail().x * 0.45f;
	ImGui::BeginChild("HistoryListPane", ImVec2(list_width, 0), true);

	if (g_appState.g_history.find(tempCurrentConfigIndex) == g_appState.g_history.end() || g_appState.g_history.at(tempCurrentConfigIndex).empty()) {
		ImGui::TextWrapped("%s", L("HISTORY_EMPTY"));
	}
	else {
		auto& history_vec = g_appState.g_history.at(tempCurrentConfigIndex);

		map<wstring, vector<HistoryEntry*>> world_history_map;
		for (auto& entry : history_vec) {
			world_history_map[entry.worldName].push_back(&entry);
		}

		for (auto& pair : world_history_map) {
			ImGuiWindowFlags treeNodeFlags = ImGuiTreeNodeFlags_None;
			ImGui::SetNextItemOpen(false, ImGuiCond_Appearing);
			// 默认展开世界
			if (!g_worldToFocusInHistory.empty() && pair.first == g_worldToFocusInHistory) {
				treeNodeFlags = ImGuiTreeNodeFlags_Leaf;
			}

			if (ImGui::TreeNodeEx(wstring_to_utf8(pair.first).c_str(), treeNodeFlags)) {
				sort(pair.second.begin(), pair.second.end(), [](const HistoryEntry* a, const HistoryEntry* b) {
					return a->timestamp_str > b->timestamp_str;
					});

				vector<HistoryEntry*> filteredEntries;
				filteredEntries.reserve(pair.second.size());
				for (HistoryEntry* entry : pair.second) {
					string entry_label_utf8 = wstring_to_utf8(entry->backupFile);
					if (filter.PassFilter(entry_label_utf8.c_str()) ||
						filter.PassFilter(wstring_to_utf8(entry->comment).c_str())) {
						filteredEntries.push_back(entry);
					}
				}

				ImGuiListClipper historyClipper;
				historyClipper.Begin(static_cast<int>(filteredEntries.size()));
				while (historyClipper.Step()) {
				for (int entryIndex = historyClipper.DisplayStart; entryIndex < historyClipper.DisplayEnd; ++entryIndex) {
					HistoryEntry* entry = filteredEntries[entryIndex];
					string entry_label_utf8 = wstring_to_utf8(entry->backupFile);
					filesystem::path backup_path = filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / entry->worldName / entry->backupFile;
					bool file_exists = filesystem::exists(backup_path);
					bool is_small = file_exists && filesystem::file_size(backup_path) < 10240;

					// --- 自定义列表项卡片 ---
					ImGui::PushID(entry);
					if (ImGui::Selectable("##entry_selectable", selected_entry == entry, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2.5f))) {
						selected_entry = entry;
						sel_world = entry->worldName; sel_file = entry->backupFile;
						is_comment_editing = false;
					}

					ImDrawList* draw_list = ImGui::GetWindowDrawList();
					ImVec2 p_min = ImGui::GetItemRectMin();
					ImVec2 p_max = ImGui::GetItemRectMax();
					if (ImGui::IsItemHovered()) {
						draw_list->AddRectFilled(p_min, p_max, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), 4.0f);
					}
					if (selected_entry == entry) {
						draw_list->AddRect(p_min, p_max, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), 4.0f, 2.0f);
					}

					// 图标
					const char* icon = file_exists ? (is_small ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_FILE) : ICON_FA_GHOST;
					ImVec4 icon_color = file_exists ? (is_small ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f)) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
					ImGui::TextColored(icon_color, "%s", icon);

					// 文本内容
					ImGui::TextUnformatted(entry_label_utf8.c_str());
					ImGui::TextDisabled("%s", wstring_to_utf8(entry->timestamp_str + L" | " + entry->comment).c_str());
					ImGui::SameLine();

					// 重要标记图标
					if (entry->isImportant) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f)); // Gold color for important
					}
					else {
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]); // Grey for not important
					}
					ImGui::Text(ICON_FA_STAR);

					ImGui::PopStyleColor();
					ImGui::PopID();
				}
				}
				ImGui::TreePop();
			}
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// --- 右侧详情与操作面板 ---
	ImGui::BeginChild("DetailsPane", ImVec2(0, 0), true);
	if (selected_entry) {
		ImGui::SeparatorText(L("HISTORY_DETAILS_PANE_TITLE"));

		filesystem::path backup_path = filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / selected_entry->worldName / selected_entry->backupFile;
		bool file_exists = filesystem::exists(backup_path);
		const bool has_cloud_copy = HasHistoryCloudCopy(*selected_entry);

		// 详细信息表格
		if (ImGui::BeginTable("DetailsTable", 2, ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableNextColumn(); ImGui::TextUnformatted(L("HISTORY_LABEL_WORLD"));
			ImGui::TableNextColumn(); ImGui::Text("%s", wstring_to_utf8(selected_entry->worldName).c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(L("HISTORY_LABEL_FILENAME"));
			ImGui::TableNextColumn(); ImGui::Text("%s", wstring_to_utf8(selected_entry->backupFile).c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(L("HISTORY_LABEL_BACKUP_TIME"));
			ImGui::TableNextColumn(); ImGui::Text("%s", wstring_to_utf8(selected_entry->timestamp_str).c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(L("HISTORY_LABEL_STATUS"));
			ImGui::TableNextColumn();
			if (file_exists) {
				bool is_small = filesystem::file_size(backup_path) < 10240;
				ImGui::TextColored(is_small ? ImVec4(1.0f, 0.9f, 0.6f, 1.0f) : ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", L(is_small ? "HISTORY_STATUS_SMALL" : "HISTORY_STATUS_OK"));
			}
			else if (has_cloud_copy) {
				ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "%s", L("HISTORY_STATUS_CLOUD_ONLY"));
			}
			else {
				ImGui::TextDisabled("%s", L("HISTORY_STATUS_MISSING"));
			}
			if (file_exists) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::TextUnformatted(L("HISTORY_LABEL_FILE_SIZE"));
				ImGui::TableNextColumn();
				char size_buf[64];
				sprintf_s(size_buf, "%.2f MB", filesystem::file_size(backup_path) / (1024.0f * 1024.0f));
				ImGui::Text("%s", size_buf);
			}
			ImGui::EndTable();
		}

		ImGui::SeparatorText(L("HISTORY_GROUP_ACTIONS"));
		const bool canUseCloud = CanUseCloudActions(cfg);
		const bool canRestore = file_exists || (has_cloud_copy && cfg.cloudAutoDownloadBeforeRestore && canUseCloud);
		if (!canRestore) ImGui::BeginDisabled();
		if (ImGui::Button(L("HISTORY_BUTTON_RESTORE"), ImVec2(CalcButtonWidth(L("HISTORY_BUTTON_RESTORE"), actionButtonMinWidth), 0))) {
			ImGui::OpenPopup("##CONFIRM_RESTORE");
		}
		if (!canRestore) ImGui::EndDisabled();
		if (ImGui::BeginPopupModal("##CONFIRM_RESTORE", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			//history_restore = false;
			ImGui::SeparatorText(L("SELECTED_BACKUP_DETAILS"));
			ImGui::Text("%s: %s", L("FILENAME_LABEL"), wstring_to_utf8(selected_entry->backupFile).c_str());
			ImGui::Text("%s: %s", L("TIMESTAMP_LABEL"), wstring_to_utf8(selected_entry->timestamp_str).c_str());
			ImGui::Text("%s: %s", L("TYPE_LABEL"), wstring_to_utf8(selected_entry->backupType).c_str());
			ImGui::Text("%s: %s", L("COMMENT_LABEL"), selected_entry->comment.empty() ? L("HISTORY_NO_COMMENT") : wstring_to_utf8(selected_entry->comment).c_str());
			
			ImGui::SeparatorText(L("CHOOSE_RESTORE_METHOD_TITLE"));
			static int restore_method = 0;
			static char customRestoreBuf[CONSTANT2] = "";

			ImGui::RadioButton(L("RESTORE_METHOD_CLEAN"), &restore_method, 0);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_METHOD_CLEAN"));

			ImGui::RadioButton(L("RESTORE_METHOD_OVERWRITE"), &restore_method, 1);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_METHOD_OVERWRITE"));

			ImGui::RadioButton(L("RESTORE_METHOD_REVERSE"), &restore_method, 2);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_METHOD_REVERSE"));

			ImGui::RadioButton(L("RESTORE_METHOD_CUSTOM"), &restore_method, 3);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_METHOD_CUSTOM"));

			// 仅在选择自定义还原时显示输入框
			if (restore_method == 3) {
				ImGui::Indent();
				ImGui::SetNextItemWidth(-1);
				ImGui::InputTextWithHint("##custom_restore_input", L("CUSTOM_RESTORE_ITEMS_HINT"), customRestoreBuf, IM_ARRAYSIZE(customRestoreBuf));
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", L("CUSTOM_RESTORE_HINT"));
				}
				ImGui::Unindent();
			}
			else {
				// 确保在切换到其他模式时清空输入，避免混淆
				if (strlen(customRestoreBuf) > 0) {
					strcpy_s(customRestoreBuf, "");
				}
			}

			ImGui::Separator();
			float restoreConfirmBtnWidth = CalcButtonWidth(L("BUTTON_CONFIRM_RESTORE"), dialogButtonMinWidth);
			float restoreSelectFileBtnWidth = CalcButtonWidth(L("BUTTON_SELECT_CUSTOM_FILE"), dialogButtonMinWidth);
			float restoreCancelBtnWidth = CalcButtonWidth(L("BUTTON_CANCEL"), dialogButtonMinWidth);

			if (ImGui::Button(L("BUTTON_CONFIRM_RESTORE"), ImVec2(restoreConfirmBtnWidth, 0))) {
				const Config configCopy = cfg;
				const HistoryEntry entryCopy = *selected_entry;
				const int configIndex = tempCurrentConfigIndex;
				const int restoreMethod = restore_method;
				const string customItems = customRestoreBuf;
				const auto worldPath = JoinPath(configCopy.saveRoot, entryCopy.worldName);
				TaskCoordinator::Instance().Submit(L"Restore backup",
					{ TaskCoordinator::WorldResourceKey(configCopy.configId, worldPath) },
					[configCopy, entryCopy, configIndex, restoreMethod, customItems, worldPath](stop_token) {
						if (configCopy.backupBefore) {
							MyFolder world = { worldPath.wstring(), entryCopy.worldName, L"", configCopy, configIndex, -1 };
							DoBackup(world, L"BeforeRestore");
						}
						DoRestore(configCopy, entryCopy.worldName, entryCopy.backupFile,
							restoreMethod, customItems);
					});
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();

			if (ImGui::Button(L("BUTTON_SELECT_CUSTOM_FILE"), ImVec2(restoreSelectFileBtnWidth, 0))) {
				wstring selectedFile = GetDesktopServices()->SelectFile().path.wstring();
				if (!selectedFile.empty()) {
					const Config configCopy = cfg;
					const wstring worldName = selected_entry->worldName;
					const int restoreMethod = restore_method;
					TaskCoordinator::Instance().Submit(L"Restore custom backup",
						{ TaskCoordinator::WorldResourceKey(configCopy.configId, JoinPath(configCopy.saveRoot, worldName)) },
						[configCopy, worldName, selectedFile, restoreMethod](stop_token) {
							DoRestore2(configCopy, worldName, selectedFile, restoreMethod);
						});
					ImGui::CloseCurrentPopup(); // Close method choice
					//entry_for_action = nullptr; // Reset selection
				}
			}

			if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(restoreCancelBtnWidth, 0))) {
				ImGui::CloseCurrentPopup();
				//selected_entry = nullptr;
			}

			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (!file_exists) ImGui::BeginDisabled();
		if (ImGui::Button(L("HISTORY_BUTTON_OPEN_FOLDER"), ImVec2(CalcButtonWidth(L("HISTORY_BUTTON_OPEN_FOLDER"), actionButtonMinWidth), 0))) {
			(void)GetDesktopServices()->RevealInFolder(backup_path.parent_path(), backup_path);
		}
		if (!file_exists) ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(selected_entry->isImportant ? L("HISTORY_UNMARK_IMPORTANT") : L("HISTORY_MARK_IMPORTANT"),
			ImVec2(CalcButtonWidth(selected_entry->isImportant ? L("HISTORY_UNMARK_IMPORTANT") : L("HISTORY_MARK_IMPORTANT"), actionButtonMinWidth), 0))) {
			selected_entry->isImportant = !selected_entry->isImportant;
			SaveHistory();
		}

		// -----------

		if (!canUseCloud || !file_exists) ImGui::BeginDisabled();
		if (ImGui::Button(L("HISTORY_BUTTON_UPLOAD_CLOUD"), ImVec2(CalcButtonWidth(L("HISTORY_BUTTON_UPLOAD_CLOUD"), compactButtonMinWidth), 0))) {
			const Config configCopy = cfg;
			const HistoryEntry entryCopy = *selected_entry;
			const int configIndex = tempCurrentConfigIndex;
			TaskCoordinator::Instance().Submit(L"Upload backup",
				{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configIndex, entryCopy](stop_token) {
				UploadHistoryEntry(configCopy, configIndex, entryCopy);
			});
		}
		if (!canUseCloud || !file_exists) ImGui::EndDisabled();
		ImGui::SameLine();
		if (!canUseCloud || !has_cloud_copy) ImGui::BeginDisabled();
		if (ImGui::Button(L("HISTORY_BUTTON_DOWNLOAD_CLOUD"), ImVec2(CalcButtonWidth(L("HISTORY_BUTTON_DOWNLOAD_CLOUD"), compactButtonMinWidth), 0))) {
			const Config configCopy = cfg;
			const HistoryEntry entryCopy = *selected_entry;
			const int configIndex = tempCurrentConfigIndex;
			TaskCoordinator::Instance().Submit(L"Download backup",
				{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configIndex, entryCopy](stop_token) {
				DownloadHistoryEntry(configCopy, configIndex, entryCopy);
			});
		}
		if (!canUseCloud || !has_cloud_copy) ImGui::EndDisabled();
		ImGui::SameLine();
		if (!cfg.enableWEIntegration || !file_exists) ImGui::BeginDisabled();
		if (ImGui::Button(L("BUTTON_ADD_TO_WE"), ImVec2(CalcButtonWidth(L("BUTTON_ADD_TO_WE"), compactButtonMinWidth), 0))) {
			const Config configCopy = cfg;
			const HistoryEntry entryCopy = *selected_entry;
			TaskCoordinator::Instance().Submit(L"Create WorldEdit snapshot",
				{ TaskCoordinator::WorldResourceKey(configCopy.configId, JoinPath(configCopy.saveRoot, entryCopy.worldName)) },
				[configCopy, entryCopy](stop_token) {
					AddBackupToWESnapshots(configCopy, entryCopy.worldName, entryCopy.backupFile);
				});
		}

		if (!cfg.enableWEIntegration || !file_exists) ImGui::EndDisabled();
		// -----------

		const float deleteBtnWidth = CalcButtonWidth(L("HISTORY_BUTTON_DELETE"), compactButtonMinWidth);
		const float deleteBtnPosX = (std::max)(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - deleteBtnWidth);
		ImGui::SameLine(deleteBtnPosX);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
		if (ImGui::Button(L("HISTORY_BUTTON_DELETE"), ImVec2(deleteBtnWidth, 0))) {
			entry_to_delete = selected_entry;
			del_world = sel_world; del_file = sel_file;
			ImGui::OpenPopup(L("HISTORY_DELETE_POPUP_TITLE"));
		}
		ImGui::PopStyleColor(2);

		// --- 删除确认弹窗 ---
		if (ImGui::BeginPopupModal(L("HISTORY_DELETE_POPUP_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			static int deleteMode = 2;
			static bool safeDeleteSelected = true;
			if (ImGui::IsWindowAppearing()) {
				filesystem::path deletePath = filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / entry_to_delete->worldName / entry_to_delete->backupFile;
				deleteMode = filesystem::exists(deletePath) ? 2 : 0;
				safeDeleteSelected = isSafeDelete && entry_to_delete->backupType.find(L"Smart") != wstring::npos;
			}

			ImGui::TextWrapped(L("HISTORY_DELETE_POPUP_MSG"), wstring_to_utf8(entry_to_delete->backupFile).c_str());
			ImGui::Separator();
			ImGui::RadioButton(L("HISTORY_DELETE_MODE_HISTORY_ONLY"), &deleteMode, 0);
			bool deleteLocalFileExists = filesystem::exists(filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / entry_to_delete->worldName / entry_to_delete->backupFile);
			if (!deleteLocalFileExists) ImGui::BeginDisabled();
			ImGui::RadioButton(L("HISTORY_DELETE_MODE_LOCAL_ONLY"), &deleteMode, 1);
			ImGui::RadioButton(L("HISTORY_DELETE_MODE_LOCAL_AND_HISTORY"), &deleteMode, 2);
			if (!deleteLocalFileExists) ImGui::EndDisabled();

			const bool canSafeDelete = deleteMode == 2 && entry_to_delete->backupType.find(L"Smart") != wstring::npos;
			if (!canSafeDelete) ImGui::BeginDisabled();
			ImGui::Checkbox(L("HISTORY_DELETE_USE_SAFE_DELETE"), &safeDeleteSelected);
			if (!canSafeDelete) ImGui::EndDisabled();

			ImGui::Separator();
			float deleteConfirmBtnWidth = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"), dialogButtonMinWidth);
			if (ImGui::Button(L("BUTTON_OK"), ImVec2(deleteConfirmBtnWidth, 0))) {
				HistoryEntry entryCopy = *entry_to_delete;
				Config configCopy = g_appState.configs[tempCurrentConfigIndex];
				int configIndex = tempCurrentConfigIndex;
				BackupDeleteMode selectedMode = deleteMode == 0
					? BackupDeleteMode::HistoryOnly
					: (deleteMode == 1 ? BackupDeleteMode::LocalArchiveOnly : BackupDeleteMode::LocalArchiveAndHistory);
				const bool useSafeDeleteForThread = safeDeleteSelected;
				TaskCoordinator::Instance().Submit(L"Delete backup",
					{ TaskCoordinator::WorldResourceKey(configCopy.configId, JoinPath(configCopy.saveRoot, entryCopy.worldName)) },
					[configCopy, entryCopy, configIndex, selectedMode, useSafeDeleteForThread](stop_token) mutable {
					DeleteBackupWithMode(configCopy, entryCopy, configIndex, selectedMode, useSafeDeleteForThread);
				});
				is_comment_editing = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(deleteConfirmBtnWidth, 0))) { ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}

		

		ImGui::SeparatorText(L("HISTORY_GROUP_COMMENT"));
		if (is_comment_editing) {
			ImGui::InputTextMultiline("##commentedit", comment_buf, sizeof(comment_buf), ImVec2(-1, ImGui::GetContentRegionAvail().y - 40));
			if (ImGui::Button(L("HISTORY_BUTTON_SAVE_COMMENT"), ImVec2(CalcButtonWidth(L("HISTORY_BUTTON_SAVE_COMMENT"), compactButtonMinWidth), 0))) {
				selected_entry->comment = utf8_to_wstring(comment_buf);
				SaveHistory();
				is_comment_editing = false;
			}
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(CalcButtonWidth(L("BUTTON_CANCEL"), compactButtonMinWidth), 0))) {
				is_comment_editing = false;
			}
		}
		else {
			string comment_text = selected_entry->comment.empty() ? "(No comment)" : wstring_to_utf8(selected_entry->comment);
			ImGui::TextWrapped("%s", comment_text.c_str());
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", L("HISTORY_EDIT_COMMENT_TIP"));
			}
			if (ImGui::IsItemClicked()) {
				is_comment_editing = true;
				strncpy_s(comment_buf, wstring_to_utf8(selected_entry->comment).c_str(), sizeof(comment_buf));
			}
		}

	}
	else {
		ImGui::TextWrapped("%s", L("HISTORY_SELECT_PROMPT"));
	}
	ImGui::EndChild();

	ImGui::End();
}

namespace {

const char* HistoryStatusKey(HistoryFileStatus status) {
	switch (status) {
	case HistoryFileStatus::Normal: return "HISTORY_STATUS_OK";
	case HistoryFileStatus::CloudOnly: return "HISTORY_STATUS_CLOUD_ONLY";
	case HistoryFileStatus::Missing: return "HISTORY_STATUS_MISSING";
	case HistoryFileStatus::SmallFile: return "HISTORY_STATUS_SMALL";
	case HistoryFileStatus::Inaccessible: return "HISTORY_STATUS_INACCESSIBLE";
	}
	return "HISTORY_STATUS_MISSING";
}

ImVec4 HistoryStatusColor(HistoryFileStatus status) {
	switch (status) {
	case HistoryFileStatus::Normal: return ImVec4(0.35f, 0.80f, 0.45f, 1.0f);
	case HistoryFileStatus::CloudOnly: return ImVec4(0.40f, 0.70f, 1.0f, 1.0f);
	case HistoryFileStatus::SmallFile: return ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
	case HistoryFileStatus::Missing:
	case HistoryFileStatus::Inaccessible:
		return ImVec4(0.95f, 0.45f, 0.35f, 1.0f);
	}
	return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

HistoryEntry* ResolveHistoryEntry(int configIndex, const HistoryEntryKey& key) {
	if (key.Empty()) return nullptr;
	const auto history = g_appState.g_history.find(configIndex);
	if (history == g_appState.g_history.end()) return nullptr;
	for (HistoryEntry& entry : history->second) {
		if (entry.worldName == key.worldName && entry.backupFile == key.backupFile) {
			return &entry;
		}
	}
	return nullptr;
}

void ConstrainHistoryPopup(const UiMetrics& metrics) {
	const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
	ImGui::SetNextWindowSizeConstraints(
		ImVec2((std::min)(metrics.Em(20.0f), work.x * 0.9f),
			(std::min)(metrics.Em(10.0f), work.y * 0.9f)),
		ImVec2(work.x * 0.9f, work.y * 0.9f));
}

bool SameLineFits(const char* nextLabel) {
	const float required = ImGui::CalcTextSize(nextLabel).x
		+ ImGui::GetStyle().FramePadding.x * 2.0f
		+ ImGui::GetStyle().ItemSpacing.x;
	if (ImGui::GetContentRegionAvail().x < required) return false;
	ImGui::SameLine();
	return true;
}

} // namespace

void ShowHistoryWindow(int requestedConfigIndex,
	const optional<wstring>& initialWorld) {
	static int lockedConfigIndex = -1;
	static bool wasOpen = false;
	static HistoryEntryKey selectedKey;
	static HistoryEntryKey restoreKey;
	static HistoryEntryKey deleteKey;
	static HistoryEntryKey commentKey;
	static wstring worldFilter;
	static char textFilter[256] = "";
	static int statusFilterIndex = 0;
	static bool importantOnly = false;
	static bool narrowShowDetails = false;
	static bool requestRestorePopup = false;
	static bool requestDeletePopup = false;
	static bool requestCommentPopup = false;
	static char commentBuffer[1024] = "";

	if (!wasOpen || lockedConfigIndex < 0) {
		lockedConfigIndex = requestedConfigIndex;
		worldFilter = initialWorld.value_or(g_worldToFocusInHistory);
		selectedKey = {};
		narrowShowDetails = false;
	}

	const UiMetrics metrics = GetUiMetrics();
	SetNextWindowSizeFromMetrics(metrics, 72.0f, 46.0f);
	SetNextWindowConstraintsFromMetrics(metrics, 36.0f, 26.0f);
	const bool visible = ImGui::Begin(L("HISTORY_WINDOW_TITLE"), &showHistoryWindow,
		ImGuiWindowFlags_NoDocking);
	wasOpen = showHistoryWindow;
	if (!visible) {
		ImGui::End();
		if (!showHistoryWindow) lockedConfigIndex = -1;
		return;
	}

	const auto configIt = g_appState.configs.find(lockedConfigIndex);
	if (configIt == g_appState.configs.end()) {
		ImGui::TextWrapped("%s", L("HISTORY_CONFIG_UNAVAILABLE"));
		ImGui::End();
		return;
	}
	Config& config = configIt->second;
	auto& entries = g_appState.g_history[lockedConfigIndex];

	ImGui::Text("%s: [No.%d] %s", L("HISTORY_LOCKED_CONFIG"), lockedConfigIndex,
		config.name.c_str());
	ImGui::Separator();

	vector<wstring> worlds;
	for (const HistoryEntry& entry : entries) {
		if (find(worlds.begin(), worlds.end(), entry.worldName) == worlds.end()) {
			worlds.push_back(entry.worldName);
		}
	}
	sort(worlds.begin(), worlds.end());

	const bool wideToolbar = ImGui::GetContentRegionAvail().x >= metrics.Em(58.0f);
	const float worldWidth = wideToolbar ? metrics.Em(13.0f) : -1.0f;
	ImGui::SetNextItemWidth(worldWidth);
	const string selectedWorldLabel = worldFilter.empty()
		? string(L("HISTORY_ALL_WORLDS")) : wstring_to_utf8(worldFilter);
	if (ImGui::BeginCombo("##HistoryWorld", selectedWorldLabel.c_str())) {
		if (ImGui::Selectable(L("HISTORY_ALL_WORLDS"), worldFilter.empty())) {
			worldFilter.clear();
		}
		for (const wstring& world : worlds) {
			const bool selected = worldFilter == world;
			if (ImGui::Selectable(wstring_to_utf8(world).c_str(), selected)) worldFilter = world;
		}
		ImGui::EndCombo();
	}
	if (wideToolbar) ImGui::SameLine();

	const char* statusNames[] = {
		L("HISTORY_FILTER_ALL"), L("HISTORY_FILTER_NORMAL"),
		L("HISTORY_FILTER_CLOUD_ONLY"), L("HISTORY_FILTER_MISSING"),
		L("HISTORY_FILTER_SMALL")
	};
	ImGui::SetNextItemWidth(wideToolbar ? metrics.Em(11.0f) : -1.0f);
	ImGui::Combo("##HistoryStatus", &statusFilterIndex, statusNames,
		IM_ARRAYSIZE(statusNames));
	if (wideToolbar) ImGui::SameLine();
	ImGui::SetNextItemWidth(wideToolbar ? -1.0f : -1.0f);
	ImGui::InputTextWithHint("##HistorySearch", L("HISTORY_SEARCH_HINT"), textFilter,
		IM_ARRAYSIZE(textFilter));

	ImGui::Checkbox(L("HISTORY_IMPORTANT_ONLY"), &importantOnly);
	SameLineFits(L("HISTORY_CLOUD_MENU"));
	if (ImGui::Button(L("HISTORY_CLOUD_MENU"))) ImGui::OpenPopup("##HistoryCloudMenu");
	if (ImGui::BeginPopup("##HistoryCloudMenu")) {
		const bool canCloud = CanUseCloudActions(config);
		ImGui::BeginDisabled(!canCloud);
		if (ImGui::MenuItem(L("HISTORY_CLOUD_ANALYZE"))) {
			const Config copy = config;
			const int index = lockedConfigIndex;
			TaskCoordinator::Instance().Submit(L"Analyze cloud history",
				{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
				[copy, index](stop_token) { AnalyzeCloudHistory(copy, index); });
		}
		if (ImGui::MenuItem(L("HISTORY_CLOUD_SYNC_HISTORY"))) {
			const Config copy = config;
			const int index = lockedConfigIndex;
			TaskCoordinator::Instance().Submit(L"Sync cloud history",
				{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
				[copy, index](stop_token) {
					SyncConfigFromCloud(copy, index, CloudSyncMode::HistoryOnly);
				});
		}
		if (ImGui::MenuItem(L("HISTORY_CLOUD_SYNC_ALL"))) {
			const Config copy = config;
			const int index = lockedConfigIndex;
			TaskCoordinator::Instance().Submit(L"Sync cloud data",
				{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
				[copy, index](stop_token) {
					SyncConfigFromCloud(copy, index, CloudSyncMode::HistoryAndBackups);
				});
		}
		ImGui::EndDisabled();
		if (!canCloud) {
			ImGui::Separator();
			ImGui::TextWrapped("%s",
				wstring_to_utf8(GetCloudActionsUnavailableReason(config)).c_str());
		}
		ImGui::EndPopup();
	}

	size_t missingCount = 0;
	for (HistoryEntry& entry : entries) {
		const HistoryFileStatus status = BuildHistoryEntryView(config, entry).status;
		if (status == HistoryFileStatus::Missing
			|| status == HistoryFileStatus::Inaccessible) ++missingCount;
	}
	const string cleanLabel = wstring_to_utf8(MineFormatMessage(
		"HISTORY_CLEAN_INVALID_COUNT", static_cast<int>(missingCount)));
	SameLineFits(cleanLabel.c_str());
	ImGui::BeginDisabled(missingCount == 0);
	if (ImGui::Button(cleanLabel.c_str())) {
		ImGui::OpenPopup(L("HISTORY_CONFIRM_CLEAN_TITLE"));
	}
	ImGui::EndDisabled();

	ConstrainHistoryPopup(metrics);
	if (ImGui::BeginPopupModal(L("HISTORY_CONFIRM_CLEAN_TITLE"), nullptr,
		ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", L("HISTORY_CONFIRM_CLEAN_MSG"));
		if (ImGui::Button(L("BUTTON_OK"))) {
			entries.erase(remove_if(entries.begin(), entries.end(), [&](HistoryEntry& entry) {
				const HistoryFileStatus status = BuildHistoryEntryView(config, entry).status;
				return status == HistoryFileStatus::Missing
					|| status == HistoryFileStatus::Inaccessible;
			}), entries.end());
			SaveHistory();
			if (!ResolveHistoryEntry(lockedConfigIndex, selectedKey)) {
				selectedKey = {};
				narrowShowDetails = false;
			}
			ImGui::CloseCurrentPopup();
		}
		SameLineFits(L("BUTTON_CANCEL"));
		if (ImGui::Button(L("BUTTON_CANCEL"))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	{
		lock_guard<mutex> cloudLock(g_appState.cloudTask.mutex);
		if (g_appState.cloudTask.activeConfigIndex == lockedConfigIndex
			&& (!g_appState.cloudTask.statusText.empty()
				|| !g_appState.cloudTask.lastMessage.empty())) {
			ImGui::TextWrapped("%s",
				wstring_to_utf8(g_appState.cloudTask.statusText).c_str());
			if (!g_appState.cloudTask.lastMessage.empty()) {
				ImGui::TextWrapped("%s",
					wstring_to_utf8(g_appState.cloudTask.lastMessage).c_str());
			}
		}
	}
	ImGui::Separator();

	const auto filtered = BuildFilteredHistoryViews(config, entries, worldFilter,
		textFilter, static_cast<HistoryStatusFilter>(statusFilterIndex), importantOnly);
	HistoryEntry* selectedEntry = ResolveHistoryEntry(lockedConfigIndex, selectedKey);
	if (!selectedEntry) {
		selectedKey = {};
		narrowShowDetails = false;
	}

	const HistoryResponsiveLayout layout = ComputeHistoryResponsiveLayout(
		ImGui::GetContentRegionAvail().x, metrics.em, metrics.spacingX);
	const bool showList = layout.useSplitView || !narrowShowDetails;
	if (showList) {
		ImGui::BeginChild("##HistoryList", ImVec2(layout.listWidth, 0.0f),
			ImGuiChildFlags_Borders);
		if (entries.empty()) {
			ImGui::TextWrapped("%s", L("HISTORY_EMPTY"));
		}
		else if (filtered.empty()) {
			ImGui::TextWrapped("%s", L("HISTORY_FILTER_EMPTY"));
		}
		for (const HistoryEntryView& view : filtered) {
			HistoryEntry& entry = *view.entry;
			ImGui::PushID(wstring_to_utf8(entry.worldName + L"\n" + entry.backupFile).c_str());
			BeginUiCard("##HistoryEntryCard");
			const bool selected = selectedKey == HistoryEntryKey{entry.worldName, entry.backupFile};
			const string filename = wstring_to_utf8(entry.backupFile);
			if (ImGui::Selectable(filename.c_str(), selected)) {
				selectedKey = {entry.worldName, entry.backupFile};
				narrowShowDetails = !layout.useSplitView;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", filename.c_str());
			ImGui::TextColored(HistoryStatusColor(view.status), "%s  %s",
				view.status == HistoryFileStatus::Normal ? ICON_FA_FILE
					: view.status == HistoryFileStatus::CloudOnly ? ICON_FA_CLOUD
					: ICON_FA_TRIANGLE_EXCLAMATION,
				L(HistoryStatusKey(view.status)));
			ImGui::SameLine();
			ImGui::TextDisabled("%s", wstring_to_utf8(entry.timestamp_str).c_str());
			if (worldFilter.empty()) {
				ImGui::Text("%s: %s", L("HISTORY_LABEL_WORLD"),
					wstring_to_utf8(entry.worldName).c_str());
			}
			const string sizeLabel = view.fileSize == 0 ? "-"
				: wstring_to_utf8(MineFormatMessage("HISTORY_SIZE_MB",
					static_cast<double>(view.fileSize) / (1024.0 * 1024.0)));
			ImGui::TextDisabled("%s | %s%s", wstring_to_utf8(entry.backupType).c_str(),
				sizeLabel.c_str(),
				entry.isImportant ? "  ★" : "");
			if (!entry.comment.empty()) {
				TextEllipsisWithTooltip(wstring_to_utf8(entry.comment).c_str(),
					ImGui::GetContentRegionAvail().x);
			}
			EndUiCard();
			ImGui::Spacing();
			ImGui::PopID();
		}
		ImGui::EndChild();
	}

	if (layout.useSplitView) ImGui::SameLine();
	if (layout.useSplitView || narrowShowDetails) {
		ImGui::BeginChild("##HistoryDetails", ImVec2(0.0f, 0.0f),
			ImGuiChildFlags_Borders);
		if (!layout.useSplitView && ImGui::Button(L("HISTORY_BACK_TO_LIST"))) {
			narrowShowDetails = false;
		}
		selectedEntry = ResolveHistoryEntry(lockedConfigIndex, selectedKey);
		if (!selectedEntry) {
			ImGui::TextWrapped("%s", L("HISTORY_SELECT_PROMPT"));
		}
		else {
			const HistoryEntryView selectedView =
				BuildHistoryEntryView(config, *selectedEntry);
			const filesystem::path backupPath = filesystem::path(config.backupPath)
				/ selectedEntry->worldName / selectedEntry->backupFile;
			const bool localFile = selectedView.status == HistoryFileStatus::Normal
				|| selectedView.status == HistoryFileStatus::SmallFile;
			const bool cloudCopy = HasHistoryCloudCopy(*selectedEntry);
			const bool canCloud = CanUseCloudActions(config);

			ImGui::SeparatorText(L("HISTORY_DETAILS_PANE_TITLE"));
			if (ImGui::BeginTable("##HistoryDetailsTable", 2,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
				auto row = [](const char* label, const string& value) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(label);
					ImGui::TableNextColumn();
					ImGui::TextWrapped("%s", value.c_str());
				};
				row(L("HISTORY_LABEL_WORLD"), wstring_to_utf8(selectedEntry->worldName));
				row(L("HISTORY_LABEL_FILENAME"), wstring_to_utf8(selectedEntry->backupFile));
				row(L("HISTORY_LABEL_BACKUP_TIME"), wstring_to_utf8(selectedEntry->timestamp_str));
				row(L("TYPE_LABEL"), wstring_to_utf8(selectedEntry->backupType));
				row(L("HISTORY_LABEL_STATUS"), L(HistoryStatusKey(selectedView.status)));
				if (localFile) {
					row(L("HISTORY_LABEL_FILE_SIZE"), wstring_to_utf8(MineFormatMessage(
						"HISTORY_SIZE_MB", static_cast<double>(selectedView.fileSize)
							/ (1024.0 * 1024.0))));
				}
				ImGui::EndTable();
			}

			ImGui::SeparatorText(L("HISTORY_PRIMARY_ACTION"));
			const bool canRestore = localFile
				|| (cloudCopy && config.cloudAutoDownloadBeforeRestore && canCloud);
			ImGui::BeginDisabled(!canRestore);
			if (ImGui::Button(L("HISTORY_BUTTON_RESTORE"), ImVec2(-1.0f, 0.0f))) {
				restoreKey = selectedKey;
				requestRestorePopup = true;
			}
			ImGui::EndDisabled();
			if (!canRestore) ImGui::TextDisabled("%s", L(cloudCopy
				? "HISTORY_RESTORE_NEEDS_CLOUD" : "HISTORY_RESTORE_UNAVAILABLE"));

			ImGui::SeparatorText(L("HISTORY_COMMON_ACTIONS"));
			ImGui::BeginDisabled(!localFile);
			if (ImGui::Button(L("HISTORY_BUTTON_OPEN_FOLDER"))) {
				(void)GetDesktopServices()->RevealInFolder(backupPath.parent_path(), backupPath);
			}
			ImGui::EndDisabled();
			SameLineFits(selectedEntry->isImportant
				? L("HISTORY_UNMARK_IMPORTANT") : L("HISTORY_MARK_IMPORTANT"));
			if (ImGui::Button(selectedEntry->isImportant
				? L("HISTORY_UNMARK_IMPORTANT") : L("HISTORY_MARK_IMPORTANT"))) {
				selectedEntry->isImportant = !selectedEntry->isImportant;
				SaveHistory();
			}
			SameLineFits(L("HISTORY_EDIT_COMMENT"));
			if (ImGui::Button(L("HISTORY_EDIT_COMMENT"))) {
				commentKey = selectedKey;
				strncpy_s(commentBuffer,
					wstring_to_utf8(selectedEntry->comment).c_str(), sizeof(commentBuffer));
				requestCommentPopup = true;
			}
			ImGui::TextWrapped("%s", selectedEntry->comment.empty()
				? L("HISTORY_NO_COMMENT") : wstring_to_utf8(selectedEntry->comment).c_str());

			ImGui::SeparatorText(L("HISTORY_CONDITIONAL_ACTIONS"));
			ImGui::BeginDisabled(!canCloud || !localFile);
			if (ImGui::Button(L("HISTORY_BUTTON_UPLOAD_CLOUD"))) {
				const Config copy = config;
				const HistoryEntry entryCopy = *selectedEntry;
				const int index = lockedConfigIndex;
				TaskCoordinator::Instance().Submit(L"Upload backup",
					{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
					[copy, index, entryCopy](stop_token) {
						UploadHistoryEntry(copy, index, entryCopy);
					});
			}
			ImGui::EndDisabled();
			SameLineFits(L("HISTORY_BUTTON_DOWNLOAD_CLOUD"));
			ImGui::BeginDisabled(!canCloud || !cloudCopy);
			if (ImGui::Button(L("HISTORY_BUTTON_DOWNLOAD_CLOUD"))) {
				const Config copy = config;
				const HistoryEntry entryCopy = *selectedEntry;
				const int index = lockedConfigIndex;
				TaskCoordinator::Instance().Submit(L"Download backup",
					{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
					[copy, index, entryCopy](stop_token) {
						DownloadHistoryEntry(copy, index, entryCopy);
					});
			}
			ImGui::EndDisabled();
			SameLineFits(L("BUTTON_ADD_TO_WE"));
			ImGui::BeginDisabled(!config.enableWEIntegration || !localFile);
			if (ImGui::Button(L("BUTTON_ADD_TO_WE"))) {
				const Config copy = config;
				const HistoryEntry entryCopy = *selectedEntry;
				TaskCoordinator::Instance().Submit(L"Create WorldEdit snapshot",
					{TaskCoordinator::WorldResourceKey(copy.configId,
						JoinPath(copy.saveRoot, entryCopy.worldName))},
					[copy, entryCopy](stop_token) {
						AddBackupToWESnapshots(copy, entryCopy.worldName,
							entryCopy.backupFile);
					});
			}
			ImGui::EndDisabled();
			if (!canCloud) {
				ImGui::TextDisabled("%s",
					wstring_to_utf8(GetCloudActionsUnavailableReason(config)).c_str());
			}
			if (!config.enableWEIntegration) {
				ImGui::TextDisabled("%s", L("HISTORY_WE_DISABLED_REASON"));
			}

			ImGui::SeparatorText(L("HISTORY_DANGER_ZONE"));
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.20f, 0.20f, 1.0f));
			if (ImGui::Button(L("HISTORY_BUTTON_DELETE"))) {
				deleteKey = selectedKey;
				requestDeletePopup = true;
			}
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();
	}

	if (requestCommentPopup) {
		ImGui::OpenPopup("##HistoryComment");
		requestCommentPopup = false;
	}
	if (requestRestorePopup) {
		ImGui::OpenPopup("##HistoryRestore");
		requestRestorePopup = false;
	}
	if (requestDeletePopup) {
		ImGui::OpenPopup(L("HISTORY_DELETE_POPUP_TITLE"));
		requestDeletePopup = false;
	}

	ConstrainHistoryPopup(metrics);
	if (ImGui::BeginPopupModal("##HistoryComment", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize)) {
		HistoryEntry* entry = ResolveHistoryEntry(lockedConfigIndex, commentKey);
		if (!entry) {
			ImGui::TextWrapped("%s", L("HISTORY_ENTRY_DISAPPEARED"));
			if (ImGui::Button(L("BUTTON_OK"))) ImGui::CloseCurrentPopup();
		}
		else {
			ImGui::SetNextItemWidth((std::min)(metrics.Em(32.0f),
				ImGui::GetMainViewport()->WorkSize.x * 0.8f));
			ImGui::InputTextMultiline("##Comment", commentBuffer,
				IM_ARRAYSIZE(commentBuffer), ImVec2(0.0f, metrics.Em(8.0f)));
			if (ImGui::Button(L("HISTORY_BUTTON_SAVE_COMMENT"))) {
				entry->comment = utf8_to_wstring(commentBuffer);
				SaveHistory();
				ImGui::CloseCurrentPopup();
			}
			SameLineFits(L("BUTTON_CANCEL"));
			if (ImGui::Button(L("BUTTON_CANCEL"))) ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	ConstrainHistoryPopup(metrics);
	if (ImGui::BeginPopupModal("##HistoryRestore", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize)) {
		HistoryEntry* entry = ResolveHistoryEntry(lockedConfigIndex, restoreKey);
		static int restoreMethod = 0;
		static char customItems[CONSTANT2] = "";
		if (!entry) {
			ImGui::TextWrapped("%s", L("HISTORY_ENTRY_DISAPPEARED"));
			if (ImGui::Button(L("BUTTON_OK"))) ImGui::CloseCurrentPopup();
		}
		else {
			ImGui::TextWrapped("%s", wstring_to_utf8(entry->backupFile).c_str());
			ImGui::RadioButton(L("RESTORE_METHOD_CLEAN"), &restoreMethod, 0);
			ImGui::RadioButton(L("RESTORE_METHOD_OVERWRITE"), &restoreMethod, 1);
			ImGui::RadioButton(L("RESTORE_METHOD_REVERSE"), &restoreMethod, 2);
			ImGui::RadioButton(L("RESTORE_METHOD_CUSTOM"), &restoreMethod, 3);
			if (restoreMethod == 3) {
				ImGui::SetNextItemWidth(-1.0f);
				ImGui::InputTextWithHint("##CustomRestore", L("CUSTOM_RESTORE_ITEMS_HINT"),
					customItems, IM_ARRAYSIZE(customItems));
			}
			if (ImGui::Button(L("BUTTON_CONFIRM_RESTORE"))) {
				const Config copy = config;
				const HistoryEntry entryCopy = *entry;
				const int index = lockedConfigIndex;
				const int method = restoreMethod;
				const string items = customItems;
				const auto worldPath = JoinPath(copy.saveRoot, entryCopy.worldName);
				TaskCoordinator::Instance().Submit(L"Restore backup",
					{TaskCoordinator::WorldResourceKey(copy.configId, worldPath)},
					[copy, entryCopy, index, method, items, worldPath](stop_token) {
						if (copy.backupBefore) {
							MyFolder world = {worldPath.wstring(), entryCopy.worldName,
								L"", copy, index, -1};
							DoBackup(world, L"BeforeRestore");
						}
						DoRestore(copy, entryCopy.worldName, entryCopy.backupFile,
							method, items);
					});
				ImGui::CloseCurrentPopup();
			}
			SameLineFits(L("BUTTON_SELECT_CUSTOM_FILE"));
			if (ImGui::Button(L("BUTTON_SELECT_CUSTOM_FILE"))) {
				const wstring selectedFile = GetDesktopServices()->SelectFile().path.wstring();
				if (!selectedFile.empty()) {
					const Config copy = config;
					const wstring worldName = entry->worldName;
					const int method = restoreMethod;
					TaskCoordinator::Instance().Submit(L"Restore custom backup",
						{TaskCoordinator::WorldResourceKey(copy.configId,
							JoinPath(copy.saveRoot, worldName))},
						[copy, worldName, selectedFile, method](stop_token) {
							DoRestore2(copy, worldName, selectedFile, method);
						});
					ImGui::CloseCurrentPopup();
				}
			}
			SameLineFits(L("BUTTON_CANCEL"));
			if (ImGui::Button(L("BUTTON_CANCEL"))) ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	ConstrainHistoryPopup(metrics);
	if (ImGui::BeginPopupModal(L("HISTORY_DELETE_POPUP_TITLE"), nullptr,
		ImGuiWindowFlags_AlwaysAutoResize)) {
		HistoryEntry* entry = ResolveHistoryEntry(lockedConfigIndex, deleteKey);
		static int deleteMode = 2;
		static bool useSafeDelete = true;
		if (!entry) {
			ImGui::TextWrapped("%s", L("HISTORY_ENTRY_DISAPPEARED"));
			if (ImGui::Button(L("BUTTON_OK"))) ImGui::CloseCurrentPopup();
		}
		else {
			const HistoryEntryView view = BuildHistoryEntryView(config, *entry);
			const bool localFile = view.status == HistoryFileStatus::Normal
				|| view.status == HistoryFileStatus::SmallFile;
			if (ImGui::IsWindowAppearing()) deleteMode = localFile ? 2 : 0;
			ImGui::TextWrapped(L("HISTORY_DELETE_POPUP_MSG"),
				wstring_to_utf8(entry->backupFile).c_str());
			ImGui::RadioButton(L("HISTORY_DELETE_MODE_HISTORY_ONLY"), &deleteMode, 0);
			ImGui::BeginDisabled(!localFile);
			ImGui::RadioButton(L("HISTORY_DELETE_MODE_LOCAL_ONLY"), &deleteMode, 1);
			ImGui::RadioButton(L("HISTORY_DELETE_MODE_LOCAL_AND_HISTORY"), &deleteMode, 2);
			ImGui::EndDisabled();
			const bool canSafeDelete = deleteMode == 2
				&& entry->backupType.find(L"Smart") != wstring::npos;
			ImGui::BeginDisabled(!canSafeDelete);
			ImGui::Checkbox(L("HISTORY_DELETE_USE_SAFE_DELETE"), &useSafeDelete);
			ImGui::EndDisabled();
			if (ImGui::Button(L("BUTTON_OK"))) {
				const HistoryEntry copyEntry = *entry;
				const Config copyConfig = config;
				const int index = lockedConfigIndex;
				const BackupDeleteMode mode = deleteMode == 0
					? BackupDeleteMode::HistoryOnly
					: deleteMode == 1 ? BackupDeleteMode::LocalArchiveOnly
						: BackupDeleteMode::LocalArchiveAndHistory;
				const bool safeDeleteCopy = useSafeDelete;
				TaskCoordinator::Instance().Submit(L"Delete backup",
					{TaskCoordinator::WorldResourceKey(copyConfig.configId,
						JoinPath(copyConfig.saveRoot, copyEntry.worldName))},
					[copyConfig, copyEntry, index, mode, safeDeleteCopy](stop_token) mutable {
						DeleteBackupWithMode(copyConfig, copyEntry, index, mode,
							safeDeleteCopy);
					});
				ImGui::CloseCurrentPopup();
			}
			SameLineFits(L("BUTTON_CANCEL"));
			if (ImGui::Button(L("BUTTON_CANCEL"))) ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	ImGui::End();
	if (!showHistoryWindow) {
		lockedConfigIndex = -1;
		selectedKey = {};
	}
}


// 构建当前选择（普通 / 特殊）下用于显示的世界列表
std::vector<DisplayWorld> BuildDisplayWorldsForSelection() {
	std::lock_guard<std::mutex> lock(g_appState.configsMutex);
	std::vector<DisplayWorld> out;
	// 普通配置视图
	if (!specialSetting) {
		if (!g_appState.configs.count(g_appState.currentConfigIndex)) return out;
		const Config& src = g_appState.configs[g_appState.currentConfigIndex];
		for (int i = 0; i < (int)src.worlds.size(); ++i) {
			if (src.worlds[i].second == L"#") continue; // 隐藏标记
			DisplayWorld dw;
			dw.name = src.worlds[i].first;
			dw.desc = src.worlds[i].second;
			dw.baseConfigIndex = g_appState.currentConfigIndex;
			dw.baseWorldIndex = i;
			dw.effectiveConfig = src; // 默认使用基础配置
			out.push_back(dw);
		}
		return out;
	}

	// 特殊配置视图
	if (!g_appState.specialConfigs.count(g_appState.currentConfigIndex)) return out;
	const SpecialConfig& sp = g_appState.specialConfigs[g_appState.currentConfigIndex];

	// 优先使用新版统一任务系统
	if (!sp.unifiedTasks.empty()) {
		for (const auto& task : sp.unifiedTasks) {
			// 仅处理备份类型的任务
			if (task.type != TaskTypeV2::Backup) continue;
			if (!task.enabled) continue;
			if (!g_appState.configs.count(task.configIndex)) continue;
			const Config& baseCfg = g_appState.configs[task.configIndex];
			if (task.worldIndex < 0 || task.worldIndex >= (int)baseCfg.worlds.size()) continue;

			DisplayWorld dw;
			dw.name = baseCfg.worlds[task.worldIndex].first;
			dw.desc = baseCfg.worlds[task.worldIndex].second;
			dw.baseConfigIndex = task.configIndex;
			dw.baseWorldIndex = task.worldIndex;

			// 合并配置：以 baseCfg 为主，特殊配置覆盖常用字段
			dw.effectiveConfig = baseCfg;
			dw.effectiveConfig.zipLevel = sp.zipLevel;
			if (sp.keepCount > 0) dw.effectiveConfig.keepCount = sp.keepCount;
			if (sp.cpuThreads > 0) dw.effectiveConfig.cpuThreads = sp.cpuThreads;
			dw.effectiveConfig.useLowPriority = sp.useLowPriority;
			dw.effectiveConfig.blacklist = sp.blacklist;

			out.push_back(dw);
		}
		return out;
	}

	// 向后兼容：使用旧版 tasks
	for (const auto& task : sp.tasks) {
		if (!g_appState.configs.count(task.configIndex)) continue;
		const Config& baseCfg = g_appState.configs[task.configIndex];
		if (task.worldIndex < 0 || task.worldIndex >= (int)baseCfg.worlds.size()) continue;

		DisplayWorld dw;
		dw.name = baseCfg.worlds[task.worldIndex].first;
		dw.desc = baseCfg.worlds[task.worldIndex].second;
		dw.baseConfigIndex = task.configIndex;
		dw.baseWorldIndex = task.worldIndex;

		// 合并配置：以 baseCfg 为主，特殊配置覆盖常用字段
		dw.effectiveConfig = baseCfg;
		dw.effectiveConfig.zipLevel = sp.zipLevel;
		if (sp.keepCount > 0) dw.effectiveConfig.keepCount = sp.keepCount;
		if (sp.cpuThreads > 0) dw.effectiveConfig.cpuThreads = sp.cpuThreads;
		dw.effectiveConfig.useLowPriority = sp.useLowPriority;
		dw.effectiveConfig.blacklist = sp.blacklist;

		out.push_back(dw);
	}

	return out;
}


int ImGuiKeyToVK(ImGuiKey key) {
	if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
		return 'A' + (key - ImGuiKey_A);
	if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
		return '0' + (key - ImGuiKey_0);
	return 0;
}
