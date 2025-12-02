#include "Broadcast.h"
#include "BackupManager.h"
#include "Console.h"
#include "text_to_text.h"
#include <atomic>
using namespace std;
extern atomic<bool> g_stopExitWatcher;
map<pair<int, int>, wstring> g_activeWorlds; // Key: {configIdx, worldIdx}, Value: worldName

bool IsFileLocked(const wstring& path);

void GameSessionWatcherThread() {
	console.AddLog(L("LOG_START_WATCHER_START"));

	while (!g_stopExitWatcher) {
		map<pair<int, int>, wstring> currently_locked_worlds;

		{
			lock_guard<mutex> lock(g_appState.configsMutex);

			for (const auto& config_pair : g_appState.configs) {
				const Config& cfg = config_pair.second;
				if (!cfg.backupOnGameStart) continue;
				for (int world_idx = 0; world_idx < cfg.worlds.size(); ++world_idx) {
					wstring levelDatPath = cfg.saveRoot + L"\\" + cfg.worlds[world_idx].first + L"\\session.lock";
					if (!filesystem::exists(levelDatPath)) { // 没有 session.lock 文件，可能是基岩版存档，需要遍历db文件夹下的所有文件看看有没有被锁定的
						wstring temp = cfg.saveRoot + L"\\" + cfg.worlds[world_idx].first + L"\\db";
						if (!filesystem::exists(temp))
							continue;
						for (const auto& entry : filesystem::directory_iterator(temp)) {
							if (IsFileLocked(entry.path())) {
								levelDatPath = entry.path();
								break;
							}
						}
					}
					if (IsFileLocked(levelDatPath)) {
						currently_locked_worlds[{config_pair.first, world_idx}] = cfg.worlds[world_idx].first;
					}
				}
			}

			for (const auto& sp_config_pair : g_appState.specialConfigs) {
				const SpecialConfig& sp_cfg = sp_config_pair.second;
				if (!sp_cfg.backupOnGameStart) continue;
				for (const auto& task : sp_cfg.tasks) {
					if (g_appState.configs.count(task.configIndex) && task.worldIndex < g_appState.configs[task.configIndex].worlds.size()) {
						const Config& base_cfg = g_appState.configs[task.configIndex];
						const auto& world = base_cfg.worlds[task.worldIndex];
						wstring levelDatPath = base_cfg.saveRoot + L"\\" + world.first + L"\\session.lock";
						if (!filesystem::exists(levelDatPath)) { // 没有 session.lock 文件，可能是基岩版存档，需要遍历db文件夹下的所有文件看看有没有被锁定的
							wstring temp = base_cfg.saveRoot + L"\\" + world.first + L"\\db";
							if (!filesystem::exists(temp))
								continue;
							for (const auto& entry : filesystem::directory_iterator(temp)) {
								if (IsFileLocked(entry.path())) {
									levelDatPath = entry.path();
									break;
								}
							}
						}
						if (IsFileLocked(levelDatPath)) {
							currently_locked_worlds[{task.configIndex, task.worldIndex}] = world.first;
						}
					}
				}
			}
		}

		{
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
				}
			}

			// 更新当前活动的世界列表
			g_activeWorlds = currently_locked_worlds;

			if (!worlds_to_backup.empty() && (g_appState.configs[g_appState.currentConfigIndex].backupOnGameStart || g_appState.specialConfigs[g_appState.currentConfigIndex].backupOnGameStart)) {
				lock_guard<mutex> config_lock(g_appState.configsMutex);
				for (const auto& backup_target : worlds_to_backup) {
					int config_idx = backup_target.first;
					int world_idx = backup_target.second;
					if (g_appState.configs.count(config_idx) && world_idx < g_appState.configs[config_idx].worlds.size()) {
						Config backupConfig = g_appState.configs[config_idx];
						backupConfig.hotBackup = true; // 必须热备份
						thread backup_thread(DoBackup, backupConfig, backupConfig.worlds[world_idx], ref(console), L"OnStart");
						backup_thread.detach();
					}
				}
			}
		}

		this_thread::sleep_for(chrono::seconds(10));
	}
	console.AddLog(L("LOG_EXIT_WATCHER_STOP"));
}


void DoHotRestore(const Config& cfg, const Folder& world, Console& console, bool deleteBackup) {

	// KnotLink 通知
	BroadcastEvent("event=pre_hot_restore;config=" + to_string(world.configIndex) + ";world=" + wstring_to_utf8(world.first));

	// 4. 启动后台等待线程
	thread([=]() {
		using namespace std::chrono;
		auto startTime = steady_clock::now();
		const auto timeout = seconds(15);

		// 等待响应或超时
		while (steady_clock::now() - startTime < timeout) {
			if (g_appState.isRespond) {
				break; // 收到响应
			}
			this_thread::sleep_for(milliseconds(100));
		}

		// 检查是收到了响应还是超时了
		if (!g_appState.isRespond) {
			console.AddLog(L("[Error] Mod did not respond within 15 seconds. Restore aborted."));
			BroadcastEvent("event=restore_cancelled;reason=timeout");
			g_appState.hotkeyRestoreState = HotRestoreState::IDLE; // 重置状态
			return;
		}

		// --- 收到响应，开始还原 ---
		g_appState.isRespond = false; // 重置标志位
		g_appState.hotkeyRestoreState = HotRestoreState::RESTORING;
		console.AddLog(L("[Hotkey] Mod is ready. Starting restore process."));

		// 查找最新备份文件 (这部分逻辑保持不变)
		wstring backupDir = cfg.backupPath + L"\\" + world.first;
		filesystem::path latestBackup;
		auto latest_time = filesystem::file_time_type{};
		bool found = false;

		if (filesystem::exists(backupDir)) {
			for (const auto& entry : filesystem::directory_iterator(backupDir)) {
				if (entry.is_regular_file()) {
					if (entry.last_write_time() > latest_time) {
						latest_time = entry.last_write_time();
						latestBackup = entry.path();
						found = true;
					}
				}
			}
		}

		if (!found) {
			console.AddLog(L("LOG_NO_BACKUP_FOUND"));
			BroadcastEvent("event=restore_finished;status=failure;reason=no_backup_found");
			g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
			return;
		}

		console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(latestBackup.filename().wstring()).c_str());

		DoRestore(cfg, world.first, latestBackup.filename().wstring(), ref(console), 0, "");

		// 假设成功，广播完成事件
		BroadcastEvent("event=restore_finished;status=success;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(world.first));
		console.AddLog(L("[Hotkey] Restore completed successfully."));

		// 最终，重置状态
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		g_appState.isRespond = false;


		}).detach(); // 分离线程，让它在后台运行
}

Folder GetOccupiedWorld() {
	lock_guard<mutex> lock(g_appState.configsMutex);
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
					if (IsFileLocked(entry.path())) {
						levelDatPath = entry.path();
						break;
					}
				}
			}
			if (IsFileLocked(levelDatPath)) {
				return Folder{ cfg.saveRoot + L"\\" + world.first, world.first, world.second, cfg, config_idx, world_idx};
			}
		}
	}
	return Folder{};
}

void TriggerHotkeyBackup() {
	console.AddLog(L("LOG_HOTKEY_BACKUP_TRIGGERED"));

	Folder world = GetOccupiedWorld();
	if (!world.path.empty()) {
		console.AddLog(L("LOG_ACTIVE_WORLD_FOUND"), wstring_to_utf8(world.name).c_str(), world.config.name.c_str());
		console.AddLog(L("KNOTLINK_PRE_HOT_BACKUP"), world.config.name.c_str(), wstring_to_utf8(world.name).c_str());

		world.config.hotBackup = true;

		thread backup_thread(DoBackup, world.config, world, ref(console), L"Hotkey");
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
					if (IsFileLocked(entry.path())) {
						levelDatPath = entry.path();
						break;
					}
				}
			}

			if (IsFileLocked(levelDatPath)) {
				console.AddLog(L("LOG_ACTIVE_WORLD_FOUND"), wstring_to_utf8(world.first).c_str(), cfg.name.c_str());

				DoHotRestore(cfg, world.first, ref(console), false);

				
				return;
			}
		}
	}
	g_appState.isRespond = false;
	console.AddLog(L("LOG_NO_ACTIVE_WORLD_FOUND"));
	g_appState.hotkeyRestoreState = HotRestoreState::IDLE; // 重置状态
}