#include "HistoryDialogs.h"

#include "AppPaths.h"
#include "AppState.h"
#include "BackupManager.h"
#include "DesktopServices.h"
#include "Globals.h"
#include "GameSessionManager.h"
#include "HistoryManager.h"
#include "TaskCoordinator.h"
#include "i18n.h"
#include "imgui-all.h"
#include "text_to_text.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>

using namespace std;

namespace {
	HistoryEntry* ResolveHistoryEntry(int configIndex, const HistoryEntryKey& key) {
		if (key.Empty()) return nullptr;
		const auto history = g_appState.g_history.find(configIndex);
		if (history == g_appState.g_history.end()) return nullptr;
		for (HistoryEntry& entry : history->second) {
			if (entry.worldName == key.worldName && entry.backupFile == key.backupFile) return &entry;
		}
		return nullptr;
	}

	void ConstrainHistoryPopup(const UiMetrics& metrics) {
		const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
		ImGui::SetNextWindowSizeConstraints(
			ImVec2((min)(metrics.Em(20.0f), work.x * 0.9f),
				(min)(metrics.Em(10.0f), work.y * 0.9f)),
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
}

void DrawHistoryDialogs(
	const UiMetrics& metrics,
	Config& config,
	int lockedConfigIndex,
	HistoryWindowController& controller,
	const vector<HistoryEntryView>& frameViews) {
	auto& requestCommentPopup = controller.requestCommentPopup;
	auto& requestRestorePopup = controller.requestRestorePopup;
	auto& requestDeletePopup = controller.requestDeletePopup;
	auto& commentKey = controller.commentKey;
	auto& restoreKey = controller.restoreKey;
	auto& deleteKey = controller.deleteKey;
	char (&commentBuffer)[1024] = controller.commentBuffer;
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

		auto& restoreMethod = controller.restoreMethod;
		char (&customItems)[2048] = controller.customRestoreItems;
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
				MyFolder world = {worldPath.wstring(), entryCopy.worldName,
					L"", copy, index, -1};
				SubmitUserRestore(world, entryCopy.backupFile, method, items,
					copy.backupBefore);
				ImGui::CloseCurrentPopup();
			}
			SameLineFits(L("BUTTON_SELECT_CUSTOM_FILE"));
			if (ImGui::Button(L("BUTTON_SELECT_CUSTOM_FILE"))) {
				const filesystem::path worldPath = JoinPath(config.saveRoot, entry->worldName);
				if (IsWorldOccupied(worldPath)) {
					MessageBoxWin(L("RESTORE_OVER_RUNNING_WORLD_TITLE"),
						L("RESTORE_EXTERNAL_ACTIVE_BLOCKED"), 1);
				}
				else {
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
		auto& deleteMode = controller.deleteMode;
		auto& useSafeDelete = controller.useSafeDelete;
		if (!entry) {
			ImGui::TextWrapped("%s", L("HISTORY_ENTRY_DISAPPEARED"));
			if (ImGui::Button(L("BUTTON_OK"))) ImGui::CloseCurrentPopup();

		}
		else {
			const HistoryEntryView* view = FindHistoryEntryView(
				frameViews,
				deleteKey);
			const bool localFile = view
				&& (view->status == HistoryFileStatus::Normal
					|| view->status == HistoryFileStatus::SmallFile);
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

}
