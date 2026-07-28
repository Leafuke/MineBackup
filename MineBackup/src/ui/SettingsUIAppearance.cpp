#include "SettingsUIPrivate.h"
#include "HistoryManager.h"
#include "CloudSyncService.h"
#include "AppPaths.h"
#include "TaskCoordinator.h"
#include "ExternalToolManager.h"
#include "NetworkBackendFactory.h"
#include "NetworkService.h"
#include "Sha256.h"
#include "imgui_style.h"

using namespace std;

static wstring FormatPortableConfigPreview(const PortableConfigMergePreview& preview, const wchar_t* direction) {
	wstringstream text;
	text << direction << L"\n\nAdded: " << preview.added.size()
		<< L"\nUpdated: " << preview.updated.size()
		<< L"\nPreserved: " << preview.preserved.size() << L"\n";
	auto appendIds = [&](const wchar_t* label, const vector<wstring>& ids) {
		if (ids.empty()) return;
		text << L"\n" << label << L":\n";
		const size_t shown = (min)(ids.size(), static_cast<size_t>(20));
		for (size_t index = 0; index < shown; ++index) text << L"  " << ids[index] << L"\n";
		if (shown < ids.size()) text << L"  ... and " << (ids.size() - shown) << L" more\n";
	};
	appendIds(L"Added ConfigId", preview.added);
	appendIds(L"Updated ConfigId", preview.updated);
	appendIds(L"Preserved ConfigId", preview.preserved);
	text << L"\nExcluded on both sides: local paths, tools, credentials, runtime state, special configs, commands, scripts and automation."
		<< L"\n\nNo local or remote data changes until you confirm this preview.";
	return text.str();
}

static wstring PortableConfigFingerprint(const map<int, Config>& configs) {
	const string serialized = PortableConfigDocument::FromLocalConfigs(configs).Serialize();
	Sha256 hash;
	hash.Update(serialized.data(), serialized.size());
	return utf8_to_wstring(hash.FinalHex());
}

static bool IsFontSupportChinese(const wstring& fontPath) {
	if (fontPath.empty()) return false;
	wstring lowerPath = fontPath;
	transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::towlower);
	return (lowerPath.find(L"msyh") != wstring::npos
		|| lowerPath.find(L"msjh") != wstring::npos
		|| lowerPath.find(L"simsun") != wstring::npos
		|| lowerPath.find(L"simhei") != wstring::npos
		|| lowerPath.find(L"simkai") != wstring::npos
		|| lowerPath.find(L"noto") != wstring::npos
		|| lowerPath.find(L"pingfang") != wstring::npos
		|| lowerPath.find(L"heiti") != wstring::npos
		|| lowerPath.find(L"songti") != wstring::npos
		|| lowerPath.find(L"wqy") != wstring::npos
		|| lowerPath.find(L"cjk") != wstring::npos
		|| lowerPath.find(L"yahei") != wstring::npos);
}

static wstring GetChineseFontPath() {
#ifdef _WIN32
	const wstring candidates[] = {
		L"C:\\Windows\\Fonts\\msyh.ttc",
		L"C:\\Windows\\Fonts\\msyh.ttf",
		L"C:\\Windows\\Fonts\\msjh.ttc",
		L"C:\\Windows\\Fonts\\msjh.ttf",
		L"C:\\Windows\\Fonts\\simsun.ttc",
		L"C:\\Windows\\Fonts\\simhei.ttf"
	};
	for (const auto& cand : candidates) {
		if (filesystem::exists(cand)) return cand;
	}
#elif defined(__APPLE__)
	const wstring candidates[] = {
		L"/System/Library/Fonts/PingFang.ttc",
		L"/System/Library/Fonts/STHeiti Light.ttc",
		L"/System/Library/Fonts/STHeiti Medium.ttc"
	};
	for (const auto& cand : candidates) {
		if (filesystem::exists(cand)) return cand;
	}
#else
	const wstring candidates[] = {
		L"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
		L"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"
	};
	for (const auto& cand : candidates) {
		if (filesystem::exists(cand)) return cand;
	}
#endif
	return L"";
}

void DrawAppearanceSettings(Config& cfg) {
	(void)cfg;
	static int lang_idx = 0;
	for (int i = 0; i < IM_ARRAYSIZE(lang_codes); ++i) {
		if (g_CurrentLang == lang_codes[i]) {
			lang_idx = i;
			break;
		}
	}

	ImGui::SetNextItemWidth((std::min)(ImGui::GetContentRegionAvail().x, GetUiMetrics().Em(22.0f)));
	if (ImGui::Combo(L("LANGUAGE"), &lang_idx, langs, IM_ARRAYSIZE(langs))) {
		string oldLang = g_CurrentLang;
		SetLanguage(lang_codes[lang_idx]);

		if (oldLang == "en_US" && g_CurrentLang == "zh_CN") {
			if (!IsFontSupportChinese(Fontss)) {
				wstring chineseFont = GetChineseFontPath();
				if (!chineseFont.empty()) {
					Fontss = chineseFont;
				}
			}
		}

		if (oldLang != g_CurrentLang) {
			g_restartRequired = true;
			g_restartBannerDismissed = false;
		}
	}

	ImGui::Spacing();

	ImGui::Text("%s", L("THEME_SETTINGS"));
	const char* theme_names[] = { L("THEME_DARK"), L("THEME_LIGHT"), L("THEME_CLASSIC"), L("THEME_WIN_LIGHT"), L("THEME_WIN_DARK"), L("THEME_NORD_LIGHT"), L("THEME_NORD_DARK"), L("THEME_CUSTOM") };
	ImGui::SetNextItemWidth((std::min)(ImGui::GetContentRegionAvail().x, GetUiMetrics().Em(22.0f)));
	if (ImGui::Combo("##Theme", &g_theme, theme_names, IM_ARRAYSIZE(theme_names))) {
		const auto customThemePath = GetAppPaths().configRoot / L"custom_theme.json";
		if (g_theme == static_cast<int>(ThemeId::Custom) && !filesystem::exists(customThemePath)) {
			ImGuiTheme::WriteDefaultCustomTheme(customThemePath, g_uiScale);
			ApplyTheme();
			(void)GetDesktopServices()->OpenFolder(customThemePath);
		}
		else {
			ApplyTheme();
		}
	}
	if (g_theme == static_cast<int>(ThemeId::Custom)) {
		const auto customThemePath = GetAppPaths().configRoot / L"custom_theme.json";
		if (ImGui::Button(L("CUSTOM_THEME_OPEN"))) {
			if (!filesystem::exists(customThemePath)) {
				ImGuiTheme::WriteDefaultCustomTheme(customThemePath, g_uiScale);
			}
			(void)GetDesktopServices()->OpenFolder(customThemePath);
		}
		ImGui::SameLine();
		if (ImGui::Button(L("CUSTOM_THEME_RELOAD"))) ApplyTheme();
		ImGui::SameLine();
		if (ImGui::Button(L("CUSTOM_THEME_VALIDATE"))) ApplyTheme();
		if (!g_customThemeError.empty()) {
			ImGui::TextColored(ImVec4(1.0f, 0.40f, 0.35f, 1.0f), "%s",
				g_customThemeError.c_str());
		}
		else {
			ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.45f, 1.0f), "%s",
				L("CUSTOM_THEME_VALID"));
		}
	}

	ImGui::Spacing();

	ImGui::SetNextItemWidth((std::min)(ImGui::GetContentRegionAvail().x, GetUiMetrics().Em(22.0f)));
	if (ImGui::SliderFloat(L("UI_SCALE"), &g_uiScale, 0.75f, 2.5f, "%.2f")) {
		ApplyTheme();
	}

	ImGui::Spacing();

	ImGui::Text("%s", L("FONT_SETTINGS"));
	char Fonts[256];
	strncpy_s(Fonts, wstring_to_utf8(Fontss).c_str(), sizeof(Fonts));
	if (ImGui::Button(L("BUTTON_SELECT_FONT"))) {
		wstring sel = GetDesktopServices()->SelectFile().path.wstring();
		if (!sel.empty()) {
			Fontss = sel;
			g_restartRequired = true;
			g_restartBannerDismissed = false;
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##fontPathValue", Fonts, 256)) {
		Fontss = utf8_to_wstring(Fonts);
		g_restartRequired = true;
		g_restartBannerDismissed = false;
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Text("%s", L("CLOSE_BEHAVIOR_LABEL"));
#ifdef __linux__
	const char* close_behavior_options[] = { L("CLOSE_BEHAVIOR_ASK"), L("CLOSE_BEHAVIOR_MINIMIZE_WINDOW"), L("CLOSE_BEHAVIOR_EXIT") };
#else
	const char* close_behavior_options[] = { L("CLOSE_BEHAVIOR_ASK"), L("CLOSE_BEHAVIOR_MINIMIZE"), L("CLOSE_BEHAVIOR_EXIT") };
#endif
	int close_behavior_idx = g_rememberCloseAction ? g_closeAction : 0;
	ImGui::SetNextItemWidth((std::min)(ImGui::GetContentRegionAvail().x, GetUiMetrics().Em(22.0f)));
	if (ImGui::Combo("##CloseBehavior", &close_behavior_idx, close_behavior_options, IM_ARRAYSIZE(close_behavior_options))) {
		if (close_behavior_idx == 0) {
			g_rememberCloseAction = false;
			g_closeAction = 0;
		}
		else {
			g_rememberCloseAction = true;
			g_closeAction = close_behavior_idx;
		}
	}
}

void DrawCloudSyncSettings(Config& cfg) {
	const int configIndex = g_appState.currentConfigIndex;

	ImGui::SeparatorText(L("CLOUD_TOOLS_CARD"));
	BeginUiCard("##CloudTools");
	ImGui::Checkbox(L("ENABLE_CLOUD_SYNC"), &cfg.cloudSyncEnabled);

	char rclonePathBuf[260];
	strncpy_s(rclonePathBuf, wstring_to_utf8(cfg.rclonePath).c_str(), sizeof(rclonePathBuf));
	ImGui::Text("%s", L("RCLONE_PATH_LABEL"));
	if (ImGui::Button(L("BUTTON_SELECT_RCLONE"))) {
		wstring selected = GetDesktopServices()->SelectFile().path.wstring();
		if (!selected.empty()) {
			cfg.rclonePath = selected;
			strncpy_s(rclonePathBuf, wstring_to_utf8(cfg.rclonePath).c_str(), sizeof(rclonePathBuf));
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##RclonePath", rclonePathBuf, sizeof(rclonePathBuf))) {
		cfg.rclonePath = utf8_to_wstring(rclonePathBuf);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RCLONE_PATH"));

	ImGui::BeginDisabled(g_RcloneInstallRunning);
	const char* rcloneInstallLabel = g_RcloneInstallRunning
		? L("RCLONE_INSTALLING") : L("RCLONE_INSTALL_BUTTON");
	if (ImGui::Button(rcloneInstallLabel, ImVec2(CalcButtonWidth(rcloneInstallLabel), 0))) {
		const auto sevenZip = ExternalToolManager::ResolveSevenZip(cfg.zipPath, GetAppPaths());
		if (!sevenZip.available) {
			g_RcloneInstallSucceeded = false;
			g_RcloneInstallMessage = sevenZip.diagnostic;
		}
		else if (ConfirmMessageBox(
			L("RCLONE_INSTALL_CONFIRM_TITLE"),
			L("RCLONE_INSTALL_CONFIRM_MESSAGE"))) {
			g_RcloneInstallRunning = true;
			g_RcloneInstallSucceeded = false;
			g_RcloneInstallMessage = utf8_to_wstring(L("RCLONE_INSTALL_PROGRESS"));
			const auto backend = CreatePlatformNetworkBackend();
			const auto paths = GetAppPaths();
			const auto sevenZipPath = sevenZip.executable;
			if (!TaskCoordinator::Instance().Submit(L"install-rclone", {L"tool:rclone"},
				[backend, paths, sevenZipPath](stop_token token) {
					NetworkService network(backend);
					const auto install = ExternalToolManager::InstallPinnedRclone(network, sevenZipPath, paths, token);
					TaskEvent event{L"rclone-install-complete", install.error};
					event.values[L"success"] = install.success ? L"1" : L"0";
					event.values[L"path"] = install.executable.wstring();
					TaskCoordinator::Instance().PostEvent(std::move(event));
				})) {
				g_RcloneInstallRunning = false;
				g_RcloneInstallMessage = utf8_to_wstring(L("RCLONE_INSTALL_BUSY"));
			}
		}
	}
	ImGui::EndDisabled();
	if (!g_RcloneInstallMessage.empty()) {
		const ImVec4 color = g_RcloneInstallSucceeded
			? ImVec4(0.30f, 0.75f, 0.35f, 1.0f)
			: ImVec4(0.85f, 0.65f, 0.25f, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::TextWrapped("%s", wstring_to_utf8(g_RcloneInstallMessage).c_str());
		ImGui::PopStyleColor();
	}

	char remotePathBuf[260];
	strncpy_s(remotePathBuf, wstring_to_utf8(cfg.rcloneRemotePath).c_str(), sizeof(remotePathBuf));
	ImGui::Text("%s", L("RCLONE_REMOTE_PATH_LABEL"));
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##RemotePath", remotePathBuf, sizeof(remotePathBuf))) {
		cfg.rcloneRemotePath = utf8_to_wstring(remotePathBuf);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RCLONE_REMOTE_PATH"));

	char workDirBuf[260];
	strncpy_s(workDirBuf, wstring_to_utf8(cfg.cloudWorkingDirectory).c_str(), sizeof(workDirBuf));
	ImGui::Text("%s", L("CLOUD_WORKDIR_LABEL"));
	if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
		wstring selected = GetDesktopServices()->SelectFolder().path.wstring();
		if (!selected.empty()) {
			cfg.cloudWorkingDirectory = selected;
			strncpy_s(workDirBuf, wstring_to_utf8(cfg.cloudWorkingDirectory).c_str(), sizeof(workDirBuf));
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##CloudWorkDir", workDirBuf, sizeof(workDirBuf))) {
		cfg.cloudWorkingDirectory = utf8_to_wstring(workDirBuf);
	}
	EndUiCard();
	ImGui::Spacing();

	ImGui::SeparatorText(L("CLOUD_POLICY_CARD"));
	BeginUiCard("##CloudPolicy");
	const char* syncModes[] = {
		L("CLOUD_MODE_HISTORY_ONLY"),
		L("CLOUD_MODE_HISTORY_AND_BACKUPS")
	};
	ImGui::Text("%s", L("CLOUD_SYNC_MODE_LABEL"));
	ImGui::SetNextItemWidth(260);
	ImGui::Combo("##CloudSyncMode", &cfg.cloudSyncMode, syncModes, IM_ARRAYSIZE(syncModes));

	ImGui::SetNextItemWidth(160);
	ImGui::InputInt(L("CLOUD_TIMEOUT_SECONDS"), &cfg.cloudTimeoutSeconds);
	if (cfg.cloudTimeoutSeconds < 10) cfg.cloudTimeoutSeconds = 10;
	ImGui::SetNextItemWidth(160);
	ImGui::InputInt(L("CLOUD_RETRY_COUNT"), &cfg.cloudRetryCount);
	if (cfg.cloudRetryCount < 0) cfg.cloudRetryCount = 0;
	if (cfg.cloudRetryCount > 5) cfg.cloudRetryCount = 5;

	ImGui::Checkbox(L("CLOUD_SYNC_HISTORY_AFTER_UPLOAD"), &cfg.cloudSyncHistoryAfterUpload);
	ImGui::Checkbox(L("CLOUD_AUTO_DOWNLOAD_BEFORE_RESTORE"), &cfg.cloudAutoDownloadBeforeRestore);
	EndUiCard();

	ImGui::Spacing();
	ImGui::SeparatorText(L("CLOUD_STATUS_CARD"));
	BeginUiCard("##CloudStatus");
	{
		lock_guard<mutex> cloudLock(g_appState.cloudTask.mutex);
		ImGui::Text("%s", L("CLOUD_LAST_STATUS"));
		ImGui::SameLine();
		ImGui::TextWrapped("%s", g_appState.cloudTask.statusText.empty()
			? L("CLOUD_STATUS_IDLE")
			: wstring_to_utf8(g_appState.cloudTask.statusText).c_str());
		ImGui::Text("%s", L("CLOUD_LAST_MESSAGE"));
		ImGui::SameLine();
		ImGui::TextWrapped("%s", g_appState.cloudTask.lastMessage.empty()
			? L("CLOUD_STATUS_NONE")
			: wstring_to_utf8(g_appState.cloudTask.lastMessage).c_str());
	}
	ImGui::Text("%s", L("CLOUD_LAST_RUN_LABEL"));
	ImGui::SameLine();
	ImGui::TextWrapped("%s", cfg.cloudLastRunUtc.empty() ? L("CLOUD_STATUS_NONE") : wstring_to_utf8(cfg.cloudLastRunUtc).c_str());
	ImGui::Text("%s", L("CLOUD_LAST_EXIT_CODE_LABEL"));
	ImGui::SameLine();
	ImGui::Text("%d", cfg.cloudLastExitCode);
	if (!cfg.cloudLastErrorMessage.empty()) {
		ImGui::TextWrapped("%s", wstring_to_utf8(cfg.cloudLastErrorMessage).c_str());
	}
	EndUiCard();

	ImGui::Spacing();
	ImGui::SeparatorText(L("CLOUD_PRIMARY_ACTIONS_CARD"));
	BeginUiCard("##CloudPrimaryActions");
	const bool canRunCloudActions = CanUseCloudActions(cfg);
	const wstring cloudUnavailableReason = GetCloudActionsUnavailableReason(cfg);
	if (!canRunCloudActions) ImGui::BeginDisabled();
	if (ImGui::Button(L("CLOUD_ANALYZE_BUTTON"))) {
		const Config configCopy = cfg;
		TaskCoordinator::Instance().Submit(L"Analyze cloud history",
			{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configIndex](stop_token) {
			AnalyzeCloudHistory(configCopy, configIndex);
		});
	}
	ImGui::SameLine();
	if (ImGui::Button(L("CLOUD_SYNC_NOW_BUTTON"))) {
		const Config configCopy = cfg;
		const CloudSyncMode mode = cfg.cloudSyncMode == static_cast<int>(CloudSyncMode::HistoryAndBackups)
			? CloudSyncMode::HistoryAndBackups : CloudSyncMode::HistoryOnly;
		TaskCoordinator::Instance().Submit(L"Sync cloud data",
			{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configIndex, mode](stop_token) {
			SyncConfigFromCloud(configCopy, configIndex, mode);
		});
	}
	if (!canRunCloudActions) ImGui::EndDisabled();
	if (!canRunCloudActions) {
		ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "%s",
			L("CLOUD_ACTION_UNAVAILABLE"));
		if (!cloudUnavailableReason.empty()) {
			ImGui::TextWrapped("%s", wstring_to_utf8(cloudUnavailableReason).c_str());
		}
	}
	EndUiCard();
	ImGui::Spacing();

	ImGui::SeparatorText(L("CLOUD_MANUAL_ACTIONS_CARD"));
	BeginUiCard("##CloudManualActions");
	if (!canRunCloudActions) ImGui::BeginDisabled();
	if (ImGui::Button(L("CLOUD_UPLOAD_HISTORY_BUTTON"))) {
		const Config configCopy = cfg;
		TaskCoordinator::Instance().Submit(L"Upload cloud history",
			{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configIndex](stop_token) {
			UploadConfigurationHistorySnapshot(configCopy, configIndex);
		});
	}
	if (ImGui::Button(L("CLOUD_EXPORT_CONFIG_BUTTON"))) {
		const Config configCopy = cfg;
		map<int, Config> configsCopy;
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			configsCopy = g_appState.configs;
		}
		TaskCoordinator::Instance().Submit(L"Export cloud configuration",
			{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configsCopy, configIndex](stop_token) {
			const auto preparation = PreparePortableConfigUpload(configCopy, configsCopy);
			TaskEvent event{L"portable-config-preview", preparation.result.detail};
			event.values[L"success"] = preparation.result.success ? L"1" : L"0";
			event.values[L"action"] = L"upload";
			event.values[L"config-index"] = to_wstring(configIndex);
			event.values[L"payload"] = utf8_to_wstring(preparation.payload);
			event.values[L"local-fingerprint"] = PortableConfigFingerprint(configsCopy);
			event.values[L"preview"] = FormatPortableConfigPreview(preparation.preview, L"Upload local portable fields to cloud");
			TaskCoordinator::Instance().PostEvent(std::move(event));
		});
	}
	if (ImGui::Button(L("CLOUD_IMPORT_CONFIG_BUTTON"))) {
		const Config configCopy = cfg;
		map<int, Config> configsCopy;
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			configsCopy = g_appState.configs;
		}
		TaskCoordinator::Instance().Submit(L"Import cloud configuration",
			{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configsCopy, configIndex](stop_token) {
			const auto preparation = PreparePortableConfigImport(configCopy, configsCopy);
			TaskEvent event{L"portable-config-preview", preparation.result.detail};
			event.values[L"success"] = preparation.result.success ? L"1" : L"0";
			event.values[L"action"] = L"import";
			event.values[L"config-index"] = to_wstring(configIndex);
			event.values[L"payload"] = utf8_to_wstring(preparation.payload);
			event.values[L"local-fingerprint"] = PortableConfigFingerprint(configsCopy);
			event.values[L"preview"] = FormatPortableConfigPreview(preparation.preview, L"Import cloud portable fields to this device");
			TaskCoordinator::Instance().PostEvent(std::move(event));
		});
	}
	if (ImGui::Button(L("CLOUD_EXPORT_HISTORY_BUTTON"))) {
		const Config configCopy = cfg;
		TaskCoordinator::Instance().Submit(L"Export cloud history",
			{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configIndex](stop_token) {
			ExportHistoryToCloud(configCopy, configIndex);
		});
	}
	if (ImGui::Button(L("CLOUD_IMPORT_HISTORY_BUTTON"))) {
		const Config configCopy = cfg;
		TaskCoordinator::Instance().Submit(L"Import cloud history",
			{ TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity) }, [configCopy, configIndex](stop_token) {
			ImportHistoryFromCloud(configCopy, configIndex, true);
		});
	}
	if (!canRunCloudActions) ImGui::EndDisabled();
	EndUiCard();

#if MINEBACKUP_ENABLE_V15_MIGRATION
	ImGui::Spacing();
	ImGui::SeparatorText(L("CLOUD_LEGACY_MIGRATION_CARD"));
	BeginUiCard("##CloudLegacyMigration");
	if (!canRunCloudActions) ImGui::BeginDisabled();
	if (ImGui::Button(L("LEGACY_REMOTE_IMPORT_BUTTON"),
		ImVec2(CalcButtonWidth(L("LEGACY_REMOTE_IMPORT_BUTTON")), 0))) {
		if (ConfirmMessageBox(
			L("LEGACY_REMOTE_IMPORT_TITLE"),
			L("LEGACY_REMOTE_IMPORT_MESSAGE"))) {
			const Config configCopy = cfg;
			map<int, Config> configsCopy;
			{
				lock_guard<mutex> lock(g_appState.configsMutex);
				configsCopy = g_appState.configs;
			}
			TaskCoordinator::Instance().Submit(L"Prepare legacy remote configuration import",
				{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
				[configCopy, configsCopy, configIndex](stop_token) {
					const auto preparation = PrepareLegacyPortableConfigImport(configCopy, configsCopy);
					TaskEvent event{L"portable-config-preview", preparation.result.detail};
					event.values[L"success"] = preparation.result.success ? L"1" : L"0";
					event.values[L"action"] = L"import";
					event.values[L"config-index"] = to_wstring(configIndex);
					event.values[L"payload"] = utf8_to_wstring(preparation.payload);
					event.values[L"local-fingerprint"] = PortableConfigFingerprint(configsCopy);
					event.values[L"preview"] = FormatPortableConfigPreview(
						preparation.preview, L"Import filtered legacy remote config.ini to this device");
					TaskCoordinator::Instance().PostEvent(std::move(event));
				});
		}
	}
	if (!canRunCloudActions) ImGui::EndDisabled();
	EndUiCard();
#endif
}
