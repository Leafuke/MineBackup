#include "SettingsUI.h"
#include "SettingsUIPrivate.h"
#include "MigrationReportUI.h"
#include "SettingsAutoSave.h"

using namespace std;

namespace {

enum class SettingsCategory {
	Appearance,
	Integration,
	Migration,
	NormalOverview,
	NormalWorlds,
	NormalBackup,
	NormalRestore,
	NormalCloud,
	NormalWorldEdit,
	SpecialOverview,
	SpecialTasks,
	SpecialBackup,
	SpecialCleanup
};

struct CategoryItem {
	SettingsCategory category;
	const char* labelKey;
};

constexpr CategoryItem kApplicationCategories[] = {
	{SettingsCategory::Appearance, "SETTINGS_CATEGORY_APPEARANCE"},
	{SettingsCategory::Integration, "SETTINGS_CATEGORY_INTEGRATION"},
	{SettingsCategory::Migration, "SETTINGS_CATEGORY_MIGRATION"}
};
constexpr CategoryItem kNormalCategories[] = {
	{SettingsCategory::NormalOverview, "SETTINGS_CATEGORY_NORMAL_OVERVIEW"},
	{SettingsCategory::NormalWorlds, "SETTINGS_CATEGORY_NORMAL_WORLDS"},
	{SettingsCategory::NormalBackup, "SETTINGS_CATEGORY_NORMAL_BACKUP"},
	{SettingsCategory::NormalRestore, "SETTINGS_CATEGORY_NORMAL_RESTORE"},
	{SettingsCategory::NormalCloud, "SETTINGS_CATEGORY_NORMAL_CLOUD"},
	{SettingsCategory::NormalWorldEdit, "SETTINGS_CATEGORY_NORMAL_WORLDEDIT"}
};
constexpr CategoryItem kSpecialCategories[] = {
	{SettingsCategory::SpecialOverview, "SETTINGS_CATEGORY_SPECIAL_OVERVIEW"},
	{SettingsCategory::SpecialTasks, "SETTINGS_CATEGORY_SPECIAL_TASKS"},
	{SettingsCategory::SpecialBackup, "SETTINGS_CATEGORY_SPECIAL_BACKUP"},
	{SettingsCategory::SpecialCleanup, "SETTINGS_CATEGORY_SPECIAL_CLEANUP"}
};

SettingsAutoSaveController g_settingsAutoSave;
SettingsCategory g_selectedCategory = SettingsCategory::Appearance;

const char* CapabilityStateLabel(CapabilityState state) {
	switch (state) {
	case CapabilityState::Available: return L("CAP_STATE_AVAILABLE");
	case CapabilityState::Unavailable: return L("CAP_STATE_UNAVAILABLE");
	case CapabilityState::PermissionRequired: return L("CAP_STATE_PERMISSION_REQUIRED");
	case CapabilityState::Failed: return L("CAP_STATE_FAILED");
	}
	return L("CAP_STATE_UNKNOWN");
}

const char* CapabilityDetail(CapabilityState state) {
	switch (state) {
	case CapabilityState::Unavailable: return L("CAP_DETAIL_UNAVAILABLE");
	case CapabilityState::PermissionRequired: return L("CAP_DETAIL_PERMISSION_REQUIRED");
	case CapabilityState::Failed: return L("CAP_DETAIL_FAILED");
	case CapabilityState::Available: return nullptr;
	}
	return nullptr;
}

void DrawCapability(const char* nameKey, const CapabilityStatus& status) {
	const ImVec4 color = status.state == CapabilityState::Available
		? ImVec4(0.35f, 0.8f, 0.45f, 1.0f)
		: status.state == CapabilityState::Failed
			? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
			: ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
	ImGui::BulletText("%s", L(nameKey));
	ImGui::SameLine();
	ImGui::TextColored(color, "%s", CapabilityStateLabel(status.state));
	if (const char* detail = CapabilityDetail(status.state)) {
		ImGui::Indent();
		ImGui::TextWrapped("%s", detail);
		ImGui::Unindent();
	}
}

void DrawDesktopCapabilitySummary() {
	const auto capabilities = GetDesktopServices()->Capabilities();
	DrawCapability("CAP_FILE_DIALOGS", capabilities.fileDialogs);
	DrawCapability("CAP_OPEN_URI", capabilities.openUri);
	DrawCapability("CAP_NOTIFICATIONS", capabilities.notifications);
	DrawCapability("CAP_TRAY", capabilities.tray);
	DrawCapability("CAP_HOTKEYS", capabilities.globalHotkeys);
	DrawCapability("CAP_AUTOSTART", capabilities.autostart);
	if (capabilities.autostart.state == CapabilityState::PermissionRequired
		&& ImGui::Button(L("OPEN_AUTOSTART_SETTINGS"))) {
		const auto result = GetDesktopServices()->OpenAutostartSettings();
		if (!result.IsAvailable() && !result.diagnostic.empty()) {
			MessageBoxWin("MineBackup", L("AUTOSTART_SETTINGS_OPEN_FAILED"), 1);
		}
	}
	DrawCapability("CAP_WINDOW_ACTIVATION", capabilities.windowActivation);
}

bool IsNormalCategory(SettingsCategory category) {
	return category >= SettingsCategory::NormalOverview
		&& category <= SettingsCategory::NormalWorldEdit;
}

bool IsSpecialCategory(SettingsCategory category) {
	return category >= SettingsCategory::SpecialOverview
		&& category <= SettingsCategory::SpecialCleanup;
}

void NormalizeSelectedCategory() {
	if (specialSetting && IsNormalCategory(g_selectedCategory)) {
		g_selectedCategory = SettingsCategory::SpecialOverview;
	}
	if (!specialSetting && IsSpecialCategory(g_selectedCategory)) {
		g_selectedCategory = SettingsCategory::NormalOverview;
	}
}

void DrawCategoryItem(const CategoryItem& item) {
	const bool selected = g_selectedCategory == item.category;
	if (ImGui::Selectable(L(item.labelKey), selected, 0,
		ImVec2(0.0f, GetUiMetrics().minButtonHeight))) {
		g_selectedCategory = item.category;
	}
}

void DrawSidebarSection(const char* titleKey, const CategoryItem* items, size_t count) {
	ImGui::TextDisabled("%s", L(titleKey));
	for (size_t index = 0; index < count; ++index) DrawCategoryItem(items[index]);
	ImGui::Spacing();
}

void DrawSidebar() {
	DrawSidebarSection("SETTINGS_GROUP_APPLICATION", kApplicationCategories,
		IM_ARRAYSIZE(kApplicationCategories));
	DrawSidebarSection(specialSetting ? "SETTINGS_GROUP_SPECIAL" : "SETTINGS_GROUP_NORMAL",
		specialSetting ? kSpecialCategories : kNormalCategories,
		specialSetting ? IM_ARRAYSIZE(kSpecialCategories) : IM_ARRAYSIZE(kNormalCategories));
}

void DrawCategoryCombo() {
	const CategoryItem* selectedItem = &kApplicationCategories[0];
	auto findSelected = [&](const CategoryItem* items, size_t count) {
		for (size_t index = 0; index < count; ++index) {
			if (items[index].category == g_selectedCategory) selectedItem = &items[index];
		}
	};
	findSelected(kApplicationCategories, IM_ARRAYSIZE(kApplicationCategories));
	if (specialSetting) findSelected(kSpecialCategories, IM_ARRAYSIZE(kSpecialCategories));
	else findSelected(kNormalCategories, IM_ARRAYSIZE(kNormalCategories));

	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::BeginCombo("##SettingsCategory", L(selectedItem->labelKey))) {
		ImGui::TextDisabled("%s", L("SETTINGS_GROUP_APPLICATION"));
		for (const auto& item : kApplicationCategories) DrawCategoryItem(item);
		ImGui::Separator();
		ImGui::TextDisabled("%s", L(specialSetting
			? "SETTINGS_GROUP_SPECIAL" : "SETTINGS_GROUP_NORMAL"));
		if (specialSetting) {
			for (const auto& item : kSpecialCategories) DrawCategoryItem(item);
		}
		else {
			for (const auto& item : kNormalCategories) DrawCategoryItem(item);
		}
		ImGui::EndCombo();
	}
}

void DrawSaveStatus() {
	const char* key = nullptr;
	ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	switch (g_settingsAutoSave.State()) {
	case SettingsSaveState::Pending: key = "SETTINGS_SAVE_PENDING"; break;
	case SettingsSaveState::Saving: key = "SETTINGS_SAVE_SAVING"; break;
	case SettingsSaveState::Saved:
		key = "SETTINGS_SAVE_SAVED";
		color = ImVec4(0.35f, 0.80f, 0.45f, 1.0f);
		break;
	case SettingsSaveState::Failed:
		key = "SETTINGS_SAVE_FAILED";
		color = ImVec4(1.0f, 0.40f, 0.35f, 1.0f);
		break;
	case SettingsSaveState::Idle: break;
	}
	if (!key) return;
	ImGui::TextColored(color, "%s", L(key));
	if (g_settingsAutoSave.State() == SettingsSaveState::Failed) {
		ImGui::SameLine();
		if (ImGui::SmallButton(L("BUTTON_RETRY"))) {
			g_settingsAutoSave.Retry([] { return SaveConfigs(); });
		}
	}
}

void DrawRestartBanner() {
	if (!g_restartRequired || g_restartBannerDismissed) return;
	BeginUiCard("##RestartRequired");
	ImGui::TextWrapped("%s", L("SETTINGS_RESTART_REQUIRED"));
	if (ImGui::Button(L("SETTINGS_RESTART_NOW"))) {
		g_settingsAutoSave.Flush([] { return SaveConfigs(); });
		if (GetDesktopServices()->RestartApplication().IsAvailable()) g_appState.done = true;
	}
	ImGui::SameLine();
	if (ImGui::Button(L("SETTINGS_RESTART_LATER"))) {
		g_restartBannerDismissed = true;
	}
	EndUiCard();
	ImGui::Spacing();
}

void DrawNormalCategory(Config& config) {
	switch (g_selectedCategory) {
	case SettingsCategory::NormalOverview: {
		char name[128];
		strncpy_s(name, config.name.c_str(), sizeof(name));
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText(L("CONFIG_NAME"), name, sizeof(name))) config.name = name;
		ImGui::Spacing();
		DrawPathSettings(config);
		break;
	}
	case SettingsCategory::NormalWorlds:
		DrawWorldManagement(config);
		break;
	case SettingsCategory::NormalBackup:
		DrawBackupBehavior(config);
		ImGui::Spacing();
		ImGui::SeparatorText(L("BLACKLIST_HEADER"));
		DrawBlacklistSettings(config);
		break;
	case SettingsCategory::NormalRestore:
		DrawRestoreBehavior(config);
		break;
	case SettingsCategory::NormalCloud:
		DrawCloudSyncSettings(config);
		break;
	case SettingsCategory::NormalWorldEdit:
		DrawWorldEditSettings(config);
		break;
	default:
		break;
	}
}

void DrawSelectedContent(Config& normalConfig) {
	BeginUiCard("##SettingsCard");
	switch (g_selectedCategory) {
	case SettingsCategory::Appearance:
		DrawAppearanceSettings(normalConfig);
		break;
	case SettingsCategory::Integration:
		ImGui::SeparatorText(L("SETTINGS_CATEGORY_INTEGRATION"));
		DrawSystemIntegrationSettings();
		ImGui::Spacing();
		ImGui::SeparatorText(L("CAPABILITIES_HEADER"));
		DrawDesktopCapabilitySummary();
		break;
	case SettingsCategory::Migration:
		MigrationReportUI::DrawSettings();
		break;
	default:
		if (specialSetting) {
			SpecialConfig& special = g_appState.specialConfigs.at(g_appState.currentConfigIndex);
			SpecialSettingsPage page = SpecialSettingsPage::Overview;
			if (g_selectedCategory == SettingsCategory::SpecialTasks) {
				page = SpecialSettingsPage::Tasks;
			}
			else if (g_selectedCategory == SettingsCategory::SpecialBackup) {
				page = SpecialSettingsPage::Backup;
			}
			else if (g_selectedCategory == SettingsCategory::SpecialCleanup) {
				page = SpecialSettingsPage::LegacyCleanup;
			}
			DrawSpecialConfigSettings(special, page);
		}
		else {
			DrawNormalCategory(normalConfig);
		}
		break;
	}
	EndUiCard();
}

bool CanSaveSettings() {
	for (const auto& [index, config] : g_appState.configs) {
		(void)index;
		if (!IsWEIntegrationPathValidForSave(config)) return false;
	}
	return true;
}

} // namespace

void ShowSettingsWindowV2() {
	if (g_appState.configs.empty()) {
		const int index = CreateNewNormalConfig();
		g_appState.currentConfigIndex = index;
		specialSetting = false;
	}
	if (!specialSetting && !g_appState.configs.contains(g_appState.currentConfigIndex)) {
		g_appState.currentConfigIndex = g_appState.configs.begin()->first;
	}
	if (specialSetting && !g_appState.specialConfigs.contains(g_appState.currentConfigIndex)) {
		specialSetting = false;
		g_appState.currentConfigIndex = g_appState.configs.begin()->first;
	}

	const UiMetrics metrics = GetUiMetrics();
	SetNextWindowSizeFromMetrics(metrics, 56.0f, 38.0f);
	SetNextWindowConstraintsFromMetrics(metrics, 30.0f, 22.0f);
	const bool visible = ImGui::Begin(L("SETTINGS"), &showSettings, ImGuiWindowFlags_NoDocking);
	if (!visible) {
		ImGui::End();
		if (!showSettings) g_settingsAutoSave.Flush([] { return SaveConfigs(); });
		return;
	}

	ImGui::TextUnformatted(L("SETTINGS"));
	ImGui::SameLine();
	const float statusWidth = ImGui::CalcTextSize(L("SETTINGS_SAVE_FAILED")).x
		+ metrics.Em(5.0f);
	if (ImGui::GetContentRegionAvail().x > statusWidth) {
		ImGui::SetCursorPosX(ImGui::GetCursorPosX()
			+ (std::max)(ImGui::GetContentRegionAvail().x - statusWidth, 0.0f));
		DrawSaveStatus();
	}
	DrawRestartBanner();
	DrawConfigManagementPanel();
	NormalizeSelectedCategory();
	ImGui::Spacing();

	const SettingsResponsiveLayout layout = ComputeSettingsResponsiveLayout(
		ImGui::GetContentRegionAvail().x, metrics.em, metrics.spacingX);
	if (!layout.useSidebar) {
		DrawCategoryCombo();
		ImGui::Spacing();
	}

	if (layout.useSidebar) {
		ImGui::BeginChild("##SettingsSidebar", ImVec2(layout.sidebarWidth, 0.0f),
			ImGuiChildFlags_Borders);
		DrawSidebar();
		ImGui::EndChild();
		ImGui::SameLine();
	}

	ImGui::BeginChild("##SettingsContent", ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
	Config* normalConfig = &g_appState.configs.begin()->second;
	if (!specialSetting) normalConfig = &g_appState.configs.at(g_appState.currentConfigIndex);
	DrawSelectedContent(*normalConfig);
	ImGui::EndChild();

	ImGuiContext* context = ImGui::GetCurrentContext();
	const bool settingsClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
		&& ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
	if ((context && context->AnyIdHasBeenEditedThisFrame) || settingsClicked) {
		g_settingsAutoSave.MarkDirty();
	}
	g_settingsAutoSave.Tick([] {
		return CanSaveSettings() && SaveConfigs();
	});

	ImGui::End();
	if (!showSettings) {
		g_settingsAutoSave.Flush([] {
			return CanSaveSettings() && SaveConfigs();
		});
	}
}
