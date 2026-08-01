#include "HistoryViewModel.h"
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

		const auto views = BuildHistoryEntryViews(config, entries);
		Expect(views.size() == 3
			&& views[0].status == HistoryFileStatus::SmallFile
			&& views[1].status == HistoryFileStatus::Normal
			&& views[2].status == HistoryFileStatus::CloudOnly,
			"history frame snapshots should classify small, normal and cloud-only files");
		const auto filtered = FilterHistoryEntryViews(
			views,
			L"World",
			"",
			HistoryStatusFilter::All,
			false);
		Expect(filtered.size() == 3
			&& filtered[0].entry.backupFile == L"normal.7z"
			&& filtered[2].entry.backupFile == L"missing.7z",
			"history snapshots should sort newest first without retaining container pointers");
		const HistoryEntryKey stable{L"World", L"normal.7z"};
		Expect(FindHistoryEntryView(views, stable)
				&& FindHistoryEntryView(views, stable)->entry.backupFile == L"normal.7z",
			"history selection should resolve through a stable world/file value key");

		HistoryWindowController controller;
		controller.Open(4, L"World", L"Fallback");
		controller.selectedKey = stable;
		controller.textFilter[0] = 'x';
		controller.Close();
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
		first.zipLevel = 2;
		first.worlds = {{L"Visible", L"description"}, {L"Hidden", L"#"}};
		Config second;
		second.name = "Second";
		second.worlds = {{L"TaskWorld", L"task"}};
		map<int, Config> configs{{1, first}, {2, second}};

		const auto normal = BuildDisplayWorlds(configs, {}, 1, false);
		Expect(normal.size() == 1
			&& normal[0].baseConfigIndex == 1
			&& normal[0].baseWorldIndex == 0,
			"normal world model should hide marker entries and preserve stable source indices");

		SpecialConfig unified;
		unified.zipLevel = 8;
		unified.keepCount = 4;
		UnifiedTaskV2 backup;
		backup.type = TaskTypeV2::Backup;
		backup.configIndex = 2;
		backup.worldIndex = 0;
		unified.unifiedTasks.push_back(backup);
		const auto special = BuildDisplayWorlds(configs, {{9, unified}}, 9, true);
		Expect(special.size() == 1
			&& special[0].name == L"TaskWorld"
			&& special[0].effectiveConfig.zipLevel == 8
			&& special[0].effectiveConfig.keepCount == 4,
			"unified special tasks should reuse the common world construction and override policy");

		SpecialConfig legacy;
		AutomatedTask oldTask;
		oldTask.configIndex = 2;
		oldTask.worldIndex = 0;
		legacy.tasks.push_back(oldTask);
		const auto oldSpecial = BuildDisplayWorlds(configs, {{10, legacy}}, 10, true);
		Expect(oldSpecial.size() == 1
			&& WorldSelectionKey{oldSpecial[0].baseConfigIndex,
				oldSpecial[0].baseWorldIndex} == WorldSelectionKey{2, 0},
			"legacy special tasks should produce the same stable world selection key");
	}
}

int main() {
	const filesystem::path root = TemporaryRoot();
	filesystem::create_directories(root);
	TestResponsiveLayouts();
	TestHistorySnapshots(root);
	TestWorldModels();
	error_code ignored;
	filesystem::remove_all(root, ignored);
	if (failures == 0) {
		cout << "[PASS] MineBackup UI model tests\n";
		return 0;
	}
	return 1;
}
