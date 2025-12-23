#include "Broadcast.h"
#include "BackupManager.h"
#include "Console.h"
#include "text_to_text.h"
#include <atomic>
#include <filesystem>
#include <mutex>
using namespace std;
extern atomic<bool> g_stopExitWatcher;
extern bool g_StopAutoBackupOnExit;
map<pair<int, int>, wstring> g_activeWorlds; // Key: {configIdx, worldIdx}, Value: worldName

bool IsFileLocked(const wstring& path);

MyFolder GetOccupiedWorld() {
	//lock_guard<mutex> lock(g_appState.configsMutex);
	for (const auto& config_pair : g_appState.configs) {
		int config_idx = config_pair.first;
		const Config& cfg = config_pair.second;
		for (int world_idx = 0; world_idx < cfg.worlds.size(); ++world_idx) {
			const auto& world = cfg.worlds[world_idx];
			wstring levelDatPath = cfg.saveRoot + L"\\" + world.first + L"\\session.lock";
			if (!filesystem::exists(levelDatPath)) { // 没有 session.lock 文件，可能是基岩版存档，需要遍历db文件夹下的所有文件看看有没有被锁定的
				wstring temp = cfg.saveRoot + L"\\" + world.first + L"\\db";
				if (!filesystem::exists(temp))
					continue;
				for (const auto& entry : filesystem::directory_iterator(temp)) {
					const auto entryPath = entry.path();
					const auto entryPathW = entryPath.wstring();
					if (IsFileLocked(entryPathW)) {
						levelDatPath = entryPathW;
						break;
					}
				}
			}
			if (IsFileLocked(levelDatPath)) {
				return MyFolder{ cfg.saveRoot + L"\\" + world.first, world.first, world.second, cfg, config_idx, world_idx };
			}
		}
	}
	return MyFolder{};
}

void GameSessionWatcherThread() {
	console.AddLog(L("LOG_START_WATCHER_START"));

	while (!g_stopExitWatcher) {
		map<pair<int, int>, wstring> currently_locked_worlds;

		MyFolder occupied_world = GetOccupiedWorld();

		if (!occupied_world.path.empty()) {
			currently_locked_worlds[{occupied_world.configIndex, occupied_world.worldIndex}] = occupied_world.name;
		}

		vector<pair<int, int>> worlds_to_backup;

		// 检查新启动的世界
		for (const auto& locked_pair : currently_locked_worlds) {
			if (g_activeWorlds.find(locked_pair.first) == g_activeWorlds.end()) {
				console.AddLog(L("LOG_GAME_SESSION_STARTED"), wstring_to_utf8(locked_pair.second).c_str());
				string payload = "event=game_session_start;config=" + to_string(locked_pair.first.first) + ";world=" + wstring_to_utf8(locked_pair.second);
				BroadcastEvent(payload);
				worlds_to_backup.push_back(locked_pair.first);
			}
		}

		for (const auto& active_pair : g_activeWorlds) {
			if (currently_locked_worlds.find(active_pair.first) == currently_locked_worlds.end()) {
				console.AddLog(L("LOG_GAME_SESSION_ENDED"), wstring_to_utf8(active_pair.second).c_str());
				string payload = "event=game_session_end;config=" + to_string(active_pair.first.first) + ";world=" + wstring_to_utf8(active_pair.second);
				BroadcastEvent(payload);

				if (g_StopAutoBackupOnExit) {
					unique_lock<mutex> taskLock(g_appState.task_mutex);
					auto taskIt = g_appState.g_active_auto_backups.find(active_pair.first);
					if (taskIt != g_appState.g_active_auto_backups.end()) {
						taskIt->second.stop_flag = true;
						std::thread worker = std::move(taskIt->second.worker);
						taskLock.unlock();
						if (worker.joinable()) {
							worker.join();
						}
						taskLock.lock();
						g_appState.g_active_auto_backups.erase(active_pair.first);
						taskLock.unlock();
						console.AddLog(L("LOG_AUTOBACKUP_STOPPED_ON_EXIT"), wstring_to_utf8(active_pair.second).c_str());
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
					backupConfig.hotBackup = true; // 必须热备份
					thread backup_thread(DoBackup, occupied_world, ref(console), L"OnStart");
					backup_thread.detach();
				}
			}
		}

		this_thread::sleep_for(chrono::seconds(10));
	}
	console.AddLog(L("LOG_EXIT_WATCHER_STOP"));
}


void TriggerHotkeyBackup(string comment) {
	console.AddLog(L("LOG_HOTKEY_BACKUP_TRIGGERED"));

	MyFolder world = GetOccupiedWorld();
	if (!world.path.empty()) {
		console.AddLog(L("LOG_ACTIVE_WORLD_FOUND"), wstring_to_utf8(world.name).c_str(), world.config.name.c_str());
		console.AddLog(L("KNOTLINK_PRE_HOT_BACKUP"), world.config.name.c_str(), wstring_to_utf8(world.name).c_str());

		world.config.hotBackup = true;

		thread backup_thread(DoBackup, world, ref(console), utf8_to_wstring(comment));
		backup_thread.detach();
		return;
	}

	console.AddLog(L("LOG_NO_ACTIVE_WORLD_FOUND"));
}

void TriggerHotkeyRestore() {

	HotRestoreState expected_idle = HotRestoreState::IDLE;
	// 使用CAS操作确保线程安全地从IDLE转换到WAITING_FOR_MOD
	if (!g_appState.hotkeyRestoreState.compare_exchange_strong(expected_idle, HotRestoreState::WAITING_FOR_MOD)) {
		console.AddLog(L("[Hotkey] A restore operation is already in progress. Ignoring request."));
		return;
	}

	g_appState.isRespond = false;
	console.AddLog(L("LOG_HOTKEY_RESTORE_TRIGGERED"));

	MyFolder world = GetOccupiedWorld();
	if (!world.path.empty()) {
		console.AddLog(L("LOG_ACTIVE_WORLD_FOUND"), wstring_to_utf8(world.name).c_str(), world.config.name.c_str());
		DoHotRestore(world, ref(console), false);
		return;
	}
	
	g_appState.isRespond = false;
	console.AddLog(L("LOG_NO_ACTIVE_WORLD_FOUND"));
	g_appState.hotkeyRestoreState = HotRestoreState::IDLE; // 重置状态
}