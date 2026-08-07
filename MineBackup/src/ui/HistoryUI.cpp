
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
#include "HistoryDialogs.h"

#include <optional>

using namespace std;

namespace {
HistoryWindowController historyController;
bool historyNeedsInitialViewport = true;

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

const HistoryEntry* ResolveHistoryEntry(
	const vector<HistoryEntry>& entries,
	const HistoryEntryKey& key) {
	if (key.Empty()) return nullptr;
	for (const HistoryEntry& entry : entries) {
		if (entry.worldName == key.worldName && entry.backupFile == key.backupFile) {
			return &entry;
		}
	}
	return nullptr;
}

void ConstrainHistoryPopup(const UiMetrics& metrics) {
    const ImGuiViewport* viewport = ImGui::GetWindowViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    const ImVec2 work = viewport->WorkSize;
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

void ResetHistoryWindowRuntimeState() {
	historyNeedsInitialViewport = true;
}

void ReleaseHistoryWindowCaches() {
	historyController.ReleaseCaches();
}

void ShowHistoryWindow(int requestedConfigIndex,
	const optional<wstring>& initialWorld) {
	historyController.Open(
		requestedConfigIndex,
		initialWorld,
		g_worldToFocusInHistory);
	auto& lockedConfigIndex = historyController.lockedConfigIndex;
	auto& selectedKey = historyController.selectedKey;
	auto& restoreKey = historyController.restoreKey;
	auto& deleteKey = historyController.deleteKey;
	auto& commentKey = historyController.commentKey;
	auto& worldFilter = historyController.worldFilter;
	auto& statusFilterIndex = historyController.statusFilterIndex;
	auto& importantOnly = historyController.importantOnly;
	auto& narrowShowDetails = historyController.narrowShowDetails;
	auto& requestRestorePopup = historyController.requestRestorePopup;
	auto& requestDeletePopup = historyController.requestDeletePopup;
	auto& requestCommentPopup = historyController.requestCommentPopup;
	char (&textFilter)[256] = historyController.textFilter;
	char (&commentBuffer)[1024] = historyController.commentBuffer;

	const UiMetrics metrics = GetUiMetrics();
	SetNextWindowSizeFromMetrics(metrics, 64.0f, 42.0f);
	SetNextWindowConstraintsFromMetrics(metrics, 32.0f, 24.0f);
	if (historyNeedsInitialViewport) {
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		historyNeedsInitialViewport = false;
	}
	const bool visible = ImGui::Begin(L("HISTORY_WINDOW_TITLE"), &showHistoryWindow,
		ImGuiWindowFlags_NoDocking);
	historyController.wasOpen = showHistoryWindow;
	if (!visible) {
		ImGui::End();
		if (!showHistoryWindow) {
			historyNeedsInitialViewport = true;
			historyController.Close();
		}
		return;
	}

	const auto configIt = g_appState.configs.find(lockedConfigIndex);
	if (configIt == g_appState.configs.end()) {
		ImGui::TextWrapped("%s", L("HISTORY_CONFIG_UNAVAILABLE"));
		ImGui::End();
		return;
	}
	Config& config = configIt->second;
	const auto entriesView = GetHistoryEntriesViewForConfig(lockedConfigIndex);
	const auto& entries = *entriesView;
	// 文件状态最多每秒扫描一次；筛选、详情和弹窗复用轻量索引行。
	const vector<HistoryEntryView>& frameViews =
		RefreshHistoryEntryViews(historyController, config, entries);

	ImGui::Text("%s: [No.%d] %s", L("HISTORY_LOCKED_CONFIG"), lockedConfigIndex,
		config.name.c_str());
	ImGui::Separator();

	const vector<wstring>& worlds = RefreshHistoryWorlds(historyController, entries);

	const bool wideToolbar = ImGui::GetContentRegionAvail().x >= metrics.Em(45.0f);
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
	for (const HistoryEntryView& view : frameViews) {

		if (view.status == HistoryFileStatus::Missing
			|| view.status == HistoryFileStatus::Inaccessible) ++missingCount;
	}
	const string cleanLabel = wstring_to_utf8(MineFormatMessage(
		"HISTORY_CLEAN_INVALID_COUNT", static_cast<int>(missingCount)));
	SameLineFits(cleanLabel.c_str());
	ImGui::BeginDisabled(missingCount == 0);
	if (ImGui::Button(cleanLabel.c_str())) {
		ImGui::OpenPopup(L("HISTORY_CONFIRM_CLEAN_TITLE"));
	}
	ImGui::EndDisabled();

	bool historyEntriesChanged = false;
	ConstrainHistoryPopup(metrics);
	if (ImGui::BeginPopupModal(L("HISTORY_CONFIRM_CLEAN_TITLE"), nullptr,
		ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", L("HISTORY_CONFIRM_CLEAN_MSG"));
		if (ImGui::Button(L("BUTTON_OK"))) {
			vector<HistoryEntry> updatedEntries = entries;
			RemoveUnavailableHistoryEntries(updatedEntries, frameViews);
			(void)ReplaceHistoryEntriesForConfig(
				lockedConfigIndex, std::move(updatedEntries));
			historyController.InvalidateFileStatusCache();
			HistoryEntry remainingSelection;
			if (!TryGetHistoryEntry(
					lockedConfigIndex,
					selectedKey.worldName,
					selectedKey.backupFile,
					remainingSelection)) {
				selectedKey = {};
				narrowShowDetails = false;
			}
			ImGui::CloseCurrentPopup();
			historyEntriesChanged = true;
		}
		SameLineFits(L("BUTTON_CANCEL"));
		if (ImGui::Button(L("BUTTON_CANCEL"))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (historyEntriesChanged) {
		// The cached rows contain original-container indices. Stop immediately
		// after a mutation and rebuild them on the next frame.
		ImGui::End();
		return;
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

	const auto& filtered = FilterHistoryEntryViews(historyController, entries,
		frameViews, worldFilter,
		textFilter, static_cast<HistoryStatusFilter>(statusFilterIndex), importantOnly);
	const HistoryEntry* selectedEntry = ResolveHistoryEntry(entries, selectedKey);
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
		const float lineHeight = ImGui::GetTextLineHeight();
		const float cardPadding = metrics.cardPadding;
		const float lineSpacing = metrics.smallGap;
		const float cardHeight = cardPadding * 2.0f + lineHeight * 2.0f + lineSpacing;
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(filtered.size()), cardHeight + metrics.smallGap);
		while (clipper.Step()) for (int itemIndex = clipper.DisplayStart;
			itemIndex < clipper.DisplayEnd; ++itemIndex) {
			const HistoryEntryView& view = frameViews[filtered[itemIndex]];
			const HistoryEntry* resolvedEntry = ResolveHistoryEntryView(entries, view);
			if (!resolvedEntry) continue;
			const HistoryEntry& entry = *resolvedEntry;
			ImGui::PushID(wstring_to_utf8(entry.worldName + L"\n" + entry.backupFile).c_str());
			const bool selected = selectedKey == HistoryEntryKey{entry.worldName, entry.backupFile};
			const string filename = wstring_to_utf8(entry.backupFile);

			// 提取文件名中的类型标签（如 [Full]）
			string displayLabel = filename;
			size_t bracketStart = filename.find('[');
			size_t bracketEnd = filename.find(']');
			if (bracketStart != string::npos && bracketEnd != string::npos && bracketEnd > bracketStart) {
				displayLabel = filename.substr(bracketStart, bracketEnd - bracketStart + 1);
			}

			// 准备卡片数据

			const char* statusIcon = view.status == HistoryFileStatus::Normal
				? ICON_FA_FILE
				: view.status == HistoryFileStatus::CloudOnly
					? ICON_FA_CLOUD : ICON_FA_TRIANGLE_EXCLAMATION;
			const ImVec4 statusIconColor = view.status == HistoryFileStatus::Normal
				? ImVec4(0.4f, 0.85f, 0.5f, 1.0f)
				: view.status == HistoryFileStatus::CloudOnly
					? ImVec4(0.45f, 0.75f, 1.0f, 1.0f)
					: view.status == HistoryFileStatus::SmallFile
						? ImVec4(1.0f, 0.75f, 0.25f, 1.0f)
						: ImVec4(0.95f, 0.45f, 0.35f, 1.0f);

			const string sizeLabel = view.fileSize == 0 ? "-"
				: wstring_to_utf8(MineFormatMessage("HISTORY_SIZE_MB",
					static_cast<double>(view.fileSize) / (1024.0 * 1024.0)));

			// 格式化时间戳，去除 T
			string formattedTimestamp = wstring_to_utf8(entry.timestamp_str);
			size_t tPos = formattedTimestamp.find('T');
			if (tPos != string::npos) {
				formattedTimestamp[tPos] = ' ';
			}

			// 渲染卡片背景
			const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
			const float cardWidth = ImGui::GetContentRegionAvail().x;
			const ImVec2 cardMin = cursorPos;
			const ImVec2 cardMax = ImVec2(cursorPos.x + cardWidth, cursorPos.y + cardHeight);

			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImU32 bgColor = selected
				? ImGui::GetColorU32(ImGuiCol_HeaderActive, 0.4f)
				: ImGui::GetColorU32(ImGuiCol_ChildBg, 0.5f);
			const float cornerRadius = 6.0f;

			// 绘制卡片背景
			drawList->AddRectFilled(cardMin, cardMax, bgColor, cornerRadius);

			// Hover 效果
			ImGui::SetCursorScreenPos(cardMin);
			ImGui::InvisibleButton("##card", ImVec2(cardWidth, cardHeight));
			const bool hovered = ImGui::IsItemHovered();
			if (hovered) {
				drawList->AddRect(cardMin, cardMax,
					ImGui::GetColorU32(ImGuiCol_Border, 0.8f), cornerRadius, 2.0f);
			}
			if (ImGui::IsItemClicked()) {
				selectedKey = {entry.worldName, entry.backupFile};
				narrowShowDetails = !layout.useSplitView;
			}

			// 选中边框
			if (selected) {
				drawList->AddRect(cardMin, cardMax,
					ImGui::GetColorU32(ImGuiCol_HeaderActive), cornerRadius, 2.5f);
			}

			// 渲染卡片内容
			ImGui::SetCursorScreenPos(ImVec2(cardMin.x + cardPadding, cardMin.y + cardPadding));

			// 第一行：图标 + 类型标签 + 注释 + 星标
			ImGui::PushStyleColor(ImGuiCol_Text, statusIconColor);
			ImGui::TextUnformatted(statusIcon);
			ImGui::PopStyleColor();

			ImGui::SameLine();
			ImGui::TextUnformatted(displayLabel.c_str());

			// 如果有注释，显示在同一行
			if (!entry.comment.empty()) {
				ImGui::SameLine();
				const string commentText = wstring_to_utf8(entry.comment);
				const float starWidth = entry.isImportant ? (ImGui::CalcTextSize(ICON_FA_STAR).x + ImGui::GetStyle().ItemSpacing.x) : 0.0f;
				const float availableWidth = cardWidth - (ImGui::GetCursorPosX() - cardMin.x) - starWidth - cardPadding;
				const ImVec4 disabledColor =
					ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
				TextEllipsisWithTooltip(
					commentText.c_str(),
					availableWidth,
					&disabledColor);
			}

			if (hovered) {
				ImGui::SetTooltip("%s", filename.c_str());
			}

			// 星标固定在右上角
			if (entry.isImportant) {
				const float starSize = ImGui::CalcTextSize(ICON_FA_STAR).x;
				ImGui::SameLine(cardWidth - starSize - cardPadding * 2.0f);
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
				ImGui::TextUnformatted(ICON_FA_STAR);
				ImGui::PopStyleColor();
			}

			// 第二行：状态 + 大小 + 世界名 + 时间戳
			ImGui::SetCursorScreenPos(ImVec2(cardMin.x + cardPadding,
				cardMin.y + cardPadding + lineHeight + lineSpacing));
			ImGui::TextDisabled("%s", L(HistoryStatusKey(view.status)));
			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
			ImGui::TextDisabled("%s", sizeLabel.c_str());
			if (worldFilter.empty()) {
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();

				ImGui::TextDisabled("%s", wstring_to_utf8(entry.worldName).c_str());
			}

			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
			ImGui::TextDisabled("%s", formattedTimestamp.c_str());

			// 移动光标到卡片后面，添加间距
			ImGui::SetCursorScreenPos(ImVec2(cardMin.x, cardMax.y + metrics.smallGap * 1.0f));
			ImGui::Dummy(ImVec2(0, 0));

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
		selectedEntry = ResolveHistoryEntry(entries, selectedKey);
		if (!selectedEntry) {
			ImGui::TextWrapped("%s", L("HISTORY_SELECT_PROMPT"));
		}
		else {
			const HistoryEntryView* selectedView =
				FindHistoryEntryView(frameViews, entries, selectedKey);
			const filesystem::path backupPath = filesystem::path(config.backupPath)
				/ selectedEntry->worldName / selectedEntry->backupFile;
			const bool localFile = selectedView
				&& (selectedView->status == HistoryFileStatus::Normal
					|| selectedView->status == HistoryFileStatus::SmallFile);
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
				row(L("HISTORY_LABEL_STATUS"), L(HistoryStatusKey(
					selectedView ? selectedView->status : HistoryFileStatus::Missing)));
				if (localFile) {
					row(L("HISTORY_LABEL_FILE_SIZE"), wstring_to_utf8(MineFormatMessage(
						"HISTORY_SIZE_MB", static_cast<double>(selectedView->fileSize)
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
				const bool important = !selectedEntry->isImportant;
				(void)UpdateHistoryEntry(
					lockedConfigIndex,
					selectedEntry->worldName,
					selectedEntry->backupFile,
					[important](HistoryEntry& entry) { entry.isImportant = important; });
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
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.40f, 0.40f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
			if (ImGui::Button(L("HISTORY_BUTTON_DELETE"))) {
				deleteKey = selectedKey;
				requestDeletePopup = true;
			}
			ImGui::PopStyleColor(2);
		}
		ImGui::EndChild();
	}

	DrawHistoryDialogs(metrics, config, lockedConfigIndex, historyController,
		frameViews, entries);

	ImGui::End();
	if (!showHistoryWindow) {
		historyNeedsInitialViewport = true;
		historyController.Close();
	}
}
