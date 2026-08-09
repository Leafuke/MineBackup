#include "MainUI.h"
#include "ApplicationActions.h"
#include "AppearanceRuntime.h"
#include "Globals.h"
#include "ImGuiRuntime.h"
#include "MainUiController.h"
#include "SettingsUI.h"
#include "SettingsUIHotkeys.h"
#include "UIHelpers.h"
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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace std;

#define APP_PRINTF_INFO(eventId, ...) \
	MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Application, eventId, __VA_ARGS__)
#define APP_PRINTF_WARNING(eventId, ...) \
	MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Application, eventId, __VA_ARGS__)
#define APP_PRINTF_ERROR(eventId, ...) \
	MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Application, eventId, __VA_ARGS__)
#define PLATFORM_PRINTF_INFO(eventId, ...) \
	MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Platform, eventId, __VA_ARGS__)
#define PLATFORM_PRINTF_WARNING(eventId, ...) \
	MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Platform, eventId, __VA_ARGS__)
#define PLATFORM_PRINTF_ERROR(eventId, ...) \
	MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Platform, eventId, __VA_ARGS__)


namespace {
	std::unique_ptr<MainUiController> controller;

	bool ApplyGuiAutostartSetting(DesktopServices& desktopServices, bool enabled) {
		const auto status = desktopServices.SetAutostart(enabled);
		if (!status.IsAvailable()) {
			PLATFORM_PRINTF_WARNING("platform.autostart.update_failed",
				"Could not %s the GUI login startup entry: %s",
				enabled ? "enable" : "disable",
				wstring_to_utf8(status.diagnostic).c_str());
			MessageBoxWin("MineBackup", L("AUTOSTART_UPDATE_FAILED"), 1);
			return false;
		}

		if (SaveConfigs()) return true;

		// 配置文件写入失败时回滚系统启动项，避免出现“注册表已启用但
		// 配置仍显示关闭”（或反过来）的不一致状态。
		const auto rollback = desktopServices.SetAutostart(!enabled);
		if (!rollback.IsAvailable()) {
			PLATFORM_PRINTF_ERROR("platform.autostart.rollback_failed",
				"Could not roll back the GUI login startup entry after configuration save failure: %s",
				wstring_to_utf8(rollback.diagnostic).c_str());
		}
		return false;
	}
}

void MainUiController::ResetForClose() {
	showAboutWindow = false;
	showImportConfigConfirm = false;
	showImportHistoryConfirm = false;
	pendingImportPath.clear();
	waitingForHotkey = false;
	worldList.ResetSelection();
}

MainUiController& GetMainUiController() {
	if (!controller) {
		controller = std::make_unique<MainUiController>();
	}
	return *controller;
}

void ReleaseMainUiResources()
{
	if (!controller) {
		return;
	}
	ReleaseMainUiGraphicsResources();
	controller.reset();
}

void ReleaseMainUiGraphicsResources()
{
	if (controller) ReleaseWorldListUiResources();
}

void DrawMainUiFrame(const MainUiFrameContext& context)
{
	auto* desktopServices = context.desktopServices;
	auto* wc = context.window;
	const AppPaths& paths = *context.paths;
	const auto& currentGlobalHotkeys = context.currentGlobalHotkeys;
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
auto& config_type = worldUi.configType;
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
ImGui::SetNextWindowPos(viewport->WorkPos);
ImGui::SetNextWindowSize(viewport->WorkSize);
ImGui::SetNextWindowViewport(viewport->ID);
ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

ImGuiWindowFlags host_window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
host_window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
host_window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

ImGui::Begin("MainDockSpaceHost", nullptr, host_window_flags);
ImGui::PopStyleVar(3);

// --- 顶部菜单栏 ---
if (ImGui::BeginMenuBar()) {

	if (ImGui::BeginMenu(L("MENU_FILE"))) {
		// 导出配置
		if (ImGui::MenuItem(L("MENU_EXPORT_CONFIG"))) {
			wstring exportPath = desktopServices->SelectSaveFile(
				L"config_export.ini", L"INI Files (*.ini)\0*.ini\0All Files (*.*)\0*.*\0").path.wstring();
			if (!exportPath.empty()) {
				SaveConfigs(exportPath);
				MB_LOG_I18N_INFO(minebackup::logging::LogCategory::Application,
					"application.config.exported", "LOG_CONFIG_EXPORTED",
					wstring_to_utf8(exportPath).c_str());
			}
		}
		// 导入配置
		if (ImGui::MenuItem(L("MENU_IMPORT_CONFIG"))) {
			wstring importPath = desktopServices->SelectFile().path.wstring();
			if (!importPath.empty() && filesystem::exists(importPath)) {
				pendingImportPath = importPath;
				showImportConfigConfirm = true;
			}
		}
		ImGui::Separator();

		// 导出历史记录
		if (ImGui::MenuItem(L("MENU_EXPORT_HISTORY"))) {
			wstring exportPath = desktopServices->SelectSaveFile(
				L"history_export.json", L"JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0").path.wstring();
			if (!exportPath.empty()) {
				try {
					if (ExportHistoryToFile(exportPath)) {
						MB_LOG_I18N_INFO(minebackup::logging::LogCategory::History,
							"history.export.completed", "LOG_HISTORY_EXPORTED",
							wstring_to_utf8(exportPath).c_str());
					}
					else {
						MB_LOG_ERROR(minebackup::logging::LogCategory::History,

							"history.export.failed", "Failed to export history");
					}
				} catch (const exception& e) {
					MB_LOG_ERROR(minebackup::logging::LogCategory::History,
						"history.export.failed",

						"Failed to export history: {}", e.what());
				}
			}
		}
		// 导入历史记录
		if (ImGui::MenuItem(L("MENU_IMPORT_HISTORY"))) {
			wstring importPath = desktopServices->SelectFile().path.wstring();
			if (!importPath.empty() && filesystem::exists(importPath)) {
				pendingImportPath = importPath;
				showImportHistoryConfirm = true;
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem(L("EXIT"))) {
			g_appState.done = true;
			SaveConfigs();
		}
		ImGui::EndMenu();
	}

	// 导入配置确认对话框
	if (showImportConfigConfirm) {
		ImGui::OpenPopup(L("CONFIRM_IMPORT_CONFIG_TITLE"));
	}
	ImGui::SetNextWindowViewport(viewport->ID);
	if (ImGui::BeginPopupModal(L("CONFIRM_IMPORT_CONFIG_TITLE"), &showImportConfigConfirm, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", L("CONFIRM_IMPORT_CONFIG_MSG"));
		ImGui::Separator();
		float importBtnW = CalcPairButtonWidth(L("BUTTON_CONFIRM"), L("BUTTON_CANCEL"));
		if (ImGui::Button(L("BUTTON_CONFIRM"), ImVec2(importBtnW, 0))) {
			LoadConfigs(filesystem::path(pendingImportPath));
			FinalizeUiScaleMigration(ImGui::GetMainViewport()->DpiScale);
			ApplyTheme();
			SaveConfigs(); // 保存到默认位置
			MB_LOG_I18N_INFO(minebackup::logging::LogCategory::Application,
				"application.config.imported", "LOG_CONFIG_IMPORTED",
				wstring_to_utf8(pendingImportPath).c_str());
			showImportConfigConfirm = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(importBtnW, 0))) {
			showImportConfigConfirm = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	// 导入历史记录确认对话框
	if (showImportHistoryConfirm) {
		ImGui::OpenPopup(L("CONFIRM_IMPORT_HISTORY_TITLE"));
	}
	ImGui::SetNextWindowViewport(viewport->ID);
	if (ImGui::BeginPopupModal(L("CONFIRM_IMPORT_HISTORY_TITLE"), &showImportHistoryConfirm, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", L("CONFIRM_IMPORT_HISTORY_MSG"));
		ImGui::Separator();
		float histBtnW = CalcPairButtonWidth(L("BUTTON_CONFIRM"), L("BUTTON_CANCEL"));
		if (ImGui::Button(L("BUTTON_CONFIRM"), ImVec2(histBtnW, 0))) {
			try {
				if (ImportHistoryFromFile(pendingImportPath, g_appState.currentConfigIndex, true)) {
					LoadHistory();
					MB_LOG_I18N_INFO(minebackup::logging::LogCategory::History,
						"history.import.completed", "LOG_HISTORY_IMPORTED",
						wstring_to_utf8(pendingImportPath).c_str());
				}
				else {
					MB_LOG_ERROR(minebackup::logging::LogCategory::History,
						"history.import.failed", "Failed to import history");
				}
			} catch (const exception& e) {
				MB_LOG_ERROR(minebackup::logging::LogCategory::History,
					"history.import.failed",
					"Failed to import history: {}", e.what());
			}
			showImportHistoryConfirm = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(histBtnW, 0))) {
			showImportHistoryConfirm = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (ImGui::BeginMenu(L("SETTINGS"))) {
		const auto desktopCapabilities = desktopServices->Capabilities();

		const bool autostartAvailable = desktopCapabilities.autostart.IsAvailable();
		if (!autostartAvailable) ImGui::BeginDisabled();
		const bool previousRunOnStartup = g_RunOnStartup;
		if (ImGui::Checkbox(L("RUN_ON_WINDOWS_STARTUP"), &g_RunOnStartup)
			&& !ApplyGuiAutostartSetting(*desktopServices, g_RunOnStartup)) {
			g_RunOnStartup = previousRunOnStartup;
		}
		if (!autostartAvailable) ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", L("TIP_GLOBAL_STARTUP"));
		}
		if (!autostartAvailable) {
			ImGui::TextDisabled("%s", L("AUTOSTART_UNAVAILABLE_NOTICE"));
		}

		const bool previousSilentStartupToTray = g_SilentStartupToTray;
		if (ImGui::Checkbox(L("START_TO_TRAY_ON_AUTOSTART"), &g_SilentStartupToTray)
			&& !SaveConfigs()) {
			g_SilentStartupToTray = previousSilentStartupToTray;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", L("TIP_START_TO_TRAY_ON_AUTOSTART"));
		}
		if (ImGui::BeginMenu(L("LOG_FILE_LEVEL"))) {
			const struct {
				minebackup::logging::LogFileLevel value;
				const char* label;
			} levels[] = {
				{minebackup::logging::LogFileLevel::Off, "LOG_FILE_LEVEL_OFF"},
				{minebackup::logging::LogFileLevel::Info, "LOG_FILE_LEVEL_INFO"},
				{minebackup::logging::LogFileLevel::Debug, "LOG_FILE_LEVEL_DEBUG"},
			};
			for (const auto& level : levels) {
				if (ImGui::MenuItem(L(level.label), nullptr, g_logFileLevel == level.value)) {
					g_logFileLevel = level.value;
					minebackup::logging::SetFileLevel(level.value);
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_LOG_FILE_LEVEL"));
		ImGui::Checkbox(L("BUTTON_AUTO_SCAN_WORLDS"), &g_AutoScanForWorlds);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_BUTTON_AUTO_SCAN_WORLDS"));
		ImGui::Checkbox(L("RECEIVE_NOTICES"), &g_ReceiveNotices);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RECEIVE_NOTICES"));
		ImGui::Checkbox(L("STOP_AUTOBACKUP_ON_EXIT"), &g_StopAutoBackupOnExit);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_STOP_AUTOBACKUP_ON_EXIT"));
		ImGui::Separator();
		// 热键设置右拉栏（鼠标放上去会向右展开两个）
		ImGui::BeginDisabled(!desktopCapabilities.globalHotkeys.IsAvailable());
		if (ImGui::BeginMenu(L("HOTKEY_SETTINGS"))) {
			if (ImGui::MenuItem(L("BUTTON_BACKUP_SELECTED"))) {
				MB_LOG_I18N_INFO(minebackup::logging::LogCategory::Platform,
					"platform.hotkey.capture_started",
					"HOTKEY_INSTRUCTION");
				waitingForHotkey = true;
				whichFunc = 1;
			}
			if (ImGui::MenuItem(L("BUTTON_RESTORE_SELECTED"))) {
				MB_LOG_I18N_INFO(minebackup::logging::LogCategory::Platform,
					"platform.hotkey.capture_started",
					"HOTKEY_INSTRUCTION");
				waitingForHotkey = true;
				whichFunc = 2;
			}
			if (waitingForHotkey) {
				ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s", L("WAITING"));
				for (int key = ImGuiKey_0; key <= ImGuiKey_Z; ++key) {
					if (ImGui::IsKeyPressed((ImGuiKey)key)) {
					waitingForHotkey = false;
					if (whichFunc == 1) {
						const int previousKey = g_hotKeyBackupId;
						g_hotKeyBackupId = ImGuiKeyToPlatformHotkey((ImGuiKey)key);
						const auto status = desktopServices->ConfigureGlobalHotkeys(
							currentGlobalHotkeys());
						if (!status.IsAvailable()) {
							g_hotKeyBackupId = previousKey;
							PLATFORM_PRINTF_ERROR("platform.hotkey.configure_failed",
								"%s", wstring_to_utf8(status.diagnostic).c_str());
							MessageBoxWin("MineBackup", L("HOTKEY_OPERATION_FAILED"), 1);
						}
						MB_LOG_I18N_INFO(minebackup::logging::LogCategory::Platform,
							"platform.hotkey.configured", "HOTKEY_SET_TO",
							(char)g_hotKeyBackupId);
							break;
						}
					else if (whichFunc == 2) {
						const int previousKey = g_hotKeyRestoreId;
						g_hotKeyRestoreId = ImGuiKeyToPlatformHotkey((ImGuiKey)key);
						const auto status = desktopServices->ConfigureGlobalHotkeys(
							currentGlobalHotkeys());
						if (!status.IsAvailable()) {
							g_hotKeyRestoreId = previousKey;
							PLATFORM_PRINTF_ERROR("platform.hotkey.configure_failed",
								"%s", wstring_to_utf8(status.diagnostic).c_str());
							MessageBoxWin("MineBackup", L("HOTKEY_OPERATION_FAILED"), 1);
						}
							MB_LOG_I18N_INFO(minebackup::logging::LogCategory::Platform,
								"platform.hotkey.configured", "HOTKEY_SET_TO",
								(char)g_hotKeyRestoreId);

							break;
						}
						break;
					}
				}
			}
			ImGui::EndMenu();
		}
		ImGui::EndDisabled();
		if (!desktopCapabilities.globalHotkeys.IsAvailable()
			&& ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", wstring_to_utf8(desktopCapabilities.globalHotkeys.diagnostic).c_str());
		}
		ImGui::Separator();
		ImGui::Checkbox(L("CHECK_FOR_UPDATES_ON_STARTUP"), &g_CheckForUpdates);

		ImGui::Separator();
		if (ImGui::MenuItem(L("DETAILED_SETTINGS_BUTTON"))) {
			showSettings = true;
		}

		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu(L("MENU_TOOLS"))) {
		const bool validationRunning = g_CoreValidationRunning.load();
		if (validationRunning) ImGui::BeginDisabled();
		if (ImGui::MenuItem(L("MENU_CORE_VALIDATION"))) {
			StartCoreValidationAsync(false);
		}
		if (validationRunning) ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", validationRunning ? L("TIP_CORE_VALIDATION_RUNNING") : L("TIP_CORE_VALIDATION"));
		}
		ImGui::Separator();
		if (ImGui::MenuItem(L("HISTORY_BUTTON"))) { showHistoryWindow = true; }
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu(L("MENU_HELP"))) {
		if (ImGui::MenuItem(L("MENU_GITHUB"))) {
			(void)desktopServices->OpenUri(L"https://github.com/Leafuke/MineBackup");
		}
		if (ImGui::MenuItem(L("MENU_ISSUE"))) {
			(void)desktopServices->OpenUri(L"https://github.com/Leafuke/MineBackup/issues");
		}
		if (ImGui::MenuItem(L("HELP_DOCUMENT"))) {
			(void)desktopServices->OpenUri(L"https://folderrewind.top/docs/guides/minebackup-v1/overview");
		}
		if (ImGui::MenuItem(L("SPONSOR_ME"))) {
			(void)desktopServices->OpenUri(L"https://afdian.com/a/MineBackup");
		}
		if (ImGui::MenuItem(L("MENU_ABOUT"))) {
			showAboutWindow = true;
			ImGui::OpenPopup(L("MENU_ABOUT"));
		}
		ImGui::EndMenu();
	}


	// 在菜单栏右侧显示更新按钮
	if (g_NewVersionAvailable) {
		ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(L("UPDATE_AVAILABLE_BUTTON")).x - ImGui::GetStyle().FramePadding.x * 2 - 100);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.902f, 0.6f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.9f, 0.5f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
		if (ImGui::Button(L("UPDATE_AVAILABLE_BUTTON"))) {
			ImGui::OpenPopup(L("UPDATE_POPUP_TITLE"));
			open_update_popup = true;
		}
		ImGui::PopStyleColor(3);
		ImGui::SetNextWindowViewport(viewport->ID);
		const UiMetrics updateMetrics = GetUiMetrics();
		const ImGuiStyle& updateStyle = ImGui::GetStyle();
		const float maxUpdateWidth = (std::max)(
			1.0f, viewport->WorkSize.x * 0.90f);
		const float maxUpdateHeight = (std::max)(
			1.0f, viewport->WorkSize.y * 0.90f);
		ImGui::SetNextWindowPos(
			viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(
			ImVec2(
				(std::min)(updateMetrics.Em(40.0f), maxUpdateWidth),
				(std::min)(updateMetrics.Em(34.0f), maxUpdateHeight)),
			ImGuiCond_Appearing);
		ImGui::SetNextWindowSizeConstraints(
			ImVec2(
				(std::min)(updateMetrics.Em(28.0f), maxUpdateWidth),
				(std::min)(updateMetrics.Em(22.0f), maxUpdateHeight)),
			ImVec2(maxUpdateWidth, maxUpdateHeight));
		if (ImGui::BeginPopupModal(
			L("UPDATE_POPUP_TITLE"), &open_update_popup,
			ImGuiWindowFlags_NoResize)) {
			ImGui::TextWrapped(L("UPDATE_POPUP_HEADER"), g_LatestVersionStr.c_str());
			ImGui::Separator();
			ImGui::TextWrapped("%s", L("UPDATE_POPUP_NOTES"));

			const float footerReserve = updateMetrics.frameHeight * 2.0f
				+ updateStyle.ItemSpacing.y * 3.0f + updateMetrics.spacingY + 2.0f;
			const float releaseNotesHeight = (std::max)(
				1.0f, (std::min)(updateMetrics.Em(24.0f),
					ImGui::GetContentRegionAvail().y - footerReserve));
			ImGui::BeginChild(
				"ReleaseNotes", ImVec2(0.0f, releaseNotesHeight), true);
			ImGui::TextWrapped("%s", g_ReleaseNotes.c_str());
			ImGui::EndChild();
			ImGui::Separator();
			const MineBackupUpdateLinks updateLinks = BuildMineBackupUpdateLinks(g_LatestVersionStr);
			const float splitActionWidth = (std::max)(
				1.0f, (ImGui::GetContentRegionAvail().x - updateStyle.ItemSpacing.x) * 0.5f);
			auto openUpdateLink = [&](const string& url) {
				if (url.empty()) return;
				(void)desktopServices->OpenUri(utf8_to_wstring(url));
				open_update_popup = false;
				ImGui::CloseCurrentPopup();
			};
			ImGui::BeginDisabled(!updateLinks.supported);
			if (ImGui::Button(
				L("UPDATE_POPUP_DOWNLOAD_BUTTON"), ImVec2(splitActionWidth, 0))) {
				openUpdateLink(updateLinks.officialDownloadUrl);
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!updateLinks.supported);
			if (ImGui::Button(
				L("UPDATE_POPUP_ACCELERATED_DOWNLOAD_BUTTON"), ImVec2(-1.0f, 0))) {
				openUpdateLink(updateLinks.acceleratedDownloadUrl);
			}
			ImGui::EndDisabled();
			const float secondRowWidth = (std::max)(
				1.0f, (ImGui::GetContentRegionAvail().x - updateStyle.ItemSpacing.x) * 0.5f);
			if (ImGui::Button(
				L("UPDATE_POPUP_CHANGELOG_BUTTON"), ImVec2(secondRowWidth, 0))) {
				openUpdateLink(updateLinks.changelogUrl);
			}
			ImGui::SameLine();
			if (ImGui::Button(
				L("BUTTON_CANCEL"), ImVec2(-1.0f, 0))) {
				open_update_popup = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	if (g_ReceiveNotices && g_NoticeCheckDone && g_NewNoticeAvailable && !notice_popup_opened && !notice_snoozed_this_session) {
		ImGui::OpenPopup(L("NOTICE_POPUP_TITLE"));
		notice_popup_opened = true;
	}

	ImGui::SetNextWindowViewport(viewport->ID);
	if (ImGui::BeginPopupModal(L("NOTICE_POPUP_TITLE"), &notice_popup_opened, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", L("NOTICE_POPUP_DESC"));
		ImGui::Separator();
		const UiMetrics noticeMetrics = GetUiMetrics();
		ImGui::BeginChild("NoticeContent", ImVec2(noticeMetrics.Em(22.0f), noticeMetrics.Em(16.0f)), true);
		ImGui::TextWrapped("%s", g_NoticeContent.c_str());
		ImGui::EndChild();
		ImGui::Separator();
		float noticeBtnWidth = CalcPairButtonWidth(L("NOTICE_CONFIRM"), L("NOTICE_LATER"));
		if (noticeBtnWidth < 250) noticeBtnWidth = 250;
		if (ImGui::Button(L("NOTICE_CONFIRM"), ImVec2(noticeBtnWidth, 0))) {
			g_NoticeLastSeenVersion = g_NoticeUpdatedAt;
			g_NewNoticeAvailable = false;
			notice_snoozed_this_session = true;
			SaveConfigs();
			notice_popup_opened = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("NOTICE_LATER"), ImVec2(noticeBtnWidth, 0))) {
			notice_snoozed_this_session = true;
			notice_popup_opened = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}




	ImGui::EndMenuBar();
}

// 关闭确认对话框
if (g_showCloseConfirmDialog) {
	ImGui::OpenPopup(L("CLOSE_CONFIRM_TITLE"));
	g_showCloseConfirmDialog = false;
}

ImGui::SetNextWindowViewport(viewport->ID);
if (ImGui::BeginPopupModal(L("CLOSE_CONFIRM_TITLE"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
	ImGui::TextWrapped("%s", L("CLOSE_CONFIRM_MSG"));
	ImGui::Separator();

	ImGui::Checkbox(L("CLOSE_REMEMBER_CHOICE"), &tempRememberChoice);

	ImGui::Dummy(ImVec2(0, 10));

	const bool canHideToTray = CanHideToTray(desktopServices->Capabilities());
	const char* minimizeLabel = canHideToTray
		? L("CLOSE_MINIMIZE_TO_TRAY") : L("CLOSE_MINIMIZE_WINDOW");
	if (ImGui::Button(minimizeLabel, ImVec2(200, 0))) {
		if (tempRememberChoice) {
			g_closeAction = 1;
			g_rememberCloseAction = true;
		}
		if (canHideToTray) {
			(void)desktopServices->SetTrayVisible(true);
			g_appState.showMainApp = false;
			glfwHideWindow(wc);
		}
		else {
			glfwIconifyWindow(wc);
		}
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button(L("CLOSE_EXIT_APP"), ImVec2(200, 0))) {
		if (tempRememberChoice) {
			g_closeAction = 2;
			g_rememberCloseAction = true;
		}
		SaveConfigs();
		g_appState.done = true;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(100, 0))) {
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

if (showAboutWindow)
	ImGui::OpenPopup(L("MENU_ABOUT"));


ImGui::SetNextWindowViewport(viewport->ID);
ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
if (ImGui::BeginPopupModal(L("MENU_ABOUT"), &showAboutWindow, ImGuiWindowFlags_AlwaysAutoResize))
{
	ImGui::Text("MineBackup v%s", CURRENT_VERSION.c_str());
	ImGui::Separator();
	ImGui::TextWrapped("%s", wstring_to_utf8(MineFormatMessage("ABOUT_DESCRIPTION", (char)g_hotKeyBackupId, (char)g_hotKeyRestoreId)).c_str());
	ImGui::Text("%s", L("ABOUT_AUTHOR"));

	ImGui::Dummy(ImVec2(0.0f, 10.0f));

	if (ImGui::Button(L("ABOUT_VISIT_GITHUB")))
	{
		(void)desktopServices->OpenUri(L"https://github.com/Leafuke/MineBackup");
	}
	ImGui::SameLine();
	if (ImGui::Button(L("ABOUT_VISIT_BILIBILI")))

	{
		(void)desktopServices->OpenUri(L"https://space.bilibili.com/545429962");
	}

	if (ImGui::Button(L("ABOUT_VISIT_KNOTLINK")))
	{
		(void)desktopServices->OpenUri(L"https://github.com/hxh230802/KnotLink");
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("ABOUT_VISIT_KNOTLINK_TIP"));
	if (ImGui::Button(L("ABOUT_VISIT_FOLDERREWIND")))
	{
		(void)desktopServices->OpenUri(L"https://github.com/Leafuke/FolderRewind");
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("ABOUT_VISIT_FOLDERREWIND_TIP"));
	if (ImGui::Button(L("ABOUT_VISIT_MINEBACKUP-MOD")))
	{
		(void)desktopServices->OpenUri(L"https://modrinth.com/mod/minebackup");
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("ABOUT_VISIT_MINEBACKUP-MOD_TIP"));

	ImGui::Dummy(ImVec2(0.0f, 10.0f));
	ImGui::TextUnformatted(L("ABOUT_QQ_GROUP"));
	ImGui::Dummy(ImVec2(0.0f, 10.0f));
	ImGui::SeparatorText(L("ABOUT_LICENSE_HEADER"));
	ImGui::Text("%s", L("ABOUT_LICENSE_TYPE"));
	ImGui::Text("%s", L("ABOUT_LICENSE_COPYRIGHT"));
	ImGui::Text("%s", L("ABOUT_LICENSE_TEXT"));

	ImGui::Dummy(ImVec2(0.0f, 10.0f));
	if (ImGui::Button(L("BUTTON_OK"), ImVec2(250, 0)))
	{
		showAboutWindow = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}



ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_NoTabBar);

if (first_time_layout) {
	first_time_layout = false;
	ImGui::DockBuilderRemoveNode(dockspace_id); // clear any previous layout
	ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

	ImGuiID dock_main_id = dockspace_id;
	ImGuiID dock_right_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.4f, nullptr, &dock_main_id);
	ImGuiID dock_middle_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.45f, nullptr, &dock_main_id);
	ImGuiID dock_left_id = dock_main_id;

	ImGui::DockBuilderDockWindow(L("WORLD_LIST"), dock_left_id);
	ImGui::DockBuilderDockWindow(L("WORLD_DETAILS_PANE_TITLE"), dock_middle_id);
	ImGui::DockBuilderDockWindow(L("CONSOLE_TITLE"), dock_right_id);
	ImGui::DockBuilderFinish(dockspace_id);
}

ImGui::End(); // End of MainDockSpaceHost
	DrawWorldListUiFrame(context);
if (ImGui::Begin(L("CONSOLE_TITLE"), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
	if (ImGui::BeginTabBar("##logging-tabs")) {
		if (ImGui::BeginTabItem(L("TAB_LOG_PANEL"))) {
			DrawLogPanel();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(L("TAB_COMMAND_CONSOLE"))) {
			DrawCommandConsole();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}
ImGui::End();


if (showSettings) {
	ShowSettingsWindowV2();  // 使用新版横向标签页设置窗口
}
if (showHistoryWindow) {
	if (specialSetting) {
		if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size())
			ShowHistoryWindow(displayWorlds[selectedWorldIndex].baseConfigIndex,
				displayWorlds[selectedWorldIndex].name);
		else {
			auto spIt = g_appState.specialConfigs.find(g_appState.currentConfigIndex);
			if (spIt != g_appState.specialConfigs.end()) {
				auto task = find_if(spIt->second.specialTasks.begin(), spIt->second.specialTasks.end(),
					[](const SpecialTask& item) {
						return item.type == SpecialTaskType::Backup && item.enabled;
					});
				if (task != spIt->second.specialTasks.end()) {
					auto config = find_if(g_appState.configs.begin(), g_appState.configs.end(),
						[&](const auto& item) { return item.second.configId == task->target.configId; });
					if (config != g_appState.configs.end()) ShowHistoryWindow(config->first);
				}
			}
		}
	}
	else {
		ShowHistoryWindow(g_appState.currentConfigIndex);
	}
}
}
