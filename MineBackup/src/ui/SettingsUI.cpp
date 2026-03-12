#include "SettingsUI.h"
#include "SettingsUIPrivate.h"

using namespace std;

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
    if (ImGui::Button(L("BUTTON_SAVE_AND_CLOSE"), ImVec2(CalcButtonWidth(L("BUTTON_SAVE_AND_CLOSE")), 0))) {
        SaveConfigs();
        showSettings = false;
    }

    ImGui::End();
}
