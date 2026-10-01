#include "BackupManager.h"
#include "AppState.h"
#include "CloudHistoryAnalysis.h"
#include "CloudSyncInternal.h"
#include "GameSessionManager.h"
#include "TaskCoordinator.h"
#include <barrier>
#include "HistoryViewModel.h"
#include "ConfigSelection.h"
#include "DesktopUiLifecycle.h"
#include "SettingsAutoSave.h"
#include "WorldListModel.h"

#include <algorithm>
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

    void TestCloudFailureCompletion() {
        Config config; config.configId = L"cloud-result";
        { lock_guard lock(g_appState.configsMutex); g_appState.configs = {{1, config}}; }
        HistoryEntry entry; entry.configId = config.configId; entry.backupFile = L"failed.7z";
        const auto failed = AggregateCloudDownloads({entry}, CloudSyncMode::HistoryAndBackups, {}, [](const auto&) {
            CloudCommandResult result; result.exitCode = 23; result.message = L"download failed"; return result;
        });
        CloudCommandResult completion; completion.success = failed.success; completion.exitCode = failed.exitCode;
        completion.message = failed.downloadFailures[0].error;
        CloudSyncInternal::CloudOperationScope operation(1, L"syncing");
        operation.Finish(completion);
        const auto snapshot = SnapshotConfigState();
        Expect(snapshot.configs.at(1).cloudLastExitCode == 23 && snapshot.configs.at(1).cloudLastErrorMessage == L"download failed",
            "cloud failure completion persists a nonzero code and failure message for the UI");
        { lock_guard lock(g_appState.cloudTask.mutex);
          Expect(!g_appState.cloudTask.busy && g_appState.cloudTask.lastMessage == L"download failed",
            "cloud operation completion clears busy without replacing failure by success"); }
    }

    void TestGameSessions(const filesystem::path& root) {
        Config a; a.configId = L"a"; a.saveRoot = (root / "a").wstring(); a.backupPath = (root / "backups-a").wstring();
        a.worlds = {{L"world", L""}}; a.backupOnGameStart = false;
        auto b = a; b.configId = L"b"; b.saveRoot = (root / "b").wstring(); b.backupPath = (root / "backups-b").wstring();
        b.backupOnGameStart = true; b.worlds.push_back({L"second", L""});
        map<int, Config> configs{{1, a}, {2, b}};
        GameSessionTracker tracker;
        const auto active = EnumerateOccupiedWorlds(configs, [](const auto&) { return true; });
        const auto first = tracker.Poll(active);
        Expect(first.started.size() == 3 && first.started[0].config.backupOnGameStart == false
            && first.started[1].config.backupOnGameStart && first.started[2].config.backupOnGameStart,
            "every active world uses its owning configuration's start policy");
        Expect(tracker.Poll(active).started.empty(), "repeat polls must not queue another start backup");
        const auto identity = GameSessionWorldKey(active[1]);
        swap(configs[2].worlds[0], configs[2].worlds[1]);
        const auto reordered = EnumerateOccupiedWorlds(configs, [](const auto&) { return true; });
        Expect(tracker.Poll(reordered).started.empty() && ResolveSessionWorld(configs, identity)->worldIndex == 1,
            "world reorder preserves session identity and resolves the new index");
        const auto registered = GameSessionWorldKey(active[1]);
        const auto display = BuildDisplayWorlds(configs, 2);
        auto shown = find_if(display.begin(), display.end(), [](const auto& w){return w.name==L"world";});
        const auto runningName = TaskCoordinator::AutoBackupTaskName(active[1].config.configId, active[1].path);
        {
            lock_guard lock(g_appState.task_mutex);
            g_appState.g_active_auto_backups.clear();
            g_appState.g_active_auto_backups.emplace(registered,AutoBackupTask{runningName,active[1].config.configId,active[1].path});
            Expect(shown != display.end() && g_appState.g_active_auto_backups.contains(DisplayWorldTaskKey(*shown)),
                "reordered world finds its original automatic-backup registry entry");
            Expect(!g_appState.g_active_auto_backups.emplace(DisplayWorldTaskKey(*shown),AutoBackupTask{}).second,
                "world reordering cannot register a duplicate timer");
            const auto other = find_if(display.begin(),display.end(),[](const auto& w){return w.name==L"second";});
            Expect(other != display.end() && !g_appState.g_active_auto_backups.contains(DisplayWorldTaskKey(*other)),
                "another world does not inherit the reordered world's timer state");
            auto timer=g_appState.g_active_auto_backups.find(DisplayWorldTaskKey(*shown));
            Expect(timer->second.taskName==runningName,"stop lookup after reorder resolves the original task name");
            g_appState.g_active_auto_backups.erase(timer);
        }
        Expect(TaskCoordinator::AutoBackupTaskName(active[1].config.configId, active[1].path)!=runningName,
            "restarting a stable world timer still uses a distinct instance name for completion events");
        configs[2].worlds.pop_back();
        Expect(!ResolveSessionWorld(configs, identity), "removed world cannot redirect a queued backup to another world");
        configs.erase(2);
        const auto ended = tracker.Poll(EnumerateOccupiedWorlds(configs, [](const auto&) { return true; }));
        Expect(ended.ended.size() == 2 && !ResolveSessionWorld(configs, identity), "configuration deletion ends its sessions safely");
        tracker.Poll({});
        Expect(tracker.Poll(EnumerateOccupiedWorlds(configs, [](const auto&) { return true; })).started.size() == 1,
            "a subsequent world launch produces exactly one new start transition");
    }

    void TestConfigurationDrafts() {
        Config first; first.configId = L"first"; first.worlds = {{L"old", L""}};
        Config second; second.configId = L"second";
        { lock_guard lock(g_appState.configsMutex); g_appState.configs = {{1, first}, {2, second}}; g_appState.currentConfigIndex = 1; }
        barrier rendezvous(2);
        {
            UiConfigDraft draft;
            UiConfigs().at(1).name = "edited";
            UiConfigs().at(1).worlds = {{L"new", L"description"}};
            jthread writer([&] {
                rendezvous.arrive_and_wait();
                ModifyConfigById(L"first", [](Config& config) { config.cloudLastExitCode = 17; config.cloudLastRunUtc = L"updated"; });
                DeleteConfigById(L"second");
                rendezvous.arrive_and_wait();
            });
            rendezvous.arrive_and_wait();
            rendezvous.arrive_and_wait();
            UiConfigs().at(2).name = "must not reappear";
            draft.Flush();
            const auto snapshot = SnapshotConfigState();
            Expect(snapshot.configs.at(1).name == "edited" && snapshot.configs.at(1).cloudLastExitCode == 17
                && snapshot.configs.at(1).worlds.front().first == L"new", "field patches preserve independent background updates");
            Expect(!snapshot.configs.contains(2), "deleted configurations must never be resurrected by a stale frame");
            ModifyConfigById(L"first", [](Config& config) { config.name = "later"; });
        }
        Expect(SnapshotConfigState().configs.at(1).name == "later", "an unchanged frame flush must not overwrite a later commit");
        const auto stableSnapshot = SnapshotConfigState();
        DeleteConfigById(L"first");
        Expect(stableSnapshot.configs.at(1).worlds.front().first == L"new", "background snapshots own their world list across deletion");
        Expect(!ModifyConfigById(L"first", [](Config& config) { config.name = "invalid"; }), "mutations reject deleted identity");
    }

    void TestResponsiveLayouts() {
        for (const float em : {16.0f, 20.0f, 32.0f, 23.25f}) {
            Expect(!ComputeHistoryResponsiveLayout(38.0f * em, em).useSplitView,
                "38em cannot fit the two history column minima and spacing");
            for (const float gap : {0.0f, em * 0.5f, em * 1.75f}) {
                const float threshold = 40.0f * em + gap;
                Expect(!ComputeHistoryResponsiveLayout(threshold - 0.1f, em, gap).useSplitView,
                    "history remains stacked below its actual column threshold");
                for (float width : {threshold, threshold + 0.1f, threshold * 2.0f}) {
                    const auto wide = ComputeHistoryResponsiveLayout(width, em, gap);
                    Expect(wide.useSplitView && wide.listWidth >= 18.0f * em
                        && wide.detailsWidth >= 22.0f * em - 0.01f
                        && wide.listWidth + wide.detailsWidth <= width - gap + 0.01f,
                        "split column constraints must be legal at every DPI and custom spacing");
                }
            }
            const auto zero = ComputeHistoryResponsiveLayout(0.0f, em);
            Expect(!zero.useSplitView && zero.listWidth == 0.0f && zero.detailsWidth == 0.0f,
                "zero width produces a nonnegative stacked layout");
            Expect(IsNarrowWorldListLayout(38.0f * em - 0.1f, em)
                && !IsNarrowWorldListLayout(38.0f * em, em), "world list retains its independent responsive threshold");
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

	void TestSettingsExternalPersistenceAcknowledgement() {
		SettingsAutoSaveController controller;
		const auto start = SettingsAutoSaveController::Clock::time_point{};
		int saves = 0;
		controller.MarkDirty(start);
		controller.AcknowledgeSaved();
		controller.Tick([&] { ++saves; return true; }, start + chrono::seconds(1));
		Expect(!controller.IsDirty()
				&& controller.State() == SettingsSaveState::Saved
				&& saves == 0,
			"an external batch save should clear pending Settings auto-save work");
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

		const auto normal = BuildDisplayWorlds(configs, 1);
		Expect(normal.size() == 1
			&& normal[0].baseConfigIndex == 1
			&& normal[0].baseWorldIndex == 0,
			"normal world model should hide marker entries and preserve stable source indices");

	}

	void TestStableConfigSelection() {
		Config normal;
		normal.configId = L"normal-stable-id";
		const map<int, Config> configs{{42, normal}};

		Expect(FindConfigByStableId(configs, L"NORMAL-STABLE-ID") == 42,
			"normal startup selection should resolve stable IDs case-insensitively");
		Expect(FindConfigByStableId(configs, L"missing") == -1,
			"unknown stable IDs should produce an explicit not-found result");
	}
}

int main() {
	const filesystem::path root = TemporaryRoot();
	filesystem::create_directories(root);
	Config restoreConfig;
	restoreConfig.configId = L"legacy-restore-target";
	restoreConfig.saveRoot = (root / "saves").wstring();
	restoreConfig.backupPath = (root / "backups").wstring();
	restoreConfig.worlds = {{L"nested/world", L""}};
	const auto unrelated = root / "saves" / "nested_world" / "keep.txt";
	filesystem::create_directories(unrelated.parent_path());
	ofstream(unrelated) << "keep";
	Expect(!DoRestore2(restoreConfig, L"nested_world", root / "external.7z", 0)
		&& !DoRestore(restoreConfig, L"nested_world", L"[Full]-external.7z", 2),
		"legacy external/custom restore must reject an unconfigured storage alias");
	ifstream unchanged(unrelated);
	string content; unchanged >> content;
	Expect(content == "keep" && !filesystem::exists(root / "saves" / "nested/world"),
		"legacy target rejection must leave worlds untouched before workspace preparation");
	unchanged.close();
	TestCloudFailureCompletion();
	TestGameSessions(root);
	TestConfigurationDrafts();
	TestResponsiveLayouts();
	TestDesktopUiLifecycle();
	TestSettingsExternalPersistenceAcknowledgement();
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
