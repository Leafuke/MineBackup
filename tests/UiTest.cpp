#include "UIHelpers.h"
#include "SettingsAutoSave.h"
#include "HistoryViewModel.h"
#include "imgui_style.h"
#include "i18n.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
void EnableDarkModeWin(bool) {}
#endif

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
	if (condition) return;
	++failures;
	std::cerr << "FAIL: " << message << '\n';
}

void ApplyBuiltInTheme(int theme) {
	ImGui::GetStyle() = ImGuiStyle();
	switch (theme) {
	case 0: ImGuiTheme::ApplyImGuiDark(); break;
	case 1: ImGuiTheme::ApplyImGuiLight(); break;
	case 2: ImGuiTheme::ApplyImGuiClassic(); break;
	case 3: ImGuiTheme::ApplyWindows11(false); break;
	case 4: ImGuiTheme::ApplyWindows11(true); break;
	case 5: ImGuiTheme::ApplyNord(false); break;
	case 6: ImGuiTheme::ApplyNord(true); break;
	default: break;
	}
	ImGuiTheme::EnsureAccessibleThemeContrast(ImGui::GetStyle());
}

void TestResponsiveLayouts() {
	for (float dpi : {1.0f, 1.5f, 2.0f}) {
		const float em = 16.0f * dpi;
		const auto settingsNarrow = ComputeSettingsResponsiveLayout(em * 41.9f, em);
		const auto settingsWide = ComputeSettingsResponsiveLayout(em * 42.0f, em);
		Check(!settingsNarrow.useSidebar, "settings switches to top navigation below 42em");
		Check(settingsWide.useSidebar, "settings uses sidebar at 42em");
		Check(settingsWide.sidebarWidth >= em * 8.5f, "settings sidebar minimum width");
		Check(settingsWide.contentWidth >= 0.0f, "settings content width is non-negative");

		const auto historyNarrow = ComputeHistoryResponsiveLayout(em * 49.9f, em);
		const auto historyWide = ComputeHistoryResponsiveLayout(em * 50.0f, em);
		Check(!historyNarrow.useSplitView, "history switches to paging below 50em");
		Check(historyWide.useSplitView, "history uses split view at 50em");
		Check(historyWide.listWidth >= em * 22.0f, "history list minimum width");
		Check(historyWide.detailsWidth >= em * 26.0f, "history details minimum width");
	}
	const auto emptySettings = ComputeSettingsResponsiveLayout(-10.0f, 0.0f);
	const auto emptyHistory = ComputeHistoryResponsiveLayout(-10.0f, 0.0f);
	Check(emptySettings.contentWidth >= 0.0f, "settings handles negative available width");
	Check(emptyHistory.listWidth >= 0.0f, "history handles negative available width");
}

void TestScaleMigration() {
	const auto dpiLegacy = MigrateUiScale(1.5f, 1.5f, true);
	Check(dpiLegacy.migrated && dpiLegacy.scale == 1.0f,
		"legacy DPI-derived scale normalizes to 1.0");
	const auto userLegacy = MigrateUiScale(1.25f, 1.5f, true);
	Check(userLegacy.scale == 1.25f, "legacy user multiplier is preserved");
	const auto secondPass = MigrateUiScale(dpiLegacy.scale, 2.0f, false);
	Check(!secondPass.migrated && secondPass.scale == 1.0f,
		"scale migration only runs once");
	Check(MigrateUiScale(9.0f, 1.0f, false).scale == 2.5f,
		"scale clamps to supported maximum");
}

void TestAutoSave() {
	using Clock = SettingsAutoSaveController::Clock;
	const Clock::time_point start{};
	SettingsAutoSaveController controller;
	int saves = 0;
	controller.MarkDirty(start);
	controller.Tick([&] { ++saves; return true; },
		start + std::chrono::milliseconds(499));
	Check(saves == 0 && controller.State() == SettingsSaveState::Pending,
		"autosave waits for the debounce interval");
	controller.Tick([&] { ++saves; return true; },
		start + std::chrono::milliseconds(500));
	Check(saves == 1 && controller.State() == SettingsSaveState::Saved
		&& !controller.IsDirty(), "autosave persists after 500ms");

	controller.MarkDirty(start);
	controller.Tick([&] { ++saves; return false; },
		start + std::chrono::milliseconds(500));
	Check(controller.State() == SettingsSaveState::Failed && controller.IsDirty(),
		"failed save remains dirty");
	Check(controller.Retry([&] { ++saves; return true; })
		&& controller.State() == SettingsSaveState::Saved,
		"failed save can be retried");

	controller.MarkDirty(start);
	Check(controller.Flush([&] { ++saves; return true; }),
		"closing settings flushes pending edits");
}

void TestThemes() {
	for (int theme = 0; theme <= 6; ++theme) {
		ImGuiStyle& style = ImGui::GetStyle();
		style = ImGuiStyle();
		for (ImVec4& color : style.Colors) color = ImVec4(1.0f, 0.0f, 1.0f, 1.0f);
		ApplyBuiltInTheme(theme);
		std::string error;
		const bool validContrast = ImGuiTheme::ValidateTextContrast(style, &error);
		Check(validContrast,
			"theme " + std::to_string(theme) + " text contrast: " + error);
		Check(style.Colors[ImGuiCol_TitleBgActive].x != 1.0f
			|| style.Colors[ImGuiCol_TitleBgActive].z != 1.0f,
			"theme assigns active title color");
		Check(ImGuiTheme::ContrastRatio(style.Colors[ImGuiCol_CheckMark],
			style.Colors[ImGuiCol_CheckboxSelectedBg]) >= 3.0f,
			"theme " + std::to_string(theme) + " selected control contrast");
	}

	ApplyBuiltInTheme(5);
	const ImGuiStyle& nordLight = ImGui::GetStyle();
	Check(ImGuiTheme::ContrastRatio(nordLight.Colors[ImGuiCol_Text],
		nordLight.Colors[ImGuiCol_TitleBgActive]) >= 4.5f,
		"Nord Light title text is readable");
	Check(ImGuiTheme::RelativeLuminance(nordLight.Colors[ImGuiCol_TitleBgActive]) > 0.5f,
		"Nord Light does not inherit a dark title bar");
}

void TestHistoryFiltering() {
	const std::filesystem::path root = std::filesystem::temp_directory_path()
		/ "minebackup-ui-test-history";
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root / "WorldA");

	{
		std::ofstream smallFile(root / "WorldA" / "small.7z", std::ios::binary);
		smallFile << std::string(100, 's');
		std::ofstream normalFile(root / "WorldA" / "normal.7z", std::ios::binary);
		normalFile << std::string(12 * 1024, 'n');
	}

	Config config;
	config.backupPath = root.wstring();
	std::vector<HistoryEntry> entries(4);
	entries[0].worldName = L"WorldA";
	entries[0].backupFile = L"small.7z";
	entries[0].timestamp_str = L"2026-07-28T10:00:00";
	entries[0].comment = L"small note";
	entries[1].worldName = L"WorldA";
	entries[1].backupFile = L"normal.7z";
	entries[1].timestamp_str = L"2026-07-28T11:00:00";
	entries[1].isImportant = true;
	entries[2].worldName = L"WorldB";
	entries[2].backupFile = L"cloud.7z";
	entries[2].timestamp_str = L"2026-07-28T09:00:00";
	entries[2].isCloudArchived = true;
	entries[2].cloudArchiveRemotePath = L"remote:cloud.7z";
	entries[3].worldName = L"WorldB";
	entries[3].backupFile = L"missing.7z";
	entries[3].timestamp_str = L"2026-07-28T08:00:00";

	const auto all = BuildFilteredHistoryViews(config, entries, L"", "",
		HistoryStatusFilter::All, false);
	Check(all.size() == 4 && all.front().entry->backupFile == L"normal.7z",
		"history merges worlds and sorts descending");
	Check(BuildFilteredHistoryViews(config, entries, L"WorldA", "",
		HistoryStatusFilter::All, false).size() == 2, "history world filter");
	Check(BuildFilteredHistoryViews(config, entries, L"", "small note",
		HistoryStatusFilter::All, false).size() == 1, "history text/comment filter");
	Check(BuildFilteredHistoryViews(config, entries, L"", "",
		HistoryStatusFilter::CloudOnly, false).size() == 1, "history cloud-only filter");
	Check(BuildFilteredHistoryViews(config, entries, L"", "",
		HistoryStatusFilter::SmallFile, false).size() == 1, "history small-file filter");
	Check(BuildFilteredHistoryViews(config, entries, L"", "",
		HistoryStatusFilter::All, true).size() == 1, "history important filter");
	Check(HistoryEntryKey{L"WorldA", L"normal.7z"}
		== HistoryEntryKey{L"WorldA", L"normal.7z"}, "history stable selection key");

	std::filesystem::remove_all(root, error);
}

void TestTranslations() {
	SetLanguage("en_US");
	Check(std::string(L("THEME_NORD_DARK")) == "Nord Dark",
		"English Nord Dark name is correct");
	Check(std::string(L("SETTINGS_SAVE_SAVED")) == "Saved",
		"English UI additions are present");
	SetLanguage("zh_CN");
	Check(std::string(L("SETTINGS_SAVE_SAVED")) == "已保存",
		"Chinese UI additions are present");
}

} // namespace

int main() {
	ImGui::CreateContext();
	TestResponsiveLayouts();
	TestScaleMigration();
	TestAutoSave();
	TestThemes();
	TestHistoryFiltering();
	TestTranslations();
	ImGui::DestroyContext();
	if (failures != 0) {
		std::cerr << failures << " UI test(s) failed\n";
		return 1;
	}
	std::cout << "All UI tests passed\n";
	return 0;
}
