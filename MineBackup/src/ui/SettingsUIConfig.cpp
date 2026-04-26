#include "SettingsUIPrivate.h"

using namespace std;

void DrawConfigManagementPanel() {
	ImGui::SeparatorText(L("CONFIG_MANAGEMENT"));

	string current_config_label = "None";
	if (g_appState.specialConfigs.count(g_appState.currentConfigIndex)) {
		specialSetting = true;
		current_config_label = "[Sp." + to_string(g_appState.currentConfigIndex) + "] " + g_appState.specialConfigs[g_appState.currentConfigIndex].name;
	}
	else if (g_appState.configs.count(g_appState.currentConfigIndex)) {
		specialSetting = false;
		current_config_label = "[No." + to_string(g_appState.currentConfigIndex) + "] " + g_appState.configs[g_appState.currentConfigIndex].name;
	}
	else {
		return;
	}

	static bool showAddConfigPopup = false, showDeleteConfigPopup = false;

	ImGui::SetNextItemWidth(300);
	if (ImGui::BeginCombo(L("CURRENT_CONFIG"), current_config_label.c_str())) {
		for (auto const& [idx, val] : g_appState.configs) {
			const bool is_selected = (g_appState.currentConfigIndex == idx);
			string label = "[No." + to_string(idx) + "] " + val.name;

			if (ImGui::Selectable(label.c_str(), is_selected)) {
				g_appState.currentConfigIndex = idx;
				specialSetting = false;
				ApplyTheme(val.theme);
			}
			if (is_selected) {
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::Separator();
		for (auto const& [idx, val] : g_appState.specialConfigs) {
			const bool is_selected = (g_appState.currentConfigIndex == idx);
			string label = "[Sp." + to_string(idx) + "] " + val.name;
			if (ImGui::Selectable(label.c_str(), is_selected)) {
				g_appState.currentConfigIndex = idx;
				specialSetting = true;
				ApplyTheme(val.theme);
			}
			if (is_selected) ImGui::SetItemDefaultFocus();
		}

		ImGui::Separator();
		if (ImGui::Selectable(L("BUTTON_ADD_CONFIG"))) {
			showAddConfigPopup = true;
		}

		if (ImGui::Selectable(L("BUTTON_DELETE_CONFIG"))) {
			if ((!specialSetting && g_appState.configs.size() > 1) || (specialSetting && !g_appState.specialConfigs.empty())) {
				showDeleteConfigPopup = true;
			}
		}
		ImGui::EndCombo();
	}

	if (showDeleteConfigPopup) {
		ImGui::OpenPopup(L("CONFIRM_DELETE_TITLE"));
	}
	if (ImGui::BeginPopupModal(L("CONFIRM_DELETE_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		showDeleteConfigPopup = false;
		if (specialSetting) {
			ImGui::Text("[Sp.]");
			ImGui::SameLine();
			ImGui::Text(L("CONFIRM_DELETE_MSG"), g_appState.currentConfigIndex, g_appState.specialConfigs[g_appState.currentConfigIndex].name.c_str());
		}
		else {
			ImGui::Text(L("CONFIRM_DELETE_MSG"), g_appState.currentConfigIndex, g_appState.configs[g_appState.currentConfigIndex].name.c_str());
		}
		ImGui::Separator();
		float btnW = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"));
		if (ImGui::Button(L("BUTTON_OK"), ImVec2(btnW, 0))) {
			if (specialSetting) {
				g_appState.specialConfigs.erase(g_appState.currentConfigIndex);
				g_appState.specialConfigMode = false;
				g_appState.currentConfigIndex = g_appState.configs.empty() ? 0 : g_appState.configs.begin()->first;
			}
			else {
				g_appState.configs.erase(g_appState.currentConfigIndex);
				g_appState.currentConfigIndex = g_appState.configs.begin()->first;
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(btnW, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (showAddConfigPopup) {
		ImGui::OpenPopup(L("ADD_NEW_CONFIG_POPUP_TITLE"));
	}
	if (ImGui::BeginPopupModal(L("ADD_NEW_CONFIG_POPUP_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		showAddConfigPopup = false;
		static int config_type = 0;
		static char new_config_name[128] = "New Config";

		ImGui::Text(L("CONFIG_TYPE_LABEL"));
		ImGui::BeginGroup();
		ImGui::RadioButton(L("CONFIG_TYPE_NORMAL"), &config_type, 0);
		ImGui::TextWrapped("%s", L("CONFIG_TYPE_NORMAL_DESC"));
		ImGui::EndGroup();

		ImGui::Spacing();

		ImGui::BeginGroup();
		ImGui::RadioButton(L("CONFIG_TYPE_SPECIAL"), &config_type, 1);
		ImGui::TextWrapped("%s", L("CONFIG_TYPE_SPECIAL_DESC"));
		ImGui::EndGroup();

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::InputText(L("NEW_CONFIG_NAME_LABEL"), new_config_name, IM_ARRAYSIZE(new_config_name));
		ImGui::Separator();

		float createBtnW = CalcPairButtonWidth(L("CREATE_BUTTON"), L("BUTTON_CANCEL"));
		if (ImGui::Button(L("CREATE_BUTTON"), ImVec2(createBtnW, 0))) {
			if (strlen(new_config_name) > 0) {
				if (config_type == 0) {
					int new_index = CreateNewNormalConfig(new_config_name);
					if (g_appState.configs.count(g_appState.currentConfigIndex)) {
						g_appState.configs[new_index] = g_appState.configs[g_appState.currentConfigIndex];
						g_appState.configs[new_index].name = new_config_name;
						g_appState.configs[new_index].saveRoot.clear();
						g_appState.configs[new_index].backupPath.clear();
						g_appState.configs[new_index].worlds.clear();
						EnsureDefaultBackupBlacklist(g_appState.configs[new_index].blacklist);
						EnsureDefaultRestoreWhitelist();
					}
					g_appState.currentConfigIndex = new_index;
					specialSetting = false;
				}
				else {
					int new_index = CreateNewSpecialConfig(new_config_name);
					g_appState.currentConfigIndex = new_index;
					specialSetting = true;
				}
				showSettings = true;
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(createBtnW, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void DrawPathSettings(Config& cfg) {
	char rootBufA[256];
	strncpy_s(rootBufA, wstring_to_utf8(cfg.saveRoot).c_str(), sizeof(rootBufA));

	ImGui::Text("%s", L("SAVES_ROOT_PATH"));
	if (ImGui::Button(L("BUTTON_SELECT_SAVES_DIR"))) {
		wstring sel = SelectFolderDialog();
		if (!sel.empty()) {
			cfg.saveRoot = sel;
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##SavesRoot", rootBufA, 256)) {
		cfg.saveRoot = utf8_to_wstring(rootBufA);
	}

	ImGui::Spacing();

	char buf[256];
	strncpy_s(buf, wstring_to_utf8(cfg.backupPath).c_str(), sizeof(buf));
	ImGui::Text("%s", L("BACKUP_DEST_PATH_LABEL"));
	if (ImGui::Button(L("BUTTON_SELECT_BACKUP_DIR"))) {
		wstring sel = SelectFolderDialog();
		if (!sel.empty()) {
			cfg.backupPath = sel;
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##BackupPath", buf, 256)) {
		cfg.backupPath = utf8_to_wstring(buf);
	}

	ImGui::Spacing();

	char zipBuf[256];
	strncpy_s(zipBuf, wstring_to_utf8(cfg.zipPath).c_str(), sizeof(zipBuf));
	if (filesystem::exists("7z.exe") && cfg.zipPath.empty()) {
		cfg.zipPath = L"7z.exe";
		ImGui::Text("%s", L("AUTODETECTED_7Z"));
	}
	else if (cfg.zipPath.empty()) {
		string zipPathStr = GetRegistryValue("Software\\7-Zip", "Path") + "7z.exe";
		if (filesystem::exists(zipPathStr)) {
			cfg.zipPath = utf8_to_wstring(zipPathStr);
			ImGui::Text("%s", L("AUTODETECTED_7Z"));
		}
	}
	ImGui::Text("%s", L("7Z_PATH_LABEL"));
	if (ImGui::Button(L("BUTTON_SELECT_7Z"))) {
		wstring sel = SelectFileDialog();
		if (!sel.empty()) {
			cfg.zipPath = sel;
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##ZipPath", zipBuf, 256)) {
		cfg.zipPath = utf8_to_wstring(zipBuf);
	}

	ImGui::Spacing();

	char snapshotPathBuf[256];
	strncpy_s(snapshotPathBuf, wstring_to_utf8(cfg.snapshotPath).c_str(), sizeof(snapshotPathBuf));
	ImGui::Text("%s", L("SNAPSHOT_PATH"));
	if (ImGui::Button(L("BUTTON_SELECT_SNAPSHOT_DIR"))) {
		wstring sel = SelectFolderDialog();
		if (!sel.empty()) {
			cfg.snapshotPath = sel;
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##SnapshotPath", snapshotPathBuf, 256)) {
		cfg.snapshotPath = utf8_to_wstring(snapshotPathBuf);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_SNAPSHOT_PATH"));
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

	ImGui::SetNextItemWidth(300);
	if (ImGui::Combo(L("COMPRESSION_METHOD"), &method_idx, zip_methods, IM_ARRAYSIZE(zip_methods))) {
		cfg.zipMethod = utf8_to_wstring(zip_methods[method_idx]);
		ClampCompressionLevel(cfg.zipMethod, cfg.zipLevel);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_COMPRESSION_METHOD"));

	int max_threads = thread::hardware_concurrency();
	ImGui::SetNextItemWidth(300);
	ImGui::SliderInt(L("CPU_THREAD_COUNT"), &cfg.cpuThreads, 0, max_threads);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_CPU_THREADS"));

	ImGui::SetNextItemWidth(300);
	int minLevel = 1;
	int maxLevel = 9;
	GetCompressionLevelRange(cfg.zipMethod, minLevel, maxLevel);
	ClampCompressionLevel(cfg.zipMethod, cfg.zipLevel);
	ImGui::SliderInt(L("COMPRESSION_LEVEL"), &cfg.zipLevel, minLevel, maxLevel);

	ImGui::SetNextItemWidth(300);
	ImGui::InputInt(L("BACKUPS_TO_KEEP"), &cfg.keepCount);
	ImGui::SameLine();
	ImGui::Checkbox(L("IS_SAFE_DELETE"), &isSafeDelete);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("IS_SAFE_DELETE_TIP"));

	ImGui::SetNextItemWidth(300);
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
	if (ImGui::Button(L("BUTTON_ADD_FILE_BLACKLIST"))) {
		wstring sel = SelectFileDialog();
		if (!sel.empty()) cfg.blacklist.push_back(sel);
	}
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_ADD_FOLDER_BLACKLIST"))) {
		wstring sel = SelectFolderDialog();
		if (!sel.empty()) cfg.blacklist.push_back(sel);
	}
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_ADD_REGEX_BLACKLIST"))) {
		ImGui::OpenPopup(L("ADD_REGEX_RULE_TITLE"));
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", L("TIP_USE_REGEX"));

	static int sel_bl_item = -1;
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_REMOVE_BLACKLIST")) && sel_bl_item >= 0 && sel_bl_item < static_cast<int>(cfg.blacklist.size())) {
		cfg.blacklist.erase(cfg.blacklist.begin() + sel_bl_item);
		sel_bl_item = -1;
	}

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
