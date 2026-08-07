#include "HistoryViewModel.h"
#include "ConfigSelection.h"
#include "DesktopUiLifecycle.h"
#include "WorldListModel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace std;

namespace {
	int failures = 0;

	void Expect(bool condition, const char* message) {
		if (condition) return;
		++failures;
		cerr << "[FAIL] " << message << '\n';
	}

	filesystem::path TemporaryRoot() {
		return filesystem::temp_directory_path()
			/ ("MineBackupUiModelTests-"
				+ to_string(chrono::steady_clock::now().time_since_epoch().count()));
	}

	HistoryEntry Entry(
		wstring world,
		wstring file,
		wstring timestamp,
		bool important = false) {
		HistoryEntry entry;
		entry.worldName = std::move(world);
		entry.backupFile = std::move(file);
		entry.timestamp_str = std::move(timestamp);
		entry.isImportant = important;
		return entry;
	}

	void TestResponsiveLayouts() {
		for (const float em : {16.0f, 20.0f, 32.0f}) {
			Expect(!ComputeHistoryResponsiveLayout(38.0f * em - 0.1f, em).useSplitView,
				"history layout should stay stacked below 38em at every DPI");
			const auto wide = ComputeHistoryResponsiveLayout(38.0f * em, em);
			Expect(wide.useSplitView && wide.listWidth > 0.0f && wide.detailsWidth > 0.0f,
				"history layout should switch to split view exactly at 38em");
			Expect(IsNarrowWorldListLayout(38.0f * em - 0.1f, em)
				&& !IsNarrowWorldListLayout(38.0f * em, em),
				"world list responsive threshold should use em instead of physical pixels");
		}
	}

	void TestDesktopUiLifecycle() {
		using namespace chrono;
		const auto start = DesktopUiLifecycle::Clock::time_point{};
		DesktopUiLifecycle lifecycle;
		Expect(lifecycle.HideToTray(start) == DesktopUiAction::HideWarm
			&& lifecycle.State() == DesktopUiState::HiddenWarm,
			"hiding to tray should enter the warm state");
		Expect(lifecycle.Tick(start + milliseconds(9900)) == DesktopUiAction::None
			&& lifecycle.HasLiveSession(),
			"a tray restore before ten seconds should keep the live session");
		Expect(lifecycle.RequestShow() == DesktopUiAction::ShowExisting
			&& lifecycle.State() == DesktopUiState::Visible,
			"warm restore should reuse the existing session");

		lifecycle.HideToTray(start + seconds(20));
		Expect(lifecycle.Tick(start + seconds(30)) == DesktopUiAction::UnloadSession
			&& lifecycle.State() == DesktopUiState::HiddenCold,
			"ten seconds hidden should unload the UI session");
		Expect(lifecycle.RequestShow() == DesktopUiAction::CreateAndShow,
			"cold restore should request a new UI session");
		lifecycle.CompleteColdRestore(true);
		Expect(lifecycle.State() == DesktopUiState::Visible,
			"a successful cold restore should become visible");

		DesktopUiLifecycle silent(DesktopUiState::HiddenCold);
		Expect(!silent.HasLiveSession()
			&& silent.RequestExit() == DesktopUiAction::None,
			"silent startup and exit should not require a UI session");
		Expect(silent.RequestShow() == DesktopUiAction::CreateAndShow,
			"silent cold startup should create the UI only when activated");
		silent.CompleteColdRestore(false);
		Expect(silent.State() == DesktopUiState::HiddenCold,
			"a failed cold restore should remain in the cold state");
	}

	void TestHistorySnapshots(const filesystem::path& root) {
		Config config;
		config.backupPath = (root / "backups").wstring();
		filesystem::create_directories(root / "backups" / "World");
		ofstream(root / "backups" / "World" / "small.7z", ios::binary)
			<< string(128, 's');
		ofstream(root / "backups" / "World" / "normal.7z", ios::binary)
			<< string(12 * 1024, 'n');

		vector<HistoryEntry> entries{
			Entry(L"World", L"small.7z", L"2026-01-02", true),
			Entry(L"World", L"normal.7z", L"2026-01-03"),
			Entry(L"World", L"missing.7z", L"2026-01-01")};
		entries.back().isCloudArchived = true;
		entries.back().cloudArchiveRemotePath = L"remote:missing.7z";

		HistoryWindowController controller;
		const auto start = chrono::steady_clock::now();
		const auto& views = RefreshHistoryEntryViews(controller, config, entries, start);
		Expect(views.size() == 3
			&& views[0].status == HistoryFileStatus::SmallFile
			&& views[1].status == HistoryFileStatus::Normal
			&& views[2].status == HistoryFileStatus::CloudOnly,
			"history frame snapshots should classify small, normal and cloud-only files");
		const auto& filtered = FilterHistoryEntryViews(
			controller,
			entries,
			views,
			L"World",
			"",
			HistoryStatusFilter::All,
			false);
		Expect(filtered.size() == 3
			&& entries[views[filtered[0]].entryIndex].backupFile == L"normal.7z"
			&& entries[views[filtered[2]].entryIndex].backupFile == L"missing.7z",
			"history filters should sort lightweight indices newest first");
		const HistoryEntryKey stable{L"World", L"normal.7z"};
		Expect(FindHistoryEntryView(views, entries, stable)
				&& FindHistoryEntryView(views, entries, stable)->entryIndex == 1,
			"history selection should resolve through a stable world/file value key");
		const auto& important = FilterHistoryEntryViews(
			controller, entries, views, L"World", "",
			HistoryStatusFilter::All, true);
		Expect(important.size() == 1,
			"history filters should initially include only important entries");
		entries[1].isImportant = true;
		const auto& updatedImportant = FilterHistoryEntryViews(
			controller, entries, views, L"World", "",
			HistoryStatusFilter::All, true);
		Expect(updatedImportant.size() == 2,
			"history filter cache should invalidate when record fields change");

		filesystem::remove(root / "backups" / "World" / "normal.7z");
		const auto& warmViews = RefreshHistoryEntryViews(
			controller, config, entries, start + chrono::milliseconds(900));
		Expect(warmViews[1].status == HistoryFileStatus::Normal,
			"history file status should remain cached for one second");
		const auto& expiredViews = RefreshHistoryEntryViews(
			controller, config, entries, start + chrono::seconds(1));
		Expect(expiredViews[1].status == HistoryFileStatus::Missing,
			"history file status should rescan when its cache expires");

		ofstream(root / "backups" / "World" / "normal.7z", ios::binary)
			<< string(12 * 1024, 'n');
		controller.InvalidateFileStatusCache();
		const auto& invalidatedViews = RefreshHistoryEntryViews(
			controller, config, entries, start + chrono::milliseconds(1100));
		Expect(invalidatedViews[1].status == HistoryFileStatus::Normal,
			"explicit invalidation should bypass the status scan interval");

		entries.push_back(Entry(L"Other", L"gone.7z", L"2026-01-04"));
		const auto& keyChangedViews = RefreshHistoryEntryViews(
			controller, config, entries, start + chrono::milliseconds(1200));
		Expect(keyChangedViews.size() == 4
			&& keyChangedViews[3].status == HistoryFileStatus::Missing,
			"history key changes should rebuild indexed rows immediately");
		const auto& worlds = RefreshHistoryWorlds(controller, entries);
		Expect(worlds.size() == 2 && worlds[0] == L"Other" && worlds[1] == L"World",
			"history world cache should rebuild and sort when record keys change");
		Expect(RemoveUnavailableHistoryEntries(entries, keyChangedViews) == 1
			&& entries.size() == 3 && entries[1].backupFile == L"normal.7z",
			"history deletion should compact records safely from original indices");
		controller.InvalidateFileStatusCache();

		controller.Open(4, L"World", L"Fallback");
		controller.selectedKey = stable;
		controller.textFilter[0] = 'x';
		controller.Close();
		Expect(controller.cachedViews.capacity() == 0
			&& controller.filteredViewIndices.capacity() == 0
			&& controller.cachedWorlds.capacity() == 0,
			"closing history should release row, filter and world cache capacity");
		controller.Open(7, nullopt, L"Fallback");
		Expect(controller.lockedConfigIndex == 7
			&& controller.selectedKey.Empty()
			&& controller.worldFilter == L"Fallback"
			&& controller.textFilter[0] == '\0',
			"reopening the history controller should reset selection and transient filters");
	}

	void TestWorldModels() {
		Config first;
		first.name = "First";
		first.configId = L"11111111-1111-4111-8111-111111111111";
		first.zipLevel = 2;
		first.worlds = {{L"Visible", L"description"}, {L"Hidden", L"#"}};
		Config second;
		second.name = "Second";
		second.configId = L"22222222-2222-4222-8222-222222222222";
		second.worlds = {{L"TaskWorld", L"task"}};
		map<int, Config> configs{{1, first}, {2, second}};

		const auto normal = BuildDisplayWorlds(configs, {}, 1, false);
		Expect(normal.size() == 1
			&& normal[0].baseConfigIndex == 1
			&& normal[0].baseWorldIndex == 0,
			"normal world model should hide marker entries and preserve stable source indices");

		SpecialConfig specialConfig;
		specialConfig.zipLevel = 8;
		specialConfig.keepCount = 4;
		SpecialTask backup;
		backup.type = SpecialTaskType::Backup;
		backup.target.configId = second.configId;
		backup.target.worldPath = L"TaskWorld";
		specialConfig.specialTasks.push_back(backup);
		const auto special = BuildDisplayWorlds(configs, {{9, specialConfig}}, 9, true);
		Expect(special.size() == 1
			&& special[0].name == L"TaskWorld"
			&& special[0].effectiveConfig.zipLevel == 8
			&& special[0].effectiveConfig.keepCount == 4,
			"stable special tasks should reuse the common world construction and override policy");

		const map<int, Config> remappedConfigs{{1, first}, {42, second}};
		const auto remapped = BuildDisplayWorlds(
			remappedConfigs, {{10, specialConfig}}, 10, true);
		Expect(remapped.size() == 1
			&& WorldSelectionKey{remapped[0].baseConfigIndex,
				remapped[0].baseWorldIndex} == WorldSelectionKey{42, 0},
			"special world references should survive numeric config index changes");
	}

	void TestStableConfigSelection() {
		Config normal;
		normal.configId = L"normal-stable-id";
		SpecialConfig special;
		special.specialConfigId = L"special-stable-id";
		const map<int, Config> configs{{42, normal}};
		const map<int, SpecialConfig> specialConfigs{{73, special}};

		Expect(FindConfigByStableId(configs, L"NORMAL-STABLE-ID") == 42,
			"normal startup selection should resolve stable IDs case-insensitively");
		Expect(FindSpecialConfigByStableId(specialConfigs, L"SPECIAL-STABLE-ID") == 73,
			"special startup selection should not treat the map index as persisted identity");
		Expect(FindConfigByStableId(configs, L"missing") == -1
			&& FindSpecialConfigByStableId(specialConfigs, L"missing") == -1,
			"unknown stable IDs should produce an explicit not-found result");
	}
}

int main() {
	const filesystem::path root = TemporaryRoot();
	filesystem::create_directories(root);
	TestResponsiveLayouts();
	TestDesktopUiLifecycle();
	TestHistorySnapshots(root);
	TestWorldModels();
	TestStableConfigSelection();
	error_code ignored;
	filesystem::remove_all(root, ignored);
	if (failures == 0) {
		cout << "[PASS] MineBackup UI model tests\n";
		return 0;
	}
	return 1;
}
