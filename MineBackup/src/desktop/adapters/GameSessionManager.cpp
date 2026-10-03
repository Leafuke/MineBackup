#include "Broadcast.h"
#include "AppPaths.h"
#include "BackupManager.h"
#include "GameSessionManager.h"
#include "FolderRewindFormat.h"
#include "Logging.h"
#include "i18n.h"
#include "Globals.h"
#include "text_to_text.h"
#include "DesktopPlatform.h"
#include "TaskCoordinator.h"
#include "PathIdentity.h"
#include "WorldIdentity.h"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
using namespace std;

#define TASK_INFO(...) MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Task, "game_session.progress", __VA_ARGS__)
#define TASK_WARNING(...) MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Task, "game_session.warning", __VA_ARGS__)


namespace {
	optional<wstring> ResolveLatestManagedBackup(const MyFolder& world) {
		const filesystem::path backupDirectory = JoinPath(world.config.backupPath, world.name);
		error_code ec;
		if (!filesystem::is_directory(backupDirectory, ec) || ec) return nullopt;

		filesystem::path latest;
		filesystem::file_time_type latestTime{};
		for (filesystem::directory_iterator it(
			backupDirectory, filesystem::directory_options::skip_permission_denied, ec), end;
			it != end && !ec; it.increment(ec)) {
			const filesystem::path candidate = it->path();
			if (!it->is_regular_file(ec) || ec) continue;
			const wstring fileName = candidate.filename().wstring();
			if (!FolderRewindFormat::IsSmartBackupType(fileName)
				&& !FolderRewindFormat::IsFullLikeBackupType(fileName)) continue;
			const auto writeTime = it->last_write_time(ec);
			if (!ec && (latest.empty() || writeTime > latestTime)) {
				latest = candidate;
				latestTime = writeTime;
			}
		}
		if (latest.empty()) return nullopt;
		return latest.filename().wstring();
	}

	void ResetHotRestoreState() {
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		g_appState.isRespond = false;
	}
}

bool IsWorldOccupied(const filesystem::path& worldPath) {
	error_code ec;
	if (!filesystem::is_directory(worldPath, ec) || ec) return false;

	for (const filesystem::path& lockCandidate : {
		worldPath / L"session.lock", worldPath / L"level.dat" }) {
		ec.clear();
		if (filesystem::exists(lockCandidate, ec) && !ec
			&& IsFileLocked(lockCandidate.wstring())) return true;
	}

	const filesystem::path dbPath = worldPath / L"db";
	const filesystem::path dbLockPath = dbPath / L"LOCK";
	ec.clear();
	if (filesystem::exists(dbLockPath, ec) && !ec
		&& IsFileLocked(dbLockPath.wstring())) return true;

	ec.clear();
	if (!filesystem::is_directory(dbPath, ec) || ec) return false;
	int scannedFiles = 0;
	for (filesystem::directory_iterator it(
		dbPath, filesystem::directory_options::skip_permission_denied, ec), end;
		it != end && !ec && scannedFiles < 20; it.increment(ec), ++scannedFiles) {
		if (IsFileLocked(it->path().wstring())) return true;
	}
	return false;
}

SessionWorldKey GameSessionWorldKey(const MyFolder& world) {
    return {world.config.configId, PathIdentity::BuildPathIdentityKey(world.path)};
}
vector<MyFolder> EnumerateOccupiedWorlds(const map<int, Config>& configs,
    const function<bool(const filesystem::path&)>& occupied) {
    vector<MyFolder> result;
    for (const auto& [configIndex, config] : configs) {
        if (config.saveRoot.empty() || config.pendingLocalBinding) continue;
        for (size_t i = 0; i < config.worlds.size(); ++i) {
            const auto& [name, description] = config.worlds[i];
            if (description == L"#") continue;
            WorldIdentity::Value identity;
            if (!WorldIdentity::TryBuild(config, name, identity)) continue;
            if (!occupied(identity.sourcePath)) continue;
            result.push_back({identity.sourcePath.wstring(), identity.relativeWorldPath, description,
                config, configIndex, static_cast<int>(i)});
        }
    }
    return result;
}
optional<MyFolder> ResolveSessionWorld(const map<int, Config>& configs, const SessionWorldKey& key) {
    for (const auto& world : EnumerateOccupiedWorlds(configs, [](const auto&) { return true; })) {
        if (GameSessionWorldKey(world) == key) return world;
    }
    return nullopt;
}
GameSessionChanges GameSessionTracker::Poll(const vector<MyFolder>& current) {
    map<SessionWorldKey, MyFolder> next;
    GameSessionChanges changes;
    for (const auto& world : current) next.emplace(GameSessionWorldKey(world), world);
    for (const auto& [key, world] : next) if (!active_.contains(key)) changes.started.push_back(world);
    for (const auto& [key, world] : active_) if (!next.contains(key)) changes.ended.push_back(world);
    active_ = std::move(next);
    return changes;
}
MyFolder GetOccupiedWorld() {
    const auto worlds = EnumerateOccupiedWorlds(SnapshotConfigState().configs, IsWorldOccupied);
    return worlds.empty() ? MyFolder{} : worlds.front();
}
void GameSessionWatcherThread(stop_token stopToken) {
    TASK_INFO(L("LOG_START_WATCHER_START"));
    GameSessionTracker tracker;
    while (!stopToken.stop_requested()) {
        const auto snapshot = SnapshotConfigState();
        const auto changes = tracker.Poll(EnumerateOccupiedWorlds(snapshot.configs, IsWorldOccupied));
        for (const auto& world : changes.started) {
            TASK_INFO(L("LOG_GAME_SESSION_STARTED"), wstring_to_utf8(world.name).c_str());
            BroadcastEvent("event=game_session_start;config=" + to_string(world.configIndex)
                + ";world=" + wstring_to_utf8(world.name));
            if (!world.config.backupOnGameStart) continue;
            const auto key = GameSessionWorldKey(world);
            TaskCoordinator::Instance().Submit(L"game-start-backup",
                {TaskCoordinator::WorldResourceKey(key.first, world.path)}, [key](stop_token token) {
                    const auto target = ResolveSessionWorld(SnapshotConfigState().configs, key);
                    if (!token.stop_requested() && target && target->config.backupOnGameStart)
                        DoBackup(*target, L"OnStart");
                });
        }
        for (const auto& world : changes.ended) {
            TASK_INFO(L("LOG_GAME_SESSION_ENDED"), wstring_to_utf8(world.name).c_str());
            BroadcastEvent("event=game_session_end;config=" + to_string(world.configIndex)
                + ";world=" + wstring_to_utf8(world.name));
            if (!SettingsState().stopAutoBackupOnExit) continue;
            vector<wstring> stopNames;
            {
                lock_guard lock(g_appState.task_mutex);
                for (auto it = g_appState.g_active_auto_backups.begin(); it != g_appState.g_active_auto_backups.end();) {
                    if (it->second.configId == world.config.configId && PathIdentity::PathsEqual(it->second.sourcePath, world.path)) {
                        stopNames.push_back(it->second.taskName); it = g_appState.g_active_auto_backups.erase(it);
                    } else ++it;
                }
            }
            for (const auto& name : stopNames) TaskCoordinator::Instance().RequestStop(name);
        }
        for (int waitStep = 0; waitStep < 100 && !stopToken.stop_requested(); ++waitStep)
            this_thread::sleep_for(chrono::milliseconds(100));
    }
    TASK_INFO(L("LOG_EXIT_WATCHER_STOP"));
}


void TriggerHotkeyBackup(string comment) {
	TASK_INFO(L("LOG_HOTKEY_BACKUP_TRIGGERED"));

	MyFolder world = GetOccupiedWorld();
	if (!world.path.empty()) {
		TASK_INFO(L("LOG_ACTIVE_WORLD_FOUND"), wstring_to_utf8(world.name).c_str(), world.config.name.c_str());

		TaskCoordinator::Instance().Submit(L"hotkey-backup",
			{TaskCoordinator::WorldResourceKey(world.config.configId, world.path)},
			[world, taskComment = utf8_to_wstring(comment)](stop_token) { DoBackup(world, taskComment); });
		return;
	}

	TASK_INFO(L("LOG_NO_ACTIVE_WORLD_FOUND"));
}

void TriggerHotkeyRestore(const string& backupFile) {
	TASK_INFO(L("LOG_HOTKEY_RESTORE_TRIGGERED"));

	MyFolder world = GetOccupiedWorld();
	if (world.path.empty()) {
		TASK_INFO(L("LOG_NO_ACTIVE_WORLD_FOUND"));
		return;
	}

	TASK_INFO(L("LOG_ACTIVE_WORLD_FOUND"), wstring_to_utf8(world.name).c_str(), world.config.name.c_str());
	SubmitUserRestore(world, utf8_to_wstring(backupFile), 0, "", world.config.backupBefore);
}

bool SubmitUserRestore(
	const MyFolder& world,
	const wstring& backupFile,
	int restoreMethod,
	string customRestoreList,
	bool backupBeforeRestore) {
	return TaskCoordinator::Instance().Submit(L"user-restore",
		{TaskCoordinator::WorldResourceKey(world.config.configId, world.path)},
		[world, backupFile, restoreMethod,
			customRestoreList = std::move(customRestoreList), backupBeforeRestore](stop_token) {
			wstring pinnedBackup = backupFile;
			if (pinnedBackup.empty()) {
				const optional<wstring> latest = ResolveLatestManagedBackup(world);
				if (!latest) {
					TASK_WARNING(L("LOG_NO_BACKUP_FOUND"));
					return;
				}
				pinnedBackup = *latest;
			}


   Config restoreConfig=world.config; restoreConfig.backupBefore=backupBeforeRestore;
   const auto verification=PreflightDesktopRestore(restoreConfig,world.name,pinnedBackup,restoreMethod,TaskCoordinator::CurrentStopToken());
   if(!IsSuccessful(verification.code)) {
    for(const auto& diagnostic:verification.diagnostics) TASK_WARNING("%s: %s",diagnostic.eventId.c_str(),diagnostic.detail.c_str());
    return;
   }
   optional<RestoreSafetyBackup> safety;
   bool successfulHotPreBackup=false;
   if(backupBeforeRestore) {
    RestoreSafetyBackup prepared; prepared.request.config=restoreConfig; prepared.request.world={restoreConfig.configId,world.name}; prepared.request.sourcePath=world.path;
    prepared.result=RunDesktopBackup(world,L"BeforeRestore",TaskCoordinator::CurrentStopToken(),BackupExecutionOptions{.deferRetention=true});
    if(!IsSuccessful(prepared.result.code)) {TASK_WARNING(L("KNOTLINK_PRE_RESTORE_BACKUP_FAILED"));return;}
    safety=std::move(prepared); successfulHotPreBackup=true;
   }
   if(!IsWorldOccupied(world.path)) {
    DoRestore(restoreConfig,world.name,pinnedBackup,restoreMethod,customRestoreList,nullptr,"",safety?&*safety:nullptr,&verification);return;
   }

			HotRestoreState expectedIdle = HotRestoreState::IDLE;
			if (!g_appState.hotkeyRestoreState.compare_exchange_strong(
				expectedIdle, HotRestoreState::WAITING_FOR_MOD)) {
				TASK_WARNING(L("KNOTLINK_RESTORE_ALREADY_IN_PROGRESS"));
				return;
			}
			g_appState.isRespond = false;


			const string requestId = wstring_to_utf8(FolderRewindFormat::GenerateGuidString());
			if (successfulHotPreBackup) {
				// The mod resumes autosave on the server thread after receiving
				// the terminal backup event. Give that task a bounded head start.
				this_thread::sleep_for(chrono::milliseconds(250));
			}
			bool modAvailable = PerformModHandshake(
				"restore", wstring_to_utf8(world.name), 3000, requestId);
			if (!modAvailable && successfulHotPreBackup
				&& !(g_appState.knotLinkMod.modDetected.load()
					&& !g_appState.knotLinkMod.versionCompatible.load())) {
				this_thread::sleep_for(chrono::milliseconds(250));
				modAvailable = PerformModHandshake(
					"restore", wstring_to_utf8(world.name), 3000, requestId);
			}
			this_thread::sleep_for(chrono::milliseconds(100));

			if (!modAvailable) {
				if (g_appState.knotLinkMod.modDetected.load()
					&& !g_appState.knotLinkMod.versionCompatible.load()) {
					TASK_WARNING(L("KNOTLINK_RESTORE_MOD_VERSION_INCOMPATIBLE"),
						g_appState.knotLinkMod.modVersion.c_str(),
						KnotLinkModInfo::MIN_MOD_VERSION);
				}
				else {
					TASK_WARNING(L("KNOTLINK_RESTORE_MOD_REQUIRED"));
				}
				ResetHotRestoreState();
				return;
			}

			TASK_INFO(L("KNOTLINK_RESTORE_MOD_OK"),
				g_appState.knotLinkMod.modVersion.c_str());
			MyFolder restoreWorld=world; restoreWorld.config=restoreConfig;
			DoHotRestore(restoreWorld, false, pinnedBackup, restoreMethod, nullptr,
				customRestoreList, requestId, safety?&*safety:nullptr,&verification);
		});
}
