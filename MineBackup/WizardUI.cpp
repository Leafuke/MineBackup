#include "Globals.h"
#include "UIHelpers.h"
#include "imgui-all.h"
#include "imgui_style.h"
#include "i18n.h"
#include "AppState.h"
#include "ConfigManager.h"
#include "text_to_text.h"
#ifdef _WIN32
#include "Platform_win.h"
#elif defined(__APPLE__)
#include "Platform_macos.h"
#else
#include "Platform_linux.h"
#endif

using namespace std;

// 前向声明 MineBackup.cpp 中保留的函数
void ApplyTheme(const int& themeId);
wstring GetDefaultUIFontPath();
string GetRegistryValue(const string& keyPath, const string& valueName);

void ShowConfigWizard(bool& showConfigWizard, bool& errorShow, bool sevenZipExtracted, const wstring& g_7zTempPath) {
	// 首次启动向导使用的静态变量
	static int page = 0, themeId = 5;
	static bool isWizardOpen = true;
	static char saveRootPath[CONSTANT1] = "";
	static char backupPath[CONSTANT1] = "";
	static char zipPath[CONSTANT1] = "";
	static char wizardFontPath[CONSTANT1] = "";
	static int wizardLangIdx = 0;
	static bool wizardLangInitialized = false;

	// 初始化语言选择索引
	if (!wizardLangInitialized) {
		for (int i = 0; i < 2; i++) {
			if (g_CurrentLang == lang_codes[i]) {
				wizardLangIdx = i;
				break;
			}
		}
		wizardLangInitialized = true;
	}

	ImGuiViewport* wizardViewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowViewport(wizardViewport->ID);
	ImGui::SetNextWindowPos(wizardViewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

	if (!isWizardOpen)
		g_appState.done = true;

	ImGui::Begin(L("WIZARD_TITLE"), &isWizardOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

	if (page == 0) {
		ImGui::Text(L("WIZARD_WELCOME"));
		ImGui::Separator();
		ImGui::TextWrapped(L("WIZARD_INTRO1"));
		ImGui::TextWrapped(L("WIZARD_INTRO2"));
		ImGui::TextWrapped(L("WIZARD_INTRO3"));

		ImGui::Dummy(ImVec2(0.0f, 10.0f));
		
		// 语言选择
		ImGui::Text(L("LANGUAGE"));
		if (ImGui::Combo("##WizardLang", &wizardLangIdx, langs, IM_ARRAYSIZE(langs))) {
			SetLanguage(lang_codes[wizardLangIdx]);
			// 切换到中文时，如果字体路径为空，自动设置中文字体
			if (g_CurrentLang == "zh_CN" && strlen(wizardFontPath) == 0) {
#ifdef _WIN32
				wstring defaultCNFont = L"C:\\Windows\\Fonts\\msyh.ttc";
				if (filesystem::exists(defaultCNFont)) {
					Fontss = defaultCNFont;
					strncpy_s(wizardFontPath, wstring_to_utf8(defaultCNFont).c_str(), sizeof(wizardFontPath));
				}
#endif
			}
		}

		ImGui::Dummy(ImVec2(0.0f, 5.0f));
		
		// 字体路径设置
		ImGui::Text(L("WIZARD_FONT_PATH"));
		if (ImGui::Button(L("BUTTON_SELECT_FONT"))) {
			wstring selected_file = SelectFileDialog();
			if (!selected_file.empty()) {
				strncpy_s(wizardFontPath, wstring_to_utf8(selected_file).c_str(), sizeof(wizardFontPath));
				Fontss = selected_file;
			}
		}
		ImGui::SameLine();
		if (ImGui::InputText("##WizardFontPath", wizardFontPath, IM_ARRAYSIZE(wizardFontPath), ImGuiInputTextFlags_EnterReturnsTrue)) {
			if (strlen(wizardFontPath) > 0) {
				Fontss = utf8_to_wstring(wizardFontPath);
			}
		}
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), L("WIZARD_FONT_TIP"));

		ImGui::Dummy(ImVec2(0.0f, 5.0f));

		ImGui::Text(L("THEME_SETTINGS"));
		const char* theme_names[] = { L("THEME_DARK"), L("THEME_LIGHT"), L("THEME_CLASSIC"), L("THEME_WIN_LIGHT"), L("THEME_WIN_DARK"), L("THEME_NORD_LIGHT"), L("THEME_NORD_DARK"), L("THEME_CUSTOM") };
		if (ImGui::Combo("##Theme", &themeId, theme_names, IM_ARRAYSIZE(theme_names))) {
			if (themeId == 7 && !filesystem::exists("custom_theme.json")) {
				// 打开自定义主题编辑器
				ImGuiTheme::WriteDefaultCustomTheme();
				// 打开 custom_theme.json 文件供用户编辑
				OpenFolder(L"custom_theme.json");
			}
			else {
				ApplyTheme(themeId);
			}
		}

		ImGui::SliderFloat(L("UI_SCALE"), &g_uiScale, 0.75f, 2.5f, "%.2f");
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_OK"))) {
			ImGuiIO& io = ImGui::GetIO(); (void)io;
			io.FontGlobalScale = g_uiScale;
		}

		ImGui::Dummy(ImVec2(0.0f, 10.0f));
		if (ImGui::Button(L("BUTTON_START_CONFIG"))) {
			page++;
		}
	}
	else if (page == 1) {
		ImGui::Text(L("WIZARD_STEP1_TITLE"));
		ImGui::TextWrapped(L("WIZARD_STEP1_DESC1"));
		ImGui::TextWrapped(L("WIZARD_STEP1_DESC2"));
		ImGui::Dummy(ImVec2(0.0f, 10.0f));

		// 找路径
		string pathTemp;
		if (ImGui::Button(L("BUTTON_AUTO_JAVA"))) {
			pathTemp.clear();
#ifdef _WIN32
			char* buffer_env_appdata = nullptr;
			_dupenv_s(&buffer_env_appdata, nullptr, "APPDATA");
			if (buffer_env_appdata) {
				filesystem::path candidate = filesystem::path(buffer_env_appdata) / ".minecraft" / "saves";
				if (filesystem::exists(candidate)) {
					pathTemp = candidate.string();
				}
				free(buffer_env_appdata);
			}
#else
			const char* home = std::getenv("HOME");
			if (home) {
				vector<filesystem::path> candidates = {
					filesystem::path(home) / ".minecraft" / "saves",
					filesystem::path(home) / ".var/app/com.mojang.Minecraft/.minecraft/saves",
					filesystem::path(home) / ".local/share/minecraft/saves"
				};
				for (const auto& candidate : candidates) {
					if (filesystem::exists(candidate)) {
						pathTemp = candidate.string();
						break;
					}
				}
			}
#endif
			if (!pathTemp.empty()) {
				strncpy_s(saveRootPath, pathTemp.c_str(), sizeof(saveRootPath));
#ifndef _WIN32
				for (size_t i = 0; saveRootPath[i]; ++i) if (saveRootPath[i] == '\\') saveRootPath[i] = '/';
#endif
			}
			else {
				MessageBoxWin("Info", "Could not auto-detect Java edition saves. Please select manually.", 1);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_AUTO_BEDROCK"))) {
			pathTemp.clear();
#ifdef _WIN32
			char* buffer_env_appdata = nullptr, * buffer_env_username = nullptr;
			_dupenv_s(&buffer_env_appdata, nullptr, "APPDATA");
			_dupenv_s(&buffer_env_username, nullptr, "USERNAME");
			if (buffer_env_appdata && filesystem::exists(filesystem::path(buffer_env_appdata) / "Minecraft Bedrock" / "Users")) {
				pathTemp = (filesystem::path(buffer_env_appdata) / "Minecraft Bedrock" / "Users").string();
				for (const auto& entry : filesystem::directory_iterator(pathTemp)) {
					if (entry.is_directory() && (entry.path().filename().string()[0] - '0') < 10 && (entry.path().filename().string()[0] - '0') >= 0) {
						pathTemp = (filesystem::path(pathTemp) / entry.path().filename() / "games" / "com.mojang" / "minecraftWorlds").string();
						break;
					}
				}
			}
			else if (buffer_env_username && filesystem::exists("C:\\Users\\" + (string)buffer_env_username + "\\Appdata\\Local\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\LocalState\\games\\com.mojang\\minecraftWorlds")) {
				pathTemp = "C:\\Users\\" + (string)buffer_env_username + "\\Appdata\\Local\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\LocalState\\games\\com.mojang\\minecraftWorlds";
			}
			if (buffer_env_appdata) free(buffer_env_appdata);
			if (buffer_env_username) free(buffer_env_username);
#else
			const char* home = std::getenv("HOME");
			if (home) {
				vector<filesystem::path> candidates = {
					filesystem::path(home) / ".local/share/mcpelauncher/games/com.mojang/minecraftWorlds",
					filesystem::path(home) / ".var/app/io.mrarm.mcpelauncher/.local/share/mcpelauncher/games/com.mojang/minecraftWorlds"
				};
				for (const auto& candidate : candidates) {
					if (filesystem::exists(candidate)) {
						pathTemp = candidate.string();
						break;
					}
				}
			}
#endif
			if (!pathTemp.empty()) {
				strncpy_s(saveRootPath, pathTemp.c_str(), sizeof(saveRootPath));
#ifndef _WIN32
				for (size_t i = 0; saveRootPath[i]; ++i) if (saveRootPath[i] == '\\') saveRootPath[i] = '/';
#endif
			}
			else {
				MessageBoxWin("Info", "Could not auto-detect Bedrock edition saves. Please select manually.", 1);
			}
		}
		if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
			wstring selected_folder = SelectFolderDialog();
			if (!selected_folder.empty()) {
				strncpy_s(saveRootPath, wstring_to_utf8(selected_folder).c_str(), sizeof(saveRootPath));
			}
		}
		ImGui::SameLine();
		ImGui::InputText(L("SAVES_ROOT_PATH"), saveRootPath, IM_ARRAYSIZE(saveRootPath));

		ImGui::Dummy(ImVec2(0.0f, 20.0f));
		if (ImGui::Button(L("BUTTON_NEXT"), ImVec2(CalcButtonWidth(L("BUTTON_NEXT")), 0))) {
#ifndef _WIN32
			for (size_t i = 0; saveRootPath[i]; ++i) if (saveRootPath[i] == '\\') saveRootPath[i] = '/';
#endif
			if (strlen(saveRootPath) > 0 && filesystem::exists(utf8_to_wstring(saveRootPath))) {
				page++;
				errorShow = false;
			}
			else {
				errorShow = true;
			}
		}
		if (errorShow)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), L("WIZARD_PATH_EMPTY_OR_INVALID"));
	}
	else if (page == 2) {
		ImGui::Text(L("WIZARD_STEP2_TITLE"));
		ImGui::TextWrapped(L("WIZARD_STEP2_DESC"));
		ImGui::Dummy(ImVec2(0.0f, 10.0f));

		if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
			wstring selected_folder = SelectFolderDialog();
			if (!selected_folder.empty()) {
				strncpy_s(backupPath, wstring_to_utf8(selected_folder).c_str(), sizeof(backupPath));
			}
		}
		ImGui::SameLine();
		ImGui::InputText(L("WIZARD_BACKUP_PATH"), backupPath, IM_ARRAYSIZE(backupPath));

		ImGui::Dummy(ImVec2(0.0f, 20.0f));
		float navBtnWidth = CalcPairButtonWidth(L("BUTTON_PREVIOUS"), L("BUTTON_NEXT"));
		if (ImGui::Button(L("BUTTON_PREVIOUS"), ImVec2(navBtnWidth, 0))) page--;
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_NEXT"), ImVec2(navBtnWidth, 0))) {
#ifndef _WIN32
			for (size_t i = 0; backupPath[i]; ++i) if (backupPath[i] == '\\') backupPath[i] = '/';
#endif
			if (strlen(backupPath) > 0 && filesystem::exists(utf8_to_wstring(backupPath))) {
				page++;
				errorShow = false;
			}
			else {
				errorShow = true;
			}
		}
		if (errorShow)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), L("WIZARD_PATH_EMPTY_OR_INVALID"));
	}
	else if (page == 3) {
		ImGui::Text(L("WIZARD_STEP3_TITLE"));
		ImGui::TextWrapped(L("WIZARD_STEP3_DESC"));
		ImGui::Dummy(ImVec2(0.0f, 10.0f));
		// 检查内嵌的7z是否已释放成功
		if (sevenZipExtracted) {
			string extracted_path_utf8 = wstring_to_utf8(g_7zTempPath);
			strncpy_s(zipPath, extracted_path_utf8.c_str(), sizeof(zipPath));
			ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.4f, 1.0f), L("WIZARD_USING_EMBEDDED_7Z"));
		}
		else {
			// 如果释放失败，执行原来的自动检测逻辑
			if (filesystem::exists("7z.exe"))
			{
				strncpy_s(zipPath, "7z.exe", sizeof(zipPath));
				ImGui::Text(L("AUTODETECTED_7Z"));
			}
			else
			{
				static string zipTemp = GetRegistryValue("Software\\7-Zip", "Path") + "7z.exe";
				strncpy_s(zipPath, zipTemp.c_str(), sizeof(zipPath));
				if (strlen(zipPath) != 0)
					ImGui::Text(L("AUTODETECTED_7Z"));
			}
		}
		if (ImGui::Button(L("BUTTON_SELECT_FILE"))) {
			wstring selected_file = SelectFileDialog();
			if (!selected_file.empty()) {
				strncpy_s(zipPath, wstring_to_utf8(selected_file).c_str(), sizeof(zipPath));
			}
		}
		ImGui::SameLine();
		ImGui::InputText(L("WIZARD_7Z_PATH"), zipPath, IM_ARRAYSIZE(zipPath));

		ImGui::Dummy(ImVec2(0.0f, 20.0f));
		
		ImGui::Checkbox(L("BUTTON_AUTO_SCAN_WORLDS"), &g_AutoScanForWorlds);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_BUTTON_AUTO_SCAN_WORLDS"));

		ImGui::Dummy(ImVec2(0.0f, 20.0f));
		if (ImGui::Button(L("BUTTON_PREVIOUS"))) page--;
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_FINISH_CONFIG"))) {
#ifndef _WIN32
			for (size_t i = 0; saveRootPath[i]; ++i) if (saveRootPath[i] == '\\') saveRootPath[i] = '/';
			for (size_t i = 0; backupPath[i]; ++i) if (backupPath[i] == '\\') backupPath[i] = '/';
			for (size_t i = 0; zipPath[i]; ++i) if (zipPath[i] == '\\') zipPath[i] = '/';
#endif
			if (strlen(saveRootPath) > 0 && strlen(backupPath) > 0 && strlen(zipPath) > 0) {
				// 创建并填充第一个配置
				g_appState.currentConfigIndex = 1;
				Config& initialConfig = g_appState.configs[g_appState.currentConfigIndex];

				// 1. 保存向导中收集的路径
				initialConfig.name = "First";
				initialConfig.saveRoot = utf8_to_wstring(saveRootPath);
				initialConfig.backupPath = utf8_to_wstring(backupPath);
				initialConfig.zipPath = utf8_to_wstring(zipPath);
#ifndef _WIN32
				replace(initialConfig.saveRoot.begin(), initialConfig.saveRoot.end(), L'\\', L'/');
				replace(initialConfig.backupPath.begin(), initialConfig.backupPath.end(), L'\\', L'/');
				replace(initialConfig.zipPath.begin(), initialConfig.zipPath.end(), L'\\', L'/');
#endif

				if (g_AutoScanForWorlds) {
					filesystem::path parent = filesystem::path(utf8_to_wstring(saveRootPath)).lexically_normal().parent_path().parent_path();
					std::error_code parent_ec;
					if (!parent.empty() && filesystem::exists(parent, parent_ec) && !parent_ec) {
						std::error_code iter_ec;
						for (filesystem::directory_iterator it(parent, filesystem::directory_options::skip_permission_denied, iter_ec);
							!iter_ec && it != filesystem::directory_iterator(); ++it) {
							const auto& entry = *it;
							if (!entry.is_directory()) continue;
							std::error_code save_ec;
							if (!filesystem::exists(entry.path() / "save", save_ec) || save_ec) continue;
							int index = CreateNewNormalConfig();
							g_appState.configs[index] = initialConfig;
							g_appState.configs[index].saveRoot = (entry.path() / "save").wstring();
							g_appState.configs[index].worlds.clear();
						}
					}
				}

				// 2. 自动扫描存档目录，填充世界列表
				if (filesystem::exists(initialConfig.saveRoot)) {
					for (auto& entry : filesystem::directory_iterator(initialConfig.saveRoot)) {
						if (entry.is_directory()) {
							// 针对基岩版的特殊处理：把 levelname.txt 里的内容当做文件描述
							
							if (filesystem::exists(entry.path() / "levelname.txt")) {
								ifstream levelNameFile(entry.path() / "levelname.txt");
								string levelName = "";
								getline(levelNameFile, levelName);
								levelNameFile.close();
								initialConfig.worlds.push_back({ entry.path().filename().wstring(), utf8_to_wstring(levelName) });
							}
							else {
								initialConfig.worlds.push_back({ entry.path().filename().wstring(), L"" });
							}
						}
					}
				}

				// 3. 设置合理的默认值
				initialConfig.zipFormat = L"7z";
				initialConfig.zipLevel = 5;
				initialConfig.keepCount = 0;
				initialConfig.backupMode = 1;
				initialConfig.hotBackup = true;
				initialConfig.backupBefore = false;
				initialConfig.skipIfUnchanged = true;
				initialConfig.theme = themeId;
				isSilence = false;
				if (strlen(wizardFontPath) > 0) {
					initialConfig.fontPath = utf8_to_wstring(wizardFontPath);
				} else {
					initialConfig.fontPath = GetDefaultUIFontPath();
				}
				g_appState.specialConfigs.clear();

				// 4. 保存到文件并切换到主应用界面
				SaveConfigs();
				showConfigWizard = false;
				g_appState.showMainApp = true;
			}
		}
		ImGui::Text(L("WIZARD_WARNING_TIPS"));
	}

	ImGui::End();
}
