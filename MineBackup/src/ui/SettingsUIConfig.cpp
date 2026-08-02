#include "SettingsUIPrivate.h"

#include "ApplicationActions.h"
#include "AppPaths.h"
#include "Broadcast.h"
#include "ExternalToolManager.h"
#include "KnotLinkServerManager.h"
#include "KnotLinkService.h"
#include "MainUI.h"
#include "TaskCoordinator.h"

using namespace std;

static bool IsAsciiOnlyPath(const wstring& value) {
	for (wchar_t ch : value) {
		if (static_cast<unsigned int>(ch) > 127u) {
			return false;
		}
	}
	return true;
}

bool IsWEIntegrationPathValidForSave(const Config& cfg) {
	if (!cfg.enableWEIntegration) return true;
	if (cfg.weSnapshotPath.empty()) return true;
	return IsAsciiOnlyPath(cfg.weSnapshotPath);
}

void DrawConfigManagementPanel() {
	string currentLabel = L("NO_CONFIG");
	if (const auto it = g_appState.specialConfigs.find(g_appState.currentConfigIndex);
		it != g_appState.specialConfigs.end()) {
		specialSetting = true;
		currentLabel = "[Sp." + to_string(it->first) + "] " + it->second.name;
	}
	else if (const auto it = g_appState.configs.find(g_appState.currentConfigIndex);
		it != g_appState.configs.end()) {
		specialSetting = false;
		currentLabel = "[No." + to_string(it->first) + "] " + it->second.name;
	}

	const UiMetrics metrics = GetUiMetrics();
	const float actionWidth = CalcButtonWidth(L("CONFIG_ACTIONS"), metrics.Em(7.0f));
	ImGui::SetNextItemWidth((std::max)(metrics.Em(12.0f),
		ImGui::GetContentRegionAvail().x - actionWidth - metrics.spacingX));
	if (ImGui::BeginCombo("##CurrentConfig", currentLabel.c_str())) {
		for (const auto& [index, config] : g_appState.configs) {
			const bool selected = !specialSetting && g_appState.currentConfigIndex == index;
			const string label = "[No." + to_string(index) + "] " + config.name;
			if (ImGui::Selectable(label.c_str(), selected)) {
				g_appState.currentConfigIndex = index;
				specialSetting = false;
			}
			if (selected) ImGui::SetItemDefaultFocus();
		}
		if (!g_appState.specialConfigs.empty()) ImGui::Separator();
		for (const auto& [index, config] : g_appState.specialConfigs) {
			const bool selected = specialSetting && g_appState.currentConfigIndex == index;
			const string label = "[Sp." + to_string(index) + "] " + config.name;
			if (ImGui::Selectable(label.c_str(), selected)) {
				g_appState.currentConfigIndex = index;
				specialSetting = true;
			}
			if (selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();

	static int pendingCreateType = -1;
	static bool requestDelete = false;
	static char newConfigName[128] = "New Config";
	if (ImGui::Button(L("CONFIG_ACTIONS"), ImVec2(actionWidth, 0.0f))) {
		ImGui::OpenPopup("##ConfigActions");
	}
	if (ImGui::BeginPopup("##ConfigActions")) {
		if (ImGui::MenuItem(L("CONFIG_NEW_NORMAL"))) pendingCreateType = 0;
		if (ImGui::MenuItem(L("CONFIG_NEW_SPECIAL"))) pendingCreateType = 1;
		const bool canCopy = !specialSetting
			&& g_appState.configs.contains(g_appState.currentConfigIndex);
		ImGui::BeginDisabled(!canCopy);
		if (ImGui::MenuItem(L("CONFIG_COPY_CURRENT"))) {
			const int sourceIndex = g_appState.currentConfigIndex;
			const Config source = g_appState.configs.at(sourceIndex);
			const int newIndex = CreateNewNormalConfig(source.name + " - Copy");
			g_appState.configs[newIndex] = source;
			AssignFreshNormalConfigId(newIndex);
			g_appState.configs[newIndex].name = source.name + " - Copy";
			g_appState.currentConfigIndex = newIndex;
			specialSetting = false;
			ImGui::MarkItemEdited(ImGui::GetItemID());
		}
		ImGui::EndDisabled();
		ImGui::Separator();
		const bool canDelete = specialSetting
			? g_appState.specialConfigs.contains(g_appState.currentConfigIndex)
			: g_appState.configs.size() > 1
				&& g_appState.configs.contains(g_appState.currentConfigIndex);
		ImGui::BeginDisabled(!canDelete);
		if (ImGui::MenuItem(L("CONFIG_DELETE_CURRENT"))) requestDelete = true;
		ImGui::EndDisabled();
		ImGui::EndPopup();
	}

	if (pendingCreateType >= 0) ImGui::OpenPopup(L("ADD_NEW_CONFIG_POPUP_TITLE"));
	ImGui::SetNextWindowViewport(ImGui::GetWindowViewport()->ID);
	if (ImGui::BeginPopupModal(L("ADD_NEW_CONFIG_POPUP_TITLE"), nullptr,
		ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", pendingCreateType == 0
			? L("CONFIG_TYPE_NORMAL_DESC") : L("CONFIG_TYPE_SPECIAL_DESC"));
		ImGui::SetNextItemWidth(metrics.Em(22.0f));
		ImGui::InputText(L("NEW_CONFIG_NAME_LABEL"), newConfigName, IM_ARRAYSIZE(newConfigName));
		const float buttonWidth = CalcPairButtonWidth(L("CREATE_BUTTON"), L("BUTTON_CANCEL"));
		ImGui::BeginDisabled(newConfigName[0] == '\0');
		if (ImGui::Button(L("CREATE_BUTTON"), ImVec2(buttonWidth, 0.0f))) {
			const int newIndex = pendingCreateType == 0
				? CreateNewNormalConfig(newConfigName)
				: CreateNewSpecialConfig(newConfigName);
			g_appState.currentConfigIndex = newIndex;
			specialSetting = pendingCreateType == 1;
			pendingCreateType = -1;
			ImGui::MarkItemEdited(ImGui::GetItemID());
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(buttonWidth, 0.0f))) {
			pendingCreateType = -1;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (requestDelete) ImGui::OpenPopup(L("CONFIRM_DELETE_TITLE"));
	ImGui::SetNextWindowViewport(ImGui::GetWindowViewport()->ID);
	if (ImGui::BeginPopupModal(L("CONFIRM_DELETE_TITLE"), nullptr,
		ImGuiWindowFlags_AlwaysAutoResize)) {
		requestDelete = false;
		const string name = specialSetting
			? g_appState.specialConfigs.at(g_appState.currentConfigIndex).name
			: g_appState.configs.at(g_appState.currentConfigIndex).name;
		ImGui::TextWrapped(L("CONFIRM_DELETE_MSG"), g_appState.currentConfigIndex, name.c_str());
		const float buttonWidth = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"));
		if (ImGui::Button(L("BUTTON_OK"), ImVec2(buttonWidth, 0.0f))) {
			if (specialSetting) {
				g_appState.specialConfigs.erase(g_appState.currentConfigIndex);
			}
			else {
				g_appState.configs.erase(g_appState.currentConfigIndex);
			}
			if (!g_appState.configs.empty()) {
				g_appState.currentConfigIndex = g_appState.configs.begin()->first;
				specialSetting = false;
			}
			else if (!g_appState.specialConfigs.empty()) {
				g_appState.currentConfigIndex = g_appState.specialConfigs.begin()->first;
				specialSetting = true;
			}
			ImGui::MarkItemEdited(ImGui::GetItemID());
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(buttonWidth, 0.0f))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

static void DrawResponsivePathField(
	const char* labelKey,
	const char* id,
	wstring& value,
	bool selectFile,
	const char* browseTooltipKey) {
	const UiMetrics metrics = GetUiMetrics();
	ImGui::TextUnformatted(L(labelKey));
	char buffer[512];
	strncpy_s(buffer, wstring_to_utf8(value).c_str(), sizeof(buffer));
	const float browseWidth = metrics.Em(2.5f);
	ImGui::SetNextItemWidth((std::max)(metrics.Em(6.0f),
		ImGui::GetContentRegionAvail().x - browseWidth - metrics.spacingX));
	if (ImGui::InputText(id, buffer, sizeof(buffer))) {
		value = utf8_to_wstring(buffer);
	}
	ImGui::SameLine();
	const string browseId = string("...##Browse") + id;
	if (ImGui::Button(browseId.c_str(), ImVec2(browseWidth, 0.0f))) {
		const auto selected = selectFile
			? GetDesktopServices()->SelectFile()
			: GetDesktopServices()->SelectFolder();
		if (!selected.path.empty()) value = selected.path.wstring();
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L(browseTooltipKey));

	if (value.empty()) {
		ImGui::TextDisabled("%s", L("PATH_STATUS_EMPTY"));
		return;
	}
	error_code error;
	const bool exists = filesystem::exists(value, error) && !error;
	ImGui::TextColored(exists
		? ImVec4(0.35f, 0.80f, 0.45f, 1.0f)
		: ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
		"%s", L(exists ? "PATH_STATUS_VALID" : "PATH_STATUS_NOT_FOUND"));
}

void DrawPathSettings(Config& cfg) {
	if (cfg.pendingLocalBinding) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.20f, 1.0f));
		ImGui::TextWrapped("%s", L("PORTABLE_BINDING_NOTICE"));
		ImGui::PopStyleColor();
	}
	DrawResponsivePathField("SAVES_ROOT_PATH", "##SavesRoot", cfg.saveRoot, false,
		"BUTTON_SELECT_SAVES_DIR");

	ImGui::Spacing();

	DrawResponsivePathField("BACKUP_DEST_PATH_LABEL", "##BackupPath", cfg.backupPath,
		false, "BUTTON_SELECT_BACKUP_DIR");

	ImGui::Spacing();

	if (cfg.zipPath.empty()) {
		const auto detected = ExternalToolManager::ResolveSevenZip({}, GetAppPaths());
		if (detected.available) {
			cfg.zipPath = detected.executable.wstring();
			ImGui::Text("%s", L("AUTODETECTED_7Z"));
		}
	}
	DrawResponsivePathField("7Z_PATH_LABEL", "##ZipPath", cfg.zipPath, true,
		"BUTTON_SELECT_7Z");
	static wstring toolStatus;
	static bool toolStatusOk = false;
	if (ImGui::Button(L("BUTTON_VERIFY_COMPRESSION_TOOL"),
		ImVec2(CalcButtonWidth(L("BUTTON_VERIFY_COMPRESSION_TOOL")), 0))) {
		const auto verified = ExternalToolManager::ResolveSevenZip(cfg.zipPath, GetAppPaths());
		toolStatusOk = verified.available;
		if (verified.available) {
			if (verified.fellBackFromUserPath) {
				cfg.zipPath = verified.executable.wstring();
			}
			toolStatus = verified.fellBackFromUserPath
				? MineFormatMessage("TOOL_FALLBACK_FORMAT", wstring_to_utf8(verified.executable.wstring()).c_str())
				: MineFormatMessage("TOOL_FORMATS_VERIFIED", wstring_to_utf8(verified.executable.wstring()).c_str());
		}
		else {
			toolStatus = verified.diagnostic;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_OPEN_CONFIG_FOLDER"),
		ImVec2(CalcButtonWidth(L("BUTTON_OPEN_CONFIG_FOLDER")), 0))) {
		(void)GetDesktopServices()->OpenFolder(GetAppPaths().configRoot);
	}
	if (!toolStatus.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, toolStatusOk
			? ImVec4(0.30f, 0.75f, 0.35f, 1.0f)
			: ImVec4(0.85f, 0.45f, 0.30f, 1.0f));
		ImGui::TextWrapped("%s", wstring_to_utf8(toolStatus).c_str());
		ImGui::PopStyleColor();
	}
	if (cfg.pendingLocalBinding) {
		error_code bindingError;
		const filesystem::path saveRoot = cfg.saveRoot;
		const filesystem::path backupRoot = cfg.backupPath;
		const bool saveRootValid = saveRoot.is_absolute() && filesystem::is_directory(saveRoot, bindingError) && !bindingError;
		bindingError.clear();
		bool backupRootValid = backupRoot.is_absolute() && filesystem::is_directory(backupRoot, bindingError) && !bindingError;
		if (!backupRootValid && backupRoot.is_absolute() && !backupRoot.parent_path().empty()) {
			bindingError.clear();
			backupRootValid = filesystem::is_directory(backupRoot.parent_path(), bindingError) && !bindingError;
		}
		const bool canCompleteBinding = saveRootValid && backupRootValid && !cfg.zipPath.empty();
		ImGui::BeginDisabled(!canCompleteBinding);
		if (ImGui::Button(L("BUTTON_CONFIRM_LOCAL_BINDING"),
			ImVec2(CalcButtonWidth(L("BUTTON_CONFIRM_LOCAL_BINDING")), 0))) {
			const auto verifiedTool = ExternalToolManager::ResolveSevenZip(cfg.zipPath, GetAppPaths());
			if (verifiedTool.available) {
				cfg.zipPath = verifiedTool.executable.wstring();
				cfg.pendingLocalBinding = false;
				cfg.cloudSyncEnabled = false;
				toolStatusOk = true;
				toolStatus = utf8_to_wstring(L("LOCAL_BINDING_COMPLETED"));
			}
			else {
				toolStatusOk = false;
				toolStatus = verifiedTool.diagnostic;
			}
		}
		ImGui::EndDisabled();
	}

	ImGui::Spacing();

	DrawResponsivePathField("SNAPSHOT_PATH", "##SnapshotPath", cfg.snapshotPath, false,
		"BUTTON_SELECT_SNAPSHOT_DIR");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_SNAPSHOT_PATH"));
}

void DrawSystemIntegrationSettings() {
	ImGui::SeparatorText(L("GROUP_MINEBACKUP_MOD_INTEGRATION"));
	ImGui::TextWrapped("%s", L("TIP_MINEBACKUP_MOD_INTEGRATION_SUMMARY"));
	ImGui::Spacing();

	if (ImGui::Checkbox(L("ENABLE_KNOTLINK"), &g_enableKnotLink)) {
		if (!g_enableKnotLink) {
			CleanupKnotLink();
			minebackup::knotlink::GetKnotLinkServerManager().Refresh(false);
			SaveConfigs();
		}
		else {
			const bool submitted = TaskCoordinator::Instance().Submit(
				L"knotlink-settings-enable", {L"service:knotlink"}, [](stop_token) {
					const bool success = InitKnotLink();
					if (success) {
						BroadcastEvent("app_startup", {{"version", CURRENT_VERSION}});
					}
					TaskEvent event{L"knotlink-settings-enable-complete", {}};
					event.values[L"success"] = success ? L"1" : L"0";
					TaskCoordinator::Instance().PostEvent(std::move(event));
				});
			if (!submitted) g_enableKnotLink = false;
		}
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_ENABLE_KNOTLINK"));

	ImGui::Checkbox(L("KNOTLINK_AUTO_START_SERVER"), &g_autoStartKnotLinkServer);
	auto serverStatus = minebackup::knotlink::GetKnotLinkServerManager().GetStatus();
	ImGui::Text("%s: %s", L("KNOTLINK_CLIENT_STATUS"),
		minebackup::knotlink::GetKnotLinkService().IsRunning()
			? L("KNOTLINK_STATUS_READY")
			: L("KNOTLINK_STATUS_STOPPED"));
	ImGui::Text("%s: %s", L("KNOTLINK_SERVER_STATUS"),
		minebackup::knotlink::KnotLinkServerManager::StateName(serverStatus.state));
	ImGui::Text("%s: %s", L("KNOTLINK_SERVER_VERSION"),
		serverStatus.version.empty() ? L("KNOTLINK_VERSION_UNKNOWN") : serverStatus.version.c_str());
	ImGui::Text("%s: 6370=%s, 6378=%s", L("KNOTLINK_PORT_STATUS"),
		serverStatus.signalPortReady ? L("KNOTLINK_PORT_READY") : L("KNOTLINK_PORT_CLOSED"),
		serverStatus.responderPortReady ? L("KNOTLINK_PORT_READY") : L("KNOTLINK_PORT_CLOSED"));
	if (!serverStatus.message.empty()) {
		ImGui::TextWrapped("%s", serverStatus.message.c_str());
	}

	if (ImGui::Button(L("KNOTLINK_START_RETRY"), ImVec2(-1, 0))) {
		TaskCoordinator::Instance().Submit(
			L"knotlink-settings-start", {L"service:knotlink"}, [](stop_token) {
				const auto status =
					minebackup::knotlink::GetKnotLinkServerManager().StartCompatibleServer();
				if (status.state == minebackup::knotlink::KnotLinkServerState::Ready) {
					if (InitKnotLink()) {
						BroadcastEvent("app_startup", {{"version", CURRENT_VERSION}});
					}
				}
			});
	}
	if (ImGui::Button(L("KNOTLINK_REFRESH_STATUS"), ImVec2(-1, 0))) {
		serverStatus =
			minebackup::knotlink::GetKnotLinkServerManager().Refresh(true);
	}
	ImGui::BeginDisabled(g_KnotLinkInstallRunning);
	if (ImGui::Button(L("KNOTLINK_DOWNLOAD_INSTALLER"), ImVec2(-1, 0))) {
		(void)StartKnotLinkInstallerDownload();
	}
	ImGui::EndDisabled();
	if (!g_KnotLinkInstallMessage.empty()) {
		ImGui::TextWrapped(
			"%s", wstring_to_utf8(g_KnotLinkInstallMessage).c_str());
	}

	if (ImGui::Button(L("MOD_LINK_MINEBACKUP_MODRINTH"), ImVec2(-1, 0))) {
		(void)GetDesktopServices()->OpenUri(L"https://modrinth.com/mod/minebackup");
	}
	if (ImGui::Button(L("MOD_LINK_KNOTLINK_HOME"), ImVec2(-1, 0))) {
		(void)GetDesktopServices()->OpenUri(
			L"https://github.com/KnotLink-Protocol/KnotLinkService");
	}
	if (ImGui::Button(L("MOD_LINK_KNOTLINK_DOWNLOAD"), ImVec2(-1, 0))) {
		(void)GetDesktopServices()->OpenUri(
			L"https://github.com/KnotLink-Protocol/KnotLinkService/releases");
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_KNOTLINK_DOWNLOAD_LINK"));
}

void DrawWorldEditSettings(Config& cfg) {
	ImGui::SeparatorText(L("GROUP_WE_INTEGRATION"));
	ImGui::TextWrapped("%s", L("TIP_WE_INTEGRATION_SUMMARY"));
	ImGui::Spacing();

	ImGui::Checkbox(L("ENABLE_WE_INTEGRATION"), &cfg.enableWEIntegration);

	char weSnapshotPathBuf[256];
	strncpy_s(weSnapshotPathBuf, wstring_to_utf8(cfg.weSnapshotPath).c_str(), sizeof(weSnapshotPathBuf));
	ImGui::Text("%s", L("WE_SNAPSHOT_PATH_LABEL"));
	if (ImGui::Button(L("BUTTON_SELECT_WE_SNAPSHOT_DIR"))) {
		wstring sel = GetDesktopServices()->SelectFolder().path.wstring();
		if (!sel.empty()) {
			cfg.weSnapshotPath = sel;
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##WESnapshotPath", weSnapshotPathBuf, 256)) {
		cfg.weSnapshotPath = utf8_to_wstring(weSnapshotPathBuf);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_WE_SNAPSHOT_PATH"));

	ImGui::TextDisabled("%s", L("WE_ASCII_PATH_REQUIRED"));
	if (!IsWEIntegrationPathValidForSave(cfg)) {
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", L("ERROR_NON_ASCII_PATH"));
	}
}

void DrawWorldManagement(Config& cfg) {
	if (ImGui::Button(L("BUTTON_SCAN_SAVES"))) {
		cfg.worlds.clear();
		if (filesystem::exists(cfg.saveRoot))
			for (auto& e : filesystem::directory_iterator(cfg.saveRoot))
				if (e.is_directory())
					cfg.worlds.push_back({ e.path().filename().wstring(), L"" });
	}

	ImGui::Separator();
	ImGui::Text("%s", L("WORLD_NAME_AND_DESC"));

	if (ImGui::BeginTable("WorldsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn(L("WORLD_NAME"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(L("WORLD_DESC"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed, 60);
		ImGui::TableHeadersRow();

		for (size_t i = 0; i < cfg.worlds.size(); ++i) {
			ImGui::TableNextRow();
			ImGui::PushID(static_cast<int>(i));

			char name[256], desc[256];
			strncpy_s(name, wstring_to_utf8(cfg.worlds[i].first).c_str(), sizeof(name));
			strncpy_s(desc, wstring_to_utf8(cfg.worlds[i].second).c_str(), sizeof(desc));

			ImGui::TableSetColumnIndex(0);
			ImGui::SetNextItemWidth(-1);
			if (ImGui::InputText("##name", name, 256))
				cfg.worlds[i].first = utf8_to_wstring(name);

			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-1);
			if (ImGui::InputText("##desc", desc, 256))
				cfg.worlds[i].second = utf8_to_wstring(desc);

			ImGui::TableSetColumnIndex(2);
			const bool hidden = (cfg.worlds[i].second == L"#");
			if (ImGui::Button(hidden ? ICON_FA_EYE : ICON_FA_EYE_SLASH, ImVec2(-1, 0))) {
				// 描述为 # 时，主界面会隐藏该世界；再次点击恢复为空描述。
				cfg.worlds[i].second = hidden ? L"" : L"#";
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", hidden ? L("TIP_SHOW_WORLD") : L("TIP_HIDE_WORLD"));
			}

			ImGui::PopID();
		}

		ImGui::EndTable();
	}
}

void DrawBackupBehavior(Config& cfg) {
	static int format_choice = (cfg.zipFormat == L"zip") ? 1 : 0;
	ImGui::Text("%s", L("COMPRESSION_FORMAT")); ImGui::SameLine();
	if (ImGui::RadioButton("7z", &format_choice, 0)) { cfg.zipFormat = L"7z"; } ImGui::SameLine();
	if (ImGui::RadioButton("zip", &format_choice, 1)) { cfg.zipFormat = L"zip"; }

	ImGui::Spacing();

	ImGui::Text("%s", L("TEXT_BACKUP_MODE")); ImGui::SameLine();
	ImGui::RadioButton(L("BUTTOM_BACKUP_MODE_NORMAL"), &cfg.backupMode, 1);
	ImGui::SameLine();
	ImGui::RadioButton(L("BUTTOM_BACKUP_MODE_SMART"), &cfg.backupMode, 2);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_SMART_BACKUP"));
	ImGui::SameLine();
	ImGui::RadioButton(L("BUTTOM_BACKUP_MODE_OVERWRITE"), &cfg.backupMode, 3);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_OVERWRITE_BACKUP"));

	if (ImGui::BeginTable("BackupOptions", 2)) {
		ImGui::TableNextColumn();
		ImGui::Checkbox(L("BACKUP_ON_START"), &cfg.backupOnGameStart);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_BACKUP_ON_START"));

		ImGui::TableNextColumn();
		ImGui::Checkbox(L("USE_LOW_PRIORITY"), &cfg.useLowPriority);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_LOW_PRIORITY"));

		ImGui::TableNextColumn();
		ImGui::Checkbox(L("SKIP_IF_UNCHANGED"), &cfg.skipIfUnchanged);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_SKIP_IF_UNCHANGED"));

		ImGui::EndTable();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const char* zip_methods[] = { "LZMA2", "Deflate", "BZip2", "zstd" };
	int method_idx = 0;
	for (int i = 0; i < IM_ARRAYSIZE(zip_methods); ++i) {
		if (_wcsicmp(cfg.zipMethod.c_str(), utf8_to_wstring(zip_methods[i]).c_str()) == 0) {
			method_idx = i;
			break;
		}
	}

	SetStandardControlWidth();
	if (ImGui::Combo(L("COMPRESSION_METHOD"), &method_idx, zip_methods, IM_ARRAYSIZE(zip_methods))) {
		cfg.zipMethod = utf8_to_wstring(zip_methods[method_idx]);
		ClampCompressionLevel(cfg.zipMethod, cfg.zipLevel);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_COMPRESSION_METHOD"));

	int max_threads = thread::hardware_concurrency();
	SetStandardControlWidth();
	ImGui::SliderInt(L("CPU_THREAD_COUNT"), &cfg.cpuThreads, 0, max_threads);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_CPU_THREADS"));

	SetStandardControlWidth();
	int minLevel = 1;
	int maxLevel = 9;
	GetCompressionLevelRange(cfg.zipMethod, minLevel, maxLevel);
	ClampCompressionLevel(cfg.zipMethod, cfg.zipLevel);
	ImGui::SliderInt(L("COMPRESSION_LEVEL"), &cfg.zipLevel, minLevel, maxLevel);

	const bool keepAndSafeDeleteInline =
		ImGui::GetContentRegionAvail().x >= GetUiMetrics().Em(31.0f);
	SetStandardControlWidth();
	ImGui::InputInt(L("BACKUPS_TO_KEEP"), &cfg.keepCount);
	if (keepAndSafeDeleteInline) ImGui::SameLine();
	ImGui::Checkbox(L("IS_SAFE_DELETE"), &isSafeDelete);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("IS_SAFE_DELETE_TIP"));

	SetStandardControlWidth();
	ImGui::InputInt(L("MAX_SMART_BACKUPS"), &cfg.maxSmartBackupsPerFull, 1, 5);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_MAX_SMART_BACKUPS"));

	if (!isSafeDelete && cfg.keepCount <= cfg.maxSmartBackupsPerFull) {
		cfg.keepCount = cfg.maxSmartBackupsPerFull + 1;
	}
}

static void DrawRuleListBox(const char* listId, vector<wstring>& rules, int& selectedItem, const char* emptyKey) {
	if (ImGui::BeginListBox(listId, ImVec2(-FLT_MIN, 5 * ImGui::GetTextLineHeightWithSpacing()))) {
		if (rules.empty()) {
			ImGui::TextDisabled("%s", L(emptyKey));
		}
		else {
			for (int n = 0; n < static_cast<int>(rules.size()); n++) {
				string label = wstring_to_utf8(rules[n]);
				if (ImGui::Selectable(label.c_str(), selectedItem == n)) {
					selectedItem = n;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", label.c_str());
				}
			}
		}
		ImGui::EndListBox();
	}
}

void DrawBlacklistSettings(Config& cfg) {
	static int sel_bl_item = -1;
	const int toolbarColumns = ImGui::GetContentRegionAvail().x >= GetUiMetrics().Em(38.0f)
		? 4 : 2;
	if (ImGui::BeginTable("##BlacklistToolbar", toolbarColumns,
		ImGuiTableFlags_SizingStretchSame)) {
	ImGui::TableNextColumn();
	if (ImGui::Button(L("BUTTON_ADD_FILE_BLACKLIST"), ImVec2(-FLT_MIN, 0.0f))) {
		wstring sel = GetDesktopServices()->SelectFile().path.wstring();
		if (!sel.empty()) cfg.blacklist.push_back(sel);
	}
	ImGui::TableNextColumn();
	if (ImGui::Button(L("BUTTON_ADD_FOLDER_BLACKLIST"), ImVec2(-FLT_MIN, 0.0f))) {
		wstring sel = GetDesktopServices()->SelectFolder().path.wstring();
		if (!sel.empty()) cfg.blacklist.push_back(sel);
	}
	ImGui::TableNextColumn();
	if (ImGui::Button(L("BUTTON_ADD_REGEX_BLACKLIST"), ImVec2(-FLT_MIN, 0.0f))) {
		ImGui::OpenPopup(L("ADD_REGEX_RULE_TITLE"));
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", L("TIP_USE_REGEX"));
	ImGui::TableNextColumn();
	ImGui::BeginDisabled(sel_bl_item < 0
		|| sel_bl_item >= static_cast<int>(cfg.blacklist.size()));
	if (ImGui::Button(L("BUTTON_REMOVE_BLACKLIST"), ImVec2(-FLT_MIN, 0.0f))) {
		cfg.blacklist.erase(cfg.blacklist.begin() + sel_bl_item);
		sel_bl_item = -1;
	}
	ImGui::EndDisabled();
	ImGui::EndTable();
	}

	ImGui::SetNextWindowViewport(ImGui::GetWindowViewport()->ID);
	if (ImGui::BeginPopupModal(L("ADD_REGEX_RULE_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		static char regex_buf[256] = "regex:";
		ImGui::InputText(L("REGEX_PATTERN_LABEL"), regex_buf, IM_ARRAYSIZE(regex_buf));
		ImGui::Separator();
		float regexBtnW = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"));
		if (ImGui::Button(L("BUTTON_OK"), ImVec2(regexBtnW, 0))) {
			if (strlen(regex_buf) > 6) {
				cfg.blacklist.push_back(utf8_to_wstring(regex_buf));
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(regexBtnW, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	static char blacklist_add_buf[256] = "";
	const float addRuleWidth = CalcButtonWidth(L("BUTTON_ADD_RULE"));
	ImGui::SetNextItemWidth((std::max)(80.0f, ImGui::GetContentRegionAvail().x - addRuleWidth - ImGui::GetStyle().ItemSpacing.x));
	ImGui::InputTextWithHint("##blacklist_add", L("RULE_TEXT_HINT"), blacklist_add_buf, IM_ARRAYSIZE(blacklist_add_buf));
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_ADD_RULE"), ImVec2(addRuleWidth, 0)) && strlen(blacklist_add_buf) > 0) {
		cfg.blacklist.push_back(utf8_to_wstring(blacklist_add_buf));
		strcpy_s(blacklist_add_buf, "");
	}

	DrawRuleListBox("##blacklist", cfg.blacklist, sel_bl_item, "BLACKLIST_EMPTY");
}

void DrawRestoreBehavior(Config& cfg) {
	ImGui::Checkbox(L("BACKUP_BEFORE_RESTORE"), &cfg.backupBefore);

	ImGui::Spacing();
	ImGui::SeparatorText(L("RESTORE_WHITELIST_HEADER"));
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_WHITELIST"));

	static char whitelist_add_buf[256] = "";
	const float addWhitelistRuleWidth = CalcButtonWidth(L("BUTTON_ADD_RULE"));
	ImGui::SetNextItemWidth((std::max)(80.0f, ImGui::GetContentRegionAvail().x - addWhitelistRuleWidth - ImGui::GetStyle().ItemSpacing.x));
	ImGui::InputTextWithHint("##whitelist_add", L("RULE_TEXT_HINT"), whitelist_add_buf, IM_ARRAYSIZE(whitelist_add_buf));
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_ADD_RULE"), ImVec2(addWhitelistRuleWidth, 0)) && strlen(whitelist_add_buf) > 0) {
		restoreWhitelist.push_back(utf8_to_wstring(whitelist_add_buf));
		strcpy_s(whitelist_add_buf, "");
	}

	static int sel_wl_item = -1;
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_REMOVE_WHITELIST")) && sel_wl_item >= 0 && sel_wl_item < static_cast<int>(restoreWhitelist.size())) {
		restoreWhitelist.erase(restoreWhitelist.begin() + sel_wl_item);
		sel_wl_item = -1;
	}

	DrawRuleListBox("##whitelist", restoreWhitelist, sel_wl_item, "WHITELIST_EMPTY");
}
