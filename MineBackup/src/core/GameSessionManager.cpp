#include "Broadcast.h"
#include "BackupManager.h"
#include "Logging.h"
#include "i18n.h"
#include "Globals.h"
#include "text_to_text.h"
#include "PlatformCompat.h"
#include "TaskCoordinator.h"
#include <atomic>
#include <filesystem>
#include <mutex>
using namespace std;

#define TASK_INFO(...) MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Task, "game_session.progress", __VA_ARGS__)
#define TASK_WARNING(...) MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Task, "game_session.warning", __VA_ARGS__)
map<pair<int, int>, wstring> g_activeWorlds; // Key: {configIdx, worldIdx}, Value: worldName

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
			wstring levelDatPath = (worldPath / L"session.lock").wstring();
			if (!filesystem::exists(levelDatPath, ec)) { // 没有 session.lock 文件，可能是基岩版存档
				// 基岩版：优先检查 db/LOCK 文件（LevelDB 的标准锁文件）
				filesystem::path dbLockPath = worldPath / L"db" / L"LOCK";
				if (filesystem::exists(dbLockPath, ec) && IsFileLocked(dbLockPath.wstring())) {
					return MyFolder{ worldPath.wstring(), world.first, world.second, cfg, config_idx, world_idx };
				}
				// 回退：遍历db文件夹（限制扫描数量避免性能问题）
				filesystem::path dbPath = worldPath / L"db";
				if (!filesystem::exists(dbPath, ec))
					continue;
				int scannedFiles = 0;
				const int maxScanFiles = 20; // 限制扫描文件数量
				bool foundLocked = false;
				for (const auto& entry : filesystem::directory_iterator(dbPath, filesystem::directory_options::skip_permission_denied, ec)) {
					if (ec) break;
					if (++scannedFiles > maxScanFiles) break;
					const auto entryPathW = entry.path().wstring();
					if (IsFileLocked(entryPathW)) {
						levelDatPath = entryPathW;
						foundLocked = true;
						break;
					}
				}
				if (foundLocked) {
					return MyFolder{ worldPath.wstring(), world.first, world.second, cfg, config_idx, world_idx };
				}
				continue; // 没找到锁定文件
			}
			if (IsFileLocked(levelDatPath)) {
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
			if (!backupOnStart) {
				auto spIt = g_appState.specialConfigs.find(g_appState.currentConfigIndex);
				if (spIt != g_appState.specialConfigs.end()) {
					backupOnStart = spIt->second.backupOnGameStart;
				}
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

	HotRestoreState expected_idle = HotRestoreState::IDLE;
	// 使用CAS操作确保线程安全地从IDLE转换到WAITING_FOR_MOD
	if (!g_appState.hotkeyRestoreState.compare_exchange_strong(expected_idle, HotRestoreState::WAITING_FOR_MOD)) {
		TASK_WARNING(L("KNOTLINK_RESTORE_ALREADY_IN_PROGRESS"));
		return;
	}

	g_appState.isRespond = false;
	TASK_INFO(L("LOG_HOTKEY_RESTORE_TRIGGERED"));

	MyFolder world = GetOccupiedWorld();
	if (world.path.empty()) {
		g_appState.isRespond = false;
		TASK_INFO(L("LOG_NO_ACTIVE_WORLD_FOUND"));
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		return;
	}

	TASK_INFO(L("LOG_ACTIVE_WORLD_FOUND"), wstring_to_utf8(world.name).c_str(), world.config.name.c_str());

	bool modAvailable = PerformModHandshake("restore", wstring_to_utf8(world.name));

	// 握手和下一个广播之间必须有短暂延时
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	if (!modAvailable) {
		if (g_appState.knotLinkMod.modDetected.load() && !g_appState.knotLinkMod.versionCompatible.load()) {
			// 检测到模组但版本不兼容
			TASK_WARNING(L("KNOTLINK_RESTORE_MOD_VERSION_INCOMPATIBLE"),
				g_appState.knotLinkMod.modVersion.c_str(),
				KnotLinkModInfo::MIN_MOD_VERSION);
		}
		else {
			// 没有检测到模组
			TASK_WARNING(L("KNOTLINK_RESTORE_MOD_REQUIRED"));
		}
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		g_appState.isRespond = false;
		return;
	}

	TASK_INFO(L("KNOTLINK_RESTORE_MOD_OK"),
		g_appState.knotLinkMod.modVersion.c_str());

	// 联动模组就绪，在后台线程中执行热还原
	TaskCoordinator::Instance().Submit(L"hotkey-restore",
		{TaskCoordinator::WorldResourceKey(world.config.configId, world.path)}, [world, backupFile](stop_token) {
		DoHotRestore(world, false, utf8_to_wstring(backupFile));
	});
}
