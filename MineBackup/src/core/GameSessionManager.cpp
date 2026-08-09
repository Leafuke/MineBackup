#include "Broadcast.h"
#include "BackupManager.h"
#include "GameSessionManager.h"
#include "FolderRewindFormat.h"
#include "Logging.h"
#include "i18n.h"
#include "Globals.h"
#include "text_to_text.h"
#include "PlatformCompat.h"
#include "TaskCoordinator.h"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
using namespace std;

#define TASK_INFO(...) MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Task, "game_session.progress", __VA_ARGS__)
#define TASK_WARNING(...) MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Task, "game_session.warning", __VA_ARGS__)
map<pair<int, int>, wstring> g_activeWorlds; // Key: {configIdx, worldIdx}, Value: worldName

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

MyFolder GetOccupiedWorld() {
	//lock_guard<mutex> lock(g_appState.configsMutex);
	for (const auto& config_pair : g_appState.configs) {
		int config_idx = config_pair.first;
		const Config& cfg = config_pair.second;
		if (cfg.saveRoot.empty()) continue; // 跳过未配置的存档路径
		for (int world_idx = 0; world_idx < (int)cfg.worlds.size(); ++world_idx) {
			const auto& world = cfg.worlds[world_idx];
			filesystem::path worldPath = JoinPath(cfg.saveRoot, world.first);
			error_code ec;
			if (!filesystem::exists(worldPath, ec) || ec) continue; // 跳过不存在的世界
			if (IsWorldOccupied(worldPath)) {
				return MyFolder{ worldPath.wstring(), world.first, world.second, cfg, config_idx, world_idx };
			}
		}
	}
	return MyFolder{};
}

void GameSessionWatcherThread(stop_token stopToken) {
	TASK_INFO(L("LOG_START_WATCHER_START"));

	while (!stopToken.stop_requested()) {
		map<pair<int, int>, wstring> currently_locked_worlds;

		MyFolder occupied_world = GetOccupiedWorld();

		if (!occupied_world.path.empty()) {
			currently_locked_worlds[{occupied_world.configIndex, occupied_world.worldIndex}] = occupied_world.name;
		}

		vector<pair<int, int>> worlds_to_backup;

		// 检查新启动的世界
		for (const auto& locked_pair : currently_locked_worlds) {
			if (g_activeWorlds.find(locked_pair.first) == g_activeWorlds.end()) {
				TASK_INFO(L("LOG_GAME_SESSION_STARTED"), wstring_to_utf8(locked_pair.second).c_str());
				string payload = "event=game_session_start;config=" + to_string(locked_pair.first.first) + ";world=" + wstring_to_utf8(locked_pair.second);
				BroadcastEvent(payload);
				worlds_to_backup.push_back(locked_pair.first);
			}
		}

		for (const auto& active_pair : g_activeWorlds) {
			if (currently_locked_worlds.find(active_pair.first) == currently_locked_worlds.end()) {
				TASK_INFO(L("LOG_GAME_SESSION_ENDED"), wstring_to_utf8(active_pair.second).c_str());
				string payload = "event=game_session_end;config=" + to_string(active_pair.first.first) + ";world=" + wstring_to_utf8(active_pair.second);
				BroadcastEvent(payload);

				if (g_StopAutoBackupOnExit) {
					unique_lock<mutex> taskLock(g_appState.task_mutex);
					auto taskIt = g_appState.g_active_auto_backups.find(active_pair.first);
					if (taskIt != g_appState.g_active_auto_backups.end()) {
						const wstring taskName = taskIt->second.taskName;
						g_appState.g_active_auto_backups.erase(active_pair.first);
						taskLock.unlock();
						TaskCoordinator::Instance().RequestStop(taskName);
						TASK_INFO(L("LOG_AUTOBACKUP_STOPPED_ON_EXIT"), wstring_to_utf8(active_pair.second).c_str());
					}
				}
			}
		}

		// 更新当前活动的世界列表
		g_activeWorlds = currently_locked_worlds;

		bool backupOnStart = false;
		{
			lock_guard<mutex> config_lock(g_appState.configsMutex);
			auto cfgIt = g_appState.configs.find(g_appState.currentConfigIndex);
			if (cfgIt != g_appState.configs.end()) {
				backupOnStart = cfgIt->second.backupOnGameStart;
			}
		}

		if (!worlds_to_backup.empty() && backupOnStart) {
			lock_guard<mutex> config_lock(g_appState.configsMutex);
			for (const auto& backup_target : worlds_to_backup) {
				int config_idx = backup_target.first;
				int world_idx = backup_target.second;
				auto cfgIt = g_appState.configs.find(config_idx);
				if (cfgIt != g_appState.configs.end() && world_idx < cfgIt->second.worlds.size()) {
					Config backupConfig = cfgIt->second;
					MyFolder backupFolder = {
						JoinPath(cfgIt->second.saveRoot, cfgIt->second.worlds[world_idx].first).wstring(),
						cfgIt->second.worlds[world_idx].first,
						cfgIt->second.worlds[world_idx].second,
						backupConfig,
						config_idx,
						world_idx
					};
					TaskCoordinator::Instance().Submit(L"game-start-backup",
						{TaskCoordinator::WorldResourceKey(backupFolder.config.configId, backupFolder.path)},
						[backupFolder](stop_token) { DoBackup(backupFolder, L"OnStart"); });
				}
			}
		}

		for (int waitStep = 0; waitStep < 100 && !stopToken.stop_requested(); ++waitStep) {
			this_thread::sleep_for(chrono::milliseconds(100));
		}
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

			const bool initiallyOccupied = IsWorldOccupied(world.path);
			bool successfulHotPreBackup = false;
			if (!initiallyOccupied) {
				BackupOutcome preBackupOutcome = BackupOutcome::Created;
				if (backupBeforeRestore) {
					preBackupOutcome = DoBackup(world, L"BeforeRestore");
				}
				if (!IsWorldOccupied(world.path)) {
					DoRestore(world.config, world.name, pinnedBackup,
						restoreMethod, customRestoreList);
					return;
				}
				// The world became active while the request was queued or while
				// the pre-restore backup ran. Continue only through hot restore.
				if (backupBeforeRestore
					&& preBackupOutcome != BackupOutcome::Created
					&& preBackupOutcome != BackupOutcome::NoChanges) {
					TASK_WARNING(L("KNOTLINK_PRE_RESTORE_BACKUP_FAILED"));
					return;
				}
			}

			HotRestoreState expectedIdle = HotRestoreState::IDLE;
			if (!g_appState.hotkeyRestoreState.compare_exchange_strong(
				expectedIdle, HotRestoreState::WAITING_FOR_MOD)) {
				TASK_WARNING(L("KNOTLINK_RESTORE_ALREADY_IN_PROGRESS"));
				return;
			}
			g_appState.isRespond = false;

			if (initiallyOccupied && backupBeforeRestore) {
				const BackupOutcome outcome = DoBackup(world, L"BeforeRestore");
				if (outcome != BackupOutcome::Created && outcome != BackupOutcome::NoChanges) {
					TASK_WARNING(L("KNOTLINK_PRE_RESTORE_BACKUP_FAILED"));
					ResetHotRestoreState();
					return;
				}
				successfulHotPreBackup = true;
			}

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
			DoHotRestore(world, false, pinnedBackup, restoreMethod, nullptr,
				customRestoreList, requestId);
		});
}
