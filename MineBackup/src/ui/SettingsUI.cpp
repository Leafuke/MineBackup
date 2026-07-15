#include "SettingsUI.h"
#include "SettingsUIPrivate.h"
#include "MigrationReportUI.h"

using namespace std;

namespace {

const char* CapabilityStateLabel(CapabilityState state) {
    switch (state) {
    case CapabilityState::Available: return "Available";
    case CapabilityState::Unavailable: return "Unavailable";
    case CapabilityState::PermissionRequired: return "Permission required";
    case CapabilityState::Failed: return "Failed";
    }
    return "Unknown";
}

void DrawCapability(const char* name, const CapabilityStatus& status) {
    const ImVec4 color = status.state == CapabilityState::Available
        ? ImVec4(0.35f, 0.8f, 0.45f, 1.0f)
        : status.state == CapabilityState::Failed
            ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
            : ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
    ImGui::BulletText("%s", name);
    ImGui::SameLine();
    ImGui::TextColored(color, "%s", CapabilityStateLabel(status.state));
    if (!status.diagnostic.empty()) {
        ImGui::Indent();
        ImGui::TextWrapped("%s", wstring_to_utf8(status.diagnostic).c_str());
        ImGui::Unindent();
    }
}

void DrawDesktopCapabilitySummary() {
    if (!ImGui::CollapsingHeader("Desktop capabilities")) return;
    const auto capabilities = GetDesktopServices()->Capabilities();
    DrawCapability("File dialogs", capabilities.fileDialogs);
    DrawCapability("Open links and folders", capabilities.openUri);
    DrawCapability("Notifications", capabilities.notifications);
    DrawCapability("System tray", capabilities.tray);
    DrawCapability("Global hotkeys", capabilities.globalHotkeys);
    DrawCapability("Autostart", capabilities.autostart);
    if (capabilities.autostart.state == CapabilityState::PermissionRequired) {
        if (ImGui::Button(L("OPEN_AUTOSTART_SETTINGS"))) {
            const auto result = GetDesktopServices()->OpenAutostartSettings();
            if (!result.IsAvailable() && !result.diagnostic.empty()) {
                MessageBoxWin("MineBackup", wstring_to_utf8(result.diagnostic), 1);
            }
        }
    }
    DrawCapability("Window activation", capabilities.windowActivation);
}

} // namespace

void ShowSettingsWindowV2() {
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    bool canSaveSettings = true;
    
    if (!ImGui::Begin(L("SETTINGS"), &showSettings, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    // 配置管理（始终显示在顶部）
    DrawConfigManagementPanel();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

	MigrationReportUI::DrawSettings();
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

    DrawDesktopCapabilitySummary();
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
        canSaveSettings = IsWEIntegrationPathValidForSave(cfg);

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

            // 模组联动
            if (ImGui::BeginTabItem(L("TAB_MOD_INTEGRATION"))) {
                ImGui::Spacing();
                DrawModIntegrationSettings(cfg);
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

        canSaveSettings = IsWEIntegrationPathValidForSave(cfg);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!canSaveSettings) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", L("ERROR_NON_ASCII_PATH"));
    }

    // 保存按钮
    if (!canSaveSettings) ImGui::BeginDisabled();
    if (ImGui::Button(L("BUTTON_SAVE_AND_CLOSE"), ImVec2(CalcButtonWidth(L("BUTTON_SAVE_AND_CLOSE")), 0))) {
        SaveConfigs();
        showSettings = false;
    }
    if (!canSaveSettings) ImGui::EndDisabled();

    ImGui::End();
}
