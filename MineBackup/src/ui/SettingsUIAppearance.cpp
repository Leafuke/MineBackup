#include "SettingsUIPrivate.h"

using namespace std;

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
	static int lang_idx = 0;
	static int prev_lang_idx = -1;
	for (int i = 0; i < IM_ARRAYSIZE(lang_codes); ++i) {
		if (g_CurrentLang == lang_codes[i]) {
			lang_idx = i;
			break;
		}
	}
	if (prev_lang_idx == -1) prev_lang_idx = lang_idx;

	ImGui::SetNextItemWidth(300);
	if (ImGui::Combo(L("LANGUAGE"), &lang_idx, langs, IM_ARRAYSIZE(langs))) {
		string oldLang = g_CurrentLang;
		SetLanguage(lang_codes[lang_idx]);

		if (oldLang == "en_US" && g_CurrentLang == "zh_CN") {
			if (!IsFontSupportChinese(Fontss)) {
				wstring chineseFont = GetChineseFontPath();
				if (!chineseFont.empty()) {
					Fontss = chineseFont;
					cfg.fontPath = chineseFont;
				}
			}
		}

		if (oldLang != g_CurrentLang) {
			SaveConfigs();
			ReStartApplication();
		}

		prev_lang_idx = lang_idx;
	}

	ImGui::Spacing();

	ImGui::Text("%s", L("THEME_SETTINGS"));
	const char* theme_names[] = { L("THEME_DARK"), L("THEME_LIGHT"), L("THEME_CLASSIC"), L("THEME_WIN_LIGHT"), L("THEME_WIN_DARK"), L("THEME_NORD_LIGHT"), L("THEME_NORD_DARK"), L("THEME_CUSTOM") };
	ImGui::SetNextItemWidth(300);
	if (ImGui::Combo("##Theme", &cfg.theme, theme_names, IM_ARRAYSIZE(theme_names))) {
		if (cfg.theme == 7 && !filesystem::exists("custom_theme.json")) {
			OpenFolder(L"custom_theme.json");
		}
		else {
			ApplyTheme(cfg.theme);
		}
	}

	ImGui::Spacing();

	ImGui::SetNextItemWidth(300);
	ImGui::SliderFloat(L("UI_SCALE"), &g_uiScale, 0.75f, 2.5f, "%.2f");
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_OK"))) {
		ImGuiIO& io = ImGui::GetIO();
		io.FontGlobalScale = g_uiScale;
	}

	ImGui::Spacing();

	ImGui::Text("%s", L("FONT_SETTINGS"));
	char Fonts[256];
	strncpy_s(Fonts, wstring_to_utf8(cfg.fontPath).c_str(), sizeof(Fonts));
	if (ImGui::Button(L("BUTTON_SELECT_FONT"))) {
		wstring sel = SelectFileDialog();
		if (!sel.empty()) {
			cfg.fontPath = sel;
			Fontss = sel;
		}
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##fontPathValue", Fonts, 256)) {
		cfg.fontPath = utf8_to_wstring(Fonts);
		Fontss = cfg.fontPath;
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
	ImGui::SetNextItemWidth(300);
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
	ImGui::Checkbox(L("ENABLE_CLOUD_SYNC"), &cfg.cloudSyncEnabled);

	ImGui::BeginDisabled(!cfg.cloudSyncEnabled);

	char rclonePathBuf[256];
	strncpy_s(rclonePathBuf, wstring_to_utf8(cfg.rclonePath).c_str(), sizeof(rclonePathBuf));
	ImGui::Text("%s", L("RCLONE_PATH_LABEL"));
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##RclonePath", rclonePathBuf, 256);
	cfg.rclonePath = utf8_to_wstring(rclonePathBuf);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RCLONE_PATH"));

	char remotePathBuf[256];
	strncpy_s(remotePathBuf, wstring_to_utf8(cfg.rcloneRemotePath).c_str(), sizeof(remotePathBuf));
	ImGui::Text("%s", L("RCLONE_REMOTE_PATH_LABEL"));
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##RemotePath", remotePathBuf, 256);
	cfg.rcloneRemotePath = utf8_to_wstring(remotePathBuf);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RCLONE_REMOTE_PATH"));

	ImGui::EndDisabled();
}