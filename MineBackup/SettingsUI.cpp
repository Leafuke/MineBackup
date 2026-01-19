// SettingsUI.cpp - 新的设置界面实现（横向标签页）
#include "imgui-all.h"
#include "AppState.h"
#include "ConfigManager.h"
#include "i18n.h"
#include "text_to_text.h"
#include "TaskSystem.h"
#include <filesystem>
#include <thread>
#include <algorithm>
#include <sstream>

#ifdef _WIN32
#include "Platform_win.h"
#include <windows.h>
#else
#include "Platform_linux.h"
#endif

using namespace std;

// 外部变量声明
extern bool showSettings;
extern bool specialSetting;
extern bool isSilence;
extern bool isSafeDelete;
extern bool g_CheckForUpdates;
extern bool g_ReceiveNotices;
extern bool g_StopAutoBackupOnExit;
extern bool g_RunOnStartup;
extern bool g_AutoScanForWorlds;
extern bool g_autoLogEnabled;
extern bool g_enableKnotLink;
extern int g_closeAction;
extern bool g_rememberCloseAction;
extern int g_hotKeyBackupId;
extern int g_hotKeyRestoreId;
extern float g_uiScale;
extern wstring Fontss;
extern vector<wstring> restoreWhitelist;
extern const char* lang_codes[2];
extern const char* langs[2];

// 前向声明
void ApplyTheme(const int& theme);
wstring SelectFolderDialog();
wstring SelectFileDialog();
void ReStartApplication();
string GetRegistryValue(const string& keyPath, const string& valueName);
void OpenFolder(const wstring& folderPath);

// 设置页面标签索引
static int g_settingsTabIndex = 0;

// 绘制配置管理面板
static void DrawConfigManagementPanel() {
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
        // 普通配置
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
        // 特殊配置
        for (auto const& [idx, val] : g_appState.specialConfigs) {
            const bool is_selected = (g_appState.currentConfigIndex == (idx));
            string label = "[Sp." + to_string((idx)) + "] " + val.name;
            if (ImGui::Selectable(label.c_str(), is_selected)) {
                g_appState.currentConfigIndex = (idx);
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

    // 删除配置弹窗
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
        if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
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
        if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 添加新配置弹窗
    if (showAddConfigPopup) {
        ImGui::OpenPopup(L("ADD_NEW_CONFIG_POPUP_TITLE"));
    }
    if (ImGui::BeginPopupModal(L("ADD_NEW_CONFIG_POPUP_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        showAddConfigPopup = false;
        static int config_type = 0;
        static char new_config_name[128] = "New Config";

        ImGui::Text(L("CONFIG_TYPE_LABEL"));
        
        // 使用更好的布局显示配置类型选项
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

        if (ImGui::Button(L("CREATE_BUTTON"), ImVec2(120, 0))) {
            if (strlen(new_config_name) > 0) {
                if (config_type == 0) {
                    int new_index = CreateNewNormalConfig(new_config_name);
                    if (g_appState.configs.count(g_appState.currentConfigIndex)) {
                        g_appState.configs[new_index] = g_appState.configs[g_appState.currentConfigIndex];
                        g_appState.configs[new_index].name = new_config_name;
                        g_appState.configs[new_index].saveRoot.clear();
                        g_appState.configs[new_index].backupPath.clear();
                        g_appState.configs[new_index].worlds.clear();
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
        if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// 绘制普通配置的路径设置
static void DrawPathSettings(Config& cfg) {
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

// 绘制世界管理设置
static void DrawWorldManagement(Config& cfg) {
    if (ImGui::Button(L("BUTTON_SCAN_SAVES"))) {
        cfg.worlds.clear();
        if (filesystem::exists(cfg.saveRoot))
            for (auto& e : filesystem::directory_iterator(cfg.saveRoot))
                if (e.is_directory())
                    cfg.worlds.push_back({ e.path().filename().wstring(), L"" });
    }

    ImGui::Separator();
    ImGui::Text("%s", L("WORLD_NAME_AND_DESC"));
    
    // 使用表格显示世界列表
    if (ImGui::BeginTable("WorldsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(L("WORLD_NAME"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(L("WORLD_DESC"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        static int itemToDelete = -1;
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
            if (ImGui::Button("X", ImVec2(-1, 0))) {
                itemToDelete = static_cast<int>(i);
            }

            ImGui::PopID();
        }

        if (itemToDelete >= 0 && itemToDelete < static_cast<int>(cfg.worlds.size())) {
            cfg.worlds.erase(cfg.worlds.begin() + itemToDelete);
            itemToDelete = -1;
        }

        ImGui::EndTable();
    }
}

// 绘制备份行为设置
static void DrawBackupBehavior(Config& cfg) {
    // 压缩格式
    static int format_choice = (cfg.zipFormat == L"zip") ? 1 : 0;
    ImGui::Text("%s", L("COMPRESSION_FORMAT")); ImGui::SameLine();
    if (ImGui::RadioButton("7z", &format_choice, 0)) { cfg.zipFormat = L"7z"; } ImGui::SameLine();
    if (ImGui::RadioButton("zip", &format_choice, 1)) { cfg.zipFormat = L"zip"; }

    ImGui::Spacing();

    // 备份模式
    ImGui::Text("%s", L("TEXT_BACKUP_MODE")); ImGui::SameLine();
    ImGui::RadioButton(L("BUTTOM_BACKUP_MODE_NORMAL"), &cfg.backupMode, 1);
    ImGui::SameLine();
    ImGui::RadioButton(L("BUTTOM_BACKUP_MODE_SMART"), &cfg.backupMode, 2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_SMART_BACKUP"));
    ImGui::SameLine();
    ImGui::RadioButton(L("BUTTOM_BACKUP_MODE_OVERWRITE"), &cfg.backupMode, 3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_OVERWRITE_BACKUP"));

    // 命名方式
    ImGui::Text("%s", L("BACKUP_NAMING")); ImGui::SameLine();
    int folder_name_choice = cfg.folderNameType;
    if (ImGui::RadioButton(L("NAME_BY_WORLD"), &folder_name_choice, 0)) { cfg.folderNameType = 0; } ImGui::SameLine();
    if (ImGui::RadioButton(L("NAME_BY_DESC"), &folder_name_choice, 1)) { cfg.folderNameType = 1; }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 复选框选项 - 使用两列布局
    if (ImGui::BeginTable("BackupOptions", 2)) {
        ImGui::TableNextColumn();
        ImGui::Checkbox(L("IS_HOT_BACKUP"), &cfg.hotBackup);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_HOT_BACKUP"));

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

    // 压缩算法
    const char* zip_methods[] = { "LZMA2", "Deflate", "BZip2", "zstd" };
    int method_idx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(zip_methods); ++i) {
        if (_wcsicmp(cfg.zipMethod.c_str(), utf8_to_wstring(zip_methods[i]).c_str()) == 0) {
            method_idx = i;
            break;
        }
    }
    
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo(L("COMPRESSION_METHOD"), &method_idx, zip_methods, IM_ARRAYSIZE(zip_methods))) {
        cfg.zipMethod = utf8_to_wstring(zip_methods[method_idx]);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_COMPRESSION_METHOD"));

    // 滑块和输入
    int max_threads = thread::hardware_concurrency();
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt(L("CPU_THREAD_COUNT"), &cfg.cpuThreads, 0, max_threads);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_CPU_THREADS"));

    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt(L("COMPRESSION_LEVEL"), &cfg.zipLevel, 0, 9);

    ImGui::SetNextItemWidth(150);
    ImGui::InputInt(L("BACKUPS_TO_KEEP"), &cfg.keepCount);
    ImGui::SameLine();
    ImGui::Checkbox(L("IS_SAFE_DELETE"), &isSafeDelete);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("IS_SAFE_DELETE_TIP"));

    ImGui::SetNextItemWidth(150);
    ImGui::InputInt(L("MAX_SMART_BACKUPS"), &cfg.maxSmartBackupsPerFull, 1, 5);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_MAX_SMART_BACKUPS"));

    if (!isSafeDelete && cfg.keepCount <= cfg.maxSmartBackupsPerFull) {
        cfg.keepCount = cfg.maxSmartBackupsPerFull + 1;
    }
}

// 绘制黑名单设置
static void DrawBlacklistSettings(Config& cfg) {
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
        ImGui::OpenPopup("Add Regex Rule");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", L("TIP_USE_REGEX"));

    static int sel_bl_item = -1;
    ImGui::SameLine();
    if (ImGui::Button(L("BUTTON_REMOVE_BLACKLIST")) && sel_bl_item != -1) {
        cfg.blacklist.erase(cfg.blacklist.begin() + sel_bl_item);
        sel_bl_item = -1;
    }

    // 添加正则弹窗
    if (ImGui::BeginPopupModal("Add Regex Rule", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char regex_buf[256] = "regex:";
        ImGui::InputText("Regex Pattern", regex_buf, IM_ARRAYSIZE(regex_buf));
        ImGui::Separator();
        if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
            if (strlen(regex_buf) > 6) {
                cfg.blacklist.push_back(utf8_to_wstring(regex_buf));
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginListBox("##blacklist", ImVec2(-FLT_MIN, 5 * ImGui::GetTextLineHeightWithSpacing()))) {
        if (cfg.blacklist.empty()) {
            ImGui::TextDisabled("No items in blacklist");
        }
        else {
            for (int n = 0; n < static_cast<int>(cfg.blacklist.size()); n++) {
                string label = wstring_to_utf8(cfg.blacklist[n]);
                if (ImGui::Selectable(label.c_str(), sel_bl_item == n)) {
                    sel_bl_item = n;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", label.c_str());
                }
            }
        }
        ImGui::EndListBox();
    }
}

// 绘制还原行为设置
static void DrawRestoreBehavior(Config& cfg) {
    ImGui::Checkbox(L("BACKUP_BEFORE_RESTORE"), &cfg.backupBefore);

    ImGui::Spacing();
    ImGui::SeparatorText(L("RESTORE_WHITELIST_HEADER"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_WHITELIST"));

    static char whitelist_add_buf[256] = "";
    ImGui::InputTextWithHint("##whitelist_add", "file_or_folder_name", whitelist_add_buf, IM_ARRAYSIZE(whitelist_add_buf));
    ImGui::SameLine();
    if (ImGui::Button(L("BUTTON_ADD_WHITELIST")) && strlen(whitelist_add_buf) > 0) {
        restoreWhitelist.push_back(utf8_to_wstring(whitelist_add_buf));
        strcpy_s(whitelist_add_buf, "");
    }

    static int sel_wl_item = -1;
    ImGui::SameLine();
    if (ImGui::Button(L("BUTTON_REMOVE_WHITELIST")) && sel_wl_item != -1) {
        restoreWhitelist.erase(restoreWhitelist.begin() + sel_wl_item);
        sel_wl_item = -1;
    }

    if (ImGui::BeginListBox("##whitelist", ImVec2(-FLT_MIN, 5 * ImGui::GetTextLineHeightWithSpacing()))) {
        if (restoreWhitelist.empty()) {
            ImGui::TextDisabled("Whitelist is empty.");
        }
        else {
            for (int n = 0; n < static_cast<int>(restoreWhitelist.size()); n++) {
                string label = wstring_to_utf8(restoreWhitelist[n]);
                if (ImGui::Selectable(label.c_str(), sel_wl_item == n)) {
                    sel_wl_item = n;
                }
            }
        }
        ImGui::EndListBox();
    }
}

// 绘制外观设置
static void DrawAppearanceSettings(Config& cfg) {
    // 语言选择
    static int lang_idx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(lang_codes); ++i) {
        if (g_CurrentLang == lang_codes[i]) {
            lang_idx = i;
            break;
        }
    }
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo(L("LANGUAGE"), &lang_idx, langs, IM_ARRAYSIZE(langs))) {
        g_CurrentLang = lang_codes[lang_idx];
    }

    ImGui::Spacing();

    // 主题设置
    ImGui::Text("%s", L("THEME_SETTINGS"));
    const char* theme_names[] = { L("THEME_DARK"), L("THEME_LIGHT"), L("THEME_CLASSIC"), L("THEME_WIN_LIGHT"), L("THEME_WIN_DARK"), L("THEME_NORD_LIGHT"), L("THEME_NORD_DARK"), L("THEME_CUSTOM") };
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("##Theme", &cfg.theme, theme_names, IM_ARRAYSIZE(theme_names))) {
        if (cfg.theme == 7 && !filesystem::exists("custom_theme.json")) {
            // 需要创建自定义主题文件
            OpenFolder(L"custom_theme.json");
        }
        else {
            ApplyTheme(cfg.theme);
        }
    }

    ImGui::Spacing();

    // UI缩放
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat(L("UI_SCALE"), &g_uiScale, 0.75f, 2.5f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button(L("BUTTON_OK"))) {
        ImGuiIO& io = ImGui::GetIO();
        io.FontGlobalScale = g_uiScale;
    }

    ImGui::Spacing();

    // 字体设置
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

    // 关闭行为
    ImGui::Text("%s", L("CLOSE_BEHAVIOR_LABEL"));
    const char* close_behavior_options[] = { L("CLOSE_BEHAVIOR_ASK"), L("CLOSE_BEHAVIOR_MINIMIZE"), L("CLOSE_BEHAVIOR_EXIT") };
    int close_behavior_idx = g_rememberCloseAction ? g_closeAction : 0;
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("##CloseBehavior", &close_behavior_idx, close_behavior_options, IM_ARRAYSIZE(close_behavior_options))) {
        if (close_behavior_idx == 0) {
            g_rememberCloseAction = false;
            g_closeAction = 0;
        } else {
            g_rememberCloseAction = true;
            g_closeAction = close_behavior_idx;
        }
    }
}

// 绘制云同步设置
static void DrawCloudSyncSettings(Config& cfg) {
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

// 绘制特殊配置的任务管理器（新版）
static void DrawUnifiedTaskManager(SpecialConfig& spCfg) {
    ImGui::SeparatorText(L("TASK_MANAGER_TITLE"));

    // 任务列表
    static int selectedTaskIndex = -1;
    
    // 工具栏
    if (ImGui::Button(L("TASK_ADD_BACKUP"))) {
        UnifiedTaskV2 newTask;
        newTask.id = spCfg.unifiedTasks.empty() ? 1 : (spCfg.unifiedTasks.back().id + 1);
        newTask.name = "Backup Task " + to_string(newTask.id);
        newTask.type = TaskTypeV2::Backup;
        spCfg.unifiedTasks.push_back(newTask);
    }
    ImGui::SameLine();
    if (ImGui::Button(L("TASK_ADD_COMMAND"))) {
        UnifiedTaskV2 newTask;
        newTask.id = spCfg.unifiedTasks.empty() ? 1 : (spCfg.unifiedTasks.back().id + 1);
        newTask.name = "Command Task " + to_string(newTask.id);
        newTask.type = TaskTypeV2::Command;
        spCfg.unifiedTasks.push_back(newTask);
    }
    ImGui::SameLine();
    if (ImGui::Button(L("TASK_REMOVE")) && selectedTaskIndex >= 0 && selectedTaskIndex < static_cast<int>(spCfg.unifiedTasks.size())) {
        spCfg.unifiedTasks.erase(spCfg.unifiedTasks.begin() + selectedTaskIndex);
        selectedTaskIndex = -1;
    }
    ImGui::SameLine();
    // 上移按钮
    ImGui::BeginDisabled(selectedTaskIndex <= 0);
    if (ImGui::Button(L("TASK_MOVE_UP"))) {
        swap(spCfg.unifiedTasks[selectedTaskIndex], spCfg.unifiedTasks[selectedTaskIndex - 1]);
        selectedTaskIndex--;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    // 下移按钮
    ImGui::BeginDisabled(selectedTaskIndex < 0 || selectedTaskIndex >= static_cast<int>(spCfg.unifiedTasks.size()) - 1);
    if (ImGui::Button(L("TASK_MOVE_DOWN"))) {
        swap(spCfg.unifiedTasks[selectedTaskIndex], spCfg.unifiedTasks[selectedTaskIndex + 1]);
        selectedTaskIndex++;
    }
    ImGui::EndDisabled();

    ImGui::Spacing();

    // 任务列表表格
    if (ImGui::BeginTable("TasksTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn(L("TASK_NAME"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(L("TASK_TYPE"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(L("TASK_EXEC_MODE"), ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn(L("TASK_TRIGGER"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(L("TASK_ENABLED"), ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(spCfg.unifiedTasks.size()); ++i) {
            auto& task = spCfg.unifiedTasks[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);

            // 序号
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(to_string(i + 1).c_str(), selectedTaskIndex == i, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedTaskIndex = i;
            }

            // 名称
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", task.name.c_str());

            // 类型
            ImGui::TableSetColumnIndex(2);
            const char* typeNames[] = { L("TASK_TYPE_BACKUP"), L("TASK_TYPE_COMMAND"), L("TASK_TYPE_SCRIPT") };
            ImGui::Text("%s", typeNames[static_cast<int>(task.type)]);

            // 执行模式
            ImGui::TableSetColumnIndex(3);
            const char* execModeNames[] = { L("TASK_EXEC_SEQUENTIAL"), L("TASK_EXEC_PARALLEL") };
            ImGui::Text("%s", execModeNames[static_cast<int>(task.executionMode)]);

            // 触发模式
            ImGui::TableSetColumnIndex(4);
            const char* triggerNames[] = { L("SCHED_MODES_ONCE"), L("SCHED_MODES_INTERVAL"), L("SCHED_MODES_SCHED") };
            ImGui::Text("%s", triggerNames[static_cast<int>(task.triggerMode)]);

            // 启用
            ImGui::TableSetColumnIndex(5);
            ImGui::Checkbox("##enabled", &task.enabled);

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    // 选中任务的详细设置
    if (selectedTaskIndex >= 0 && selectedTaskIndex < static_cast<int>(spCfg.unifiedTasks.size())) {
        auto& task = spCfg.unifiedTasks[selectedTaskIndex];
        
        ImGui::Spacing();
        ImGui::SeparatorText(L("TASK_DETAILS"));

        // 任务名称
        char nameBuf[128];
        strncpy_s(nameBuf, task.name.c_str(), sizeof(nameBuf));
        ImGui::SetNextItemWidth(300);
        if (ImGui::InputText(L("TASK_NAME"), nameBuf, sizeof(nameBuf))) {
            task.name = nameBuf;
        }

        // 执行模式
        int execMode = static_cast<int>(task.executionMode);
        ImGui::Text("%s", L("TASK_EXEC_MODE_LABEL"));
        ImGui::RadioButton(L("TASK_EXEC_SEQUENTIAL"), &execMode, 0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_TASK_EXEC_SEQUENTIAL"));
        ImGui::SameLine();
        ImGui::RadioButton(L("TASK_EXEC_PARALLEL"), &execMode, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_TASK_EXEC_PARALLEL"));
        task.executionMode = static_cast<TaskExecMode>(execMode);

        ImGui::Spacing();

        // 根据任务类型显示不同设置
        if (task.type == TaskTypeV2::Backup) {
            // 配置选择
            string current_config_label = g_appState.configs.count(task.configIndex) ? 
                (string(L("CONFIG_N")) + to_string(task.configIndex)) : "None";
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo(L("CONFIG_COMBO"), current_config_label.c_str())) {
                for (auto const& [idx, val] : g_appState.configs) {
                    if (ImGui::Selectable((string(L("CONFIG_N")) + to_string(idx) + " - " + val.name).c_str(), task.configIndex == idx)) {
                        task.configIndex = idx;
                        task.worldIndex = val.worlds.empty() ? -1 : 0;
                    }
                }
                ImGui::EndCombo();
            }

            // 世界选择
            if (g_appState.configs.count(task.configIndex)) {
                Config& selected_cfg = g_appState.configs[task.configIndex];
                string current_world_label = "None";
                if (!selected_cfg.worlds.empty() && task.worldIndex >= 0 && task.worldIndex < static_cast<int>(selected_cfg.worlds.size())) {
                    current_world_label = wstring_to_utf8(selected_cfg.worlds[task.worldIndex].first);
                }
                ImGui::SetNextItemWidth(200);
                if (ImGui::BeginCombo(L("WORLD_COMBO"), current_world_label.c_str())) {
                    for (int w_idx = 0; w_idx < static_cast<int>(selected_cfg.worlds.size()); ++w_idx) {
                        if (ImGui::Selectable(wstring_to_utf8(selected_cfg.worlds[w_idx].first).c_str(), task.worldIndex == w_idx)) {
                            task.worldIndex = w_idx;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }
        else if (task.type == TaskTypeV2::Command) {
            // 命令输入
            char cmdBuf[512];
            strncpy_s(cmdBuf, wstring_to_utf8(task.command).c_str(), sizeof(cmdBuf));
            ImGui::Text("%s", L("TASK_COMMAND_LABEL"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##command", cmdBuf, sizeof(cmdBuf))) {
                task.command = utf8_to_wstring(cmdBuf);
            }

            // 工作目录
            char workDirBuf[256];
            strncpy_s(workDirBuf, wstring_to_utf8(task.workingDirectory).c_str(), sizeof(workDirBuf));
            ImGui::Text("%s", L("TASK_WORKDIR_LABEL"));
            if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
                wstring sel = SelectFolderDialog();
                if (!sel.empty()) task.workingDirectory = sel;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##workdir", workDirBuf, sizeof(workDirBuf))) {
                task.workingDirectory = utf8_to_wstring(workDirBuf);
            }
        }

        ImGui::Spacing();

        // 触发模式设置
        int triggerMode = static_cast<int>(task.triggerMode);
        ImGui::Text("%s", L("TASK_TRIGGER_LABEL"));
        ImGui::RadioButton(L("SCHED_MODES_ONCE"), &triggerMode, 0); ImGui::SameLine();
        ImGui::RadioButton(L("SCHED_MODES_INTERVAL"), &triggerMode, 1); ImGui::SameLine();
        ImGui::RadioButton(L("SCHED_MODES_SCHED"), &triggerMode, 2);
        task.triggerMode = static_cast<TaskTrigger>(triggerMode);

        if (task.triggerMode == TaskTrigger::Interval) {
            ImGui::SetNextItemWidth(150);
            ImGui::InputInt(L("INTERVAL_MINUTES"), &task.intervalMinutes);
            if (task.intervalMinutes < 1) task.intervalMinutes = 1;
        }
        else if (task.triggerMode == TaskTrigger::Scheduled) {
            ImGui::Text("At:"); ImGui::SameLine();
            ImGui::SetNextItemWidth(80); ImGui::InputInt(L("SCHED_HOUR"), &task.schedHour);
            ImGui::SameLine(); ImGui::Text(":"); ImGui::SameLine();
            ImGui::SetNextItemWidth(80); ImGui::InputInt(L("SCHED_MINUTE"), &task.schedMinute);
            ImGui::Text("On (Month/Day):"); ImGui::SameLine();
            ImGui::SetNextItemWidth(80); ImGui::InputInt(L("SCHED_MONTH"), &task.schedMonth);
            ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
            ImGui::SetNextItemWidth(80); ImGui::InputInt(L("SCHED_DAY"), &task.schedDay);
            ImGui::SameLine(); ImGui::TextDisabled("(0=Every)");

            task.schedHour = max(0, min(23, task.schedHour));
            task.schedMinute = max(0, min(59, task.schedMinute));
            task.schedMonth = max(0, min(12, task.schedMonth));
            task.schedDay = max(0, min(31, task.schedDay));
        }
    }
}

// 绘制特殊配置的Windows服务设置
static void DrawServiceSettings(SpecialConfig& spCfg) {
    ImGui::SeparatorText(L("SERVICE_MODE_TITLE"));
    
    ImGui::TextWrapped("%s", L("SERVICE_MODE_DESC"));
    ImGui::Spacing();

    ImGui::Checkbox(L("SERVICE_ENABLE"), &spCfg.useServiceMode);
    
    ImGui::BeginDisabled(!spCfg.useServiceMode);

    // 服务名称
    char serviceNameBuf[128];
    strncpy_s(serviceNameBuf, wstring_to_utf8(spCfg.serviceConfig.serviceName).c_str(), sizeof(serviceNameBuf));
    ImGui::SetNextItemWidth(300);
    if (ImGui::InputText(L("SERVICE_NAME"), serviceNameBuf, sizeof(serviceNameBuf))) {
        spCfg.serviceConfig.serviceName = utf8_to_wstring(serviceNameBuf);
    }

    // 服务显示名称
    char displayNameBuf[256];
    strncpy_s(displayNameBuf, wstring_to_utf8(spCfg.serviceConfig.serviceDisplayName).c_str(), sizeof(displayNameBuf));
    ImGui::SetNextItemWidth(300);
    if (ImGui::InputText(L("SERVICE_DISPLAY_NAME"), displayNameBuf, sizeof(displayNameBuf))) {
        spCfg.serviceConfig.serviceDisplayName = utf8_to_wstring(displayNameBuf);
    }

    // 服务选项
    ImGui::Checkbox(L("SERVICE_AUTO_START"), &spCfg.serviceConfig.startWithSystem);
    ImGui::Checkbox(L("SERVICE_DELAYED_START"), &spCfg.serviceConfig.delayedStart);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_SERVICE_DELAYED_START"));

    ImGui::Spacing();

    // 服务操作按钮
#ifdef _WIN32
    bool isInstalled = TaskSystem::IsServiceInstalled(spCfg.serviceConfig.serviceName);
    bool isRunning = isInstalled && TaskSystem::IsServiceRunning(spCfg.serviceConfig.serviceName);

    if (isInstalled) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", L("SERVICE_STATUS_INSTALLED"));
        if (isRunning) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "(%s)", L("SERVICE_STATUS_RUNNING"));
        }
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "%s", L("SERVICE_STATUS_NOT_INSTALLED"));
    }

    ImGui::Spacing();

    if (!isInstalled) {
        if (ImGui::Button(L("SERVICE_INSTALL"))) {
            if (TaskSystem::InstallService(spCfg.serviceConfig)) {
                // 成功
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_SERVICE_INSTALL"));
    }
    else {
        if (ImGui::Button(L("SERVICE_UNINSTALL"))) {
            TaskSystem::UninstallService(spCfg.serviceConfig.serviceName);
        }
        ImGui::SameLine();
        if (!isRunning) {
            if (ImGui::Button(L("SERVICE_START"))) {
                TaskSystem::MineStartService(spCfg.serviceConfig.serviceName);
            }
        }
        else {
            if (ImGui::Button(L("SERVICE_STOP"))) {
                TaskSystem::StopService(spCfg.serviceConfig.serviceName);
            }
        }
    }
#else
    ImGui::TextDisabled("%s", L("SERVICE_NOT_SUPPORTED"));
#endif

    ImGui::EndDisabled();
}

// 绘制特殊配置设置
static void DrawSpecialConfigSettings(SpecialConfig& spCfg) {
    char buf[128];
    strncpy_s(buf, spCfg.name.c_str(), sizeof(buf));
    if (ImGui::InputText(L("CONFIG_NAME"), buf, sizeof(buf))) spCfg.name = buf;

    ImGui::Spacing();

    // 使用子标签页组织特殊配置设置
    if (ImGui::BeginTabBar("SpecialConfigTabs", ImGuiTabBarFlags_None)) {
        // 启动行为标签页
        if (ImGui::BeginTabItem(L("TAB_STARTUP"))) {
            ImGui::Spacing();
            ImGui::Checkbox(L("EXECUTE_ON_STARTUP"), &spCfg.autoExecute);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_EXECUTE_ON_STARTUP"));
            ImGui::Checkbox(L("EXIT_WHEN_FINISHED"), &spCfg.exitAfterExecution);
            
            #ifdef _WIN32
            if (ImGui::Checkbox(L("RUN_ON_WINDOWS_STARTUP"), &spCfg.runOnStartup)) {
                wchar_t selfPath[MAX_PATH];
                GetModuleFileNameW(NULL, selfPath, MAX_PATH);
                SetAutoStart("MineBackup_AutoTask_" + to_string(g_appState.currentConfigIndex), selfPath, true, g_appState.currentConfigIndex, spCfg.runOnStartup);
            }
            #endif
            
            ImGui::Checkbox(L("HIDE_CONSOLE_WINDOW"), &spCfg.hideWindow);

            ImGui::Spacing();
            if (ImGui::Button(L("BUTTON_SWITCH_TO_SP_MODE"))) {
                g_appState.specialConfigs[g_appState.currentConfigIndex].autoExecute = true;
                SaveConfigs();
                ReStartApplication();
                g_appState.done = true;
            }
            ImGui::EndTabItem();
        }

        // 任务管理标签页
        if (ImGui::BeginTabItem(L("TAB_TASKS"))) {
            ImGui::Spacing();
            DrawUnifiedTaskManager(spCfg);
            ImGui::EndTabItem();
        }

        // 服务模式标签页
        if (ImGui::BeginTabItem(L("TAB_SERVICE"))) {
            ImGui::Spacing();
            DrawServiceSettings(spCfg);
            ImGui::EndTabItem();
        }

        // 备份参数覆盖标签页
        if (ImGui::BeginTabItem(L("TAB_BACKUP_OVERRIDES"))) {
            ImGui::Spacing();
            ImGui::Checkbox(L("IS_HOT_BACKUP"), &spCfg.hotBackup);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_HOT_BACKUP"));
            ImGui::Checkbox(L("BACKUP_ON_START"), &spCfg.backupOnGameStart);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_BACKUP_ON_START"));
            ImGui::Checkbox(L("USE_LOW_PRIORITY"), &spCfg.useLowPriority);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_LOW_PRIORITY"));

            int max_threads = thread::hardware_concurrency();
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt(L("CPU_THREAD_COUNT"), &spCfg.cpuThreads, 0, max_threads);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_CPU_THREADS"));

            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt(L("COMPRESSION_LEVEL"), &spCfg.zipLevel, 0, 9);

            ImGui::SetNextItemWidth(150);
            ImGui::InputInt(L("BACKUPS_TO_KEEP"), &spCfg.keepCount);
            ImGui::EndTabItem();
        }

        // 外观标签页
        if (ImGui::BeginTabItem(L("TAB_APPEARANCE"))) {
            ImGui::Spacing();
            const char* theme_names[] = { L("THEME_DARK"), L("THEME_LIGHT"), L("THEME_CLASSIC"), L("THEME_WIN_LIGHT"), L("THEME_WIN_DARK"), L("THEME_NORD_LIGHT"), L("THEME_NORD_DARK"), L("THEME_CUSTOM") };
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo(L("THEME_SETTINGS"), &spCfg.theme, theme_names, IM_ARRAYSIZE(theme_names))) {
                ApplyTheme(spCfg.theme);
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

// 主设置窗口函数 - 新版横向标签页布局
void ShowSettingsWindowV2() {
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    
    if (!ImGui::Begin(L("SETTINGS"), &showSettings, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    // 配置管理（始终显示在顶部）
    DrawConfigManagementPanel();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 根据配置类型显示不同的设置界面
    if (specialSetting) {
        if (!g_appState.specialConfigs.count(g_appState.currentConfigIndex)) {
            specialSetting = false;
            g_appState.currentConfigIndex = g_appState.configs.empty() ? 1 : g_appState.configs.begin()->first;
        }
        else {
            SpecialConfig& spCfg = g_appState.specialConfigs[g_appState.currentConfigIndex];
            DrawSpecialConfigSettings(spCfg);
        }
    }
    else {
        if (!g_appState.configs.count(g_appState.currentConfigIndex)) {
            if (g_appState.configs.empty()) g_appState.configs[1] = Config();
            g_appState.currentConfigIndex = g_appState.configs.begin()->first;
        }
        Config& cfg = g_appState.configs[g_appState.currentConfigIndex];

        // 配置名称
        char nameBuf[128];
        strncpy_s(nameBuf, cfg.name.c_str(), sizeof(nameBuf));
        ImGui::SetNextItemWidth(300);
        if (ImGui::InputText(L("CONFIG_NAME"), nameBuf, sizeof(nameBuf))) cfg.name = nameBuf;

        ImGui::Spacing();

        // 横向标签页
        if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_None)) {
            // 路径设置
            if (ImGui::BeginTabItem(L("TAB_PATHS"))) {
                ImGui::Spacing();
                DrawPathSettings(cfg);
                ImGui::EndTabItem();
            }

            // 世界管理
            if (ImGui::BeginTabItem(L("TAB_WORLDS"))) {
                ImGui::Spacing();
                DrawWorldManagement(cfg);
                ImGui::EndTabItem();
            }

            // 备份行为
            if (ImGui::BeginTabItem(L("TAB_BACKUP"))) {
                ImGui::Spacing();
                DrawBackupBehavior(cfg);
                ImGui::Spacing();
                ImGui::SeparatorText(L("BLACKLIST_HEADER"));
                DrawBlacklistSettings(cfg);
                ImGui::EndTabItem();
            }

            // 还原行为
            if (ImGui::BeginTabItem(L("TAB_RESTORE"))) {
                ImGui::Spacing();
                DrawRestoreBehavior(cfg);
                ImGui::EndTabItem();
            }

            // 云同步
            if (ImGui::BeginTabItem(L("TAB_CLOUD"))) {
                ImGui::Spacing();
                DrawCloudSyncSettings(cfg);
                ImGui::EndTabItem();
            }

            // 外观
            if (ImGui::BeginTabItem(L("TAB_APPEARANCE"))) {
                ImGui::Spacing();
                DrawAppearanceSettings(cfg);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 保存按钮
    if (ImGui::Button(L("BUTTON_SAVE_AND_CLOSE"), ImVec2(150, 30))) {
        SaveConfigs();
        showSettings = false;
    }

    ImGui::End();
}
