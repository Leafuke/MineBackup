#include "Broadcast.h"
#include "Console.h"
#include "text_to_text.h"
#include "i18n.h"
#include "ConfigManager.h"
#include "BackupManager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
using namespace std;
Console console;
static map<pair<int, int>, AutoBackupTask> g_active_auto_backups; // Key: {configIdx, worldIdx}
std::mutex consoleMutex;				// 控制台模式的锁


string ProcessCommand(const string& commandStr, Console* console) {
	stringstream ss(commandStr);
	string command;
	ss >> command;
	

	auto error_response = [&](const string& msg) {
		BroadcastEvent(L("KNOTLINK_COMMAND_ERROR") + msg);
		console->AddLog(L("KNOTLINK_COMMAND_ERROR"), command.c_str(), msg.c_str());
		return "ERROR:" + msg;
		};

	// 使用 lock_guard 确保在函数作用域内访问 g_appState.configs 是线程安全的
	lock_guard<mutex> lock(g_appState.configsMutex);

	if (command == "LIST_CONFIGS") {
		string result = "OK:";
		for (const auto& pair : g_appState.configs) {
			result += to_string(pair.first) + "," + pair.second.name + ";";
		}
		if (!result.empty()) result.pop_back(); // 移除最后的';'
		BroadcastEvent("event=list_configs;data=" + result);
		return result;
	}
	else if (command == "LIST_BACKUPS") {
		int config_idx, world_idx;
		if (!(ss >> config_idx) || g_appState.configs.find(config_idx) == g_appState.configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return error_response(L("BROADCAST_CONFIG_INDEX_ERROR"));
		}
		if (!(ss >> world_idx) || world_idx >= g_appState.configs[config_idx].worlds.size() || world_idx < 0) {
			BroadcastEvent(L("BROADCAST_WORLD_INDEX_ERROR"));
			return error_response(L("BROADCAST_WORLD_INDEX_ERROR"));
		}
		const auto& cfg = g_appState.configs[config_idx];
		wstring backupDir = cfg.backupPath + L"\\" + cfg.worlds[world_idx].first;
		string result = "OK:";
		if (filesystem::exists(backupDir)) {
			for (const auto& entry : filesystem::directory_iterator(backupDir)) {
				if (entry.is_regular_file()) {
					result += wstring_to_utf8(entry.path().filename().wstring()) + ";";
				}
			}
		}
		if (result.back() == ';') result.pop_back();
		BroadcastEvent("event=list_backups;config=" + to_string(config_idx) + ";world=" + to_string(world_idx) + ";data=" + result);
		return result;
	}
	else if (command == "LIST_WORLDS") {
		int config_idx;
		if (!(ss >> config_idx) || g_appState.configs.find(config_idx) == g_appState.configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return error_response(L("BROADCAST_CONFIG_INDEX_ERROR"));
		}
		const auto& cfg = g_appState.configs[config_idx];
		string result = "OK:";
		for (const auto& world : cfg.worlds) {
			result += wstring_to_utf8(world.first) + ";";
		}
		if (!result.empty()) result.pop_back(); // 移除最后的';'

		BroadcastEvent("event=list_worlds;config=" + to_string(config_idx) + ";data=" + result);
		return result;
	}
	else if (command == "GET_CONFIG") {
		int config_idx;
		if (!(ss >> config_idx) || g_appState.configs.find(config_idx) == g_appState.configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return L("BROADCAST_CONFIG_INDEX_ERROR");
		}
		const auto& cfg = g_appState.configs[config_idx];
		BroadcastEvent("event=get_config;config=" + to_string(config_idx) + ";name=" + cfg.name +
			";backup_mode=" + to_string(cfg.backupMode) + ";hot_backup=" + (cfg.hotBackup ? "true" : "false") +
			";keep_count=" + to_string(cfg.keepCount));
		return "OK:name=" + cfg.name + ";backup_mode=" + to_string(cfg.backupMode) +
			";hot_backup=" + (cfg.hotBackup ? "true" : "false") + ";keep_count=" + to_string(cfg.keepCount);
	}
	else if (command == "SET_CONFIG") {
		int config_idx;
		string key, value;

		if (!(ss >> config_idx >> key >> value) || g_appState.configs.find(config_idx) == g_appState.configs.end()) {
			BroadcastEvent("ERROR:Invalid arguments. Usage: SET_CONFIG <config_idx> <key> <value>");
			return "ERROR:Invalid arguments.Usage : SET_CONFIG <config_idx> <key> <value>";
		}
		auto& cfg = g_appState.configs[config_idx];
		string response_msg = "OK:Set " + key + " to " + value;

		if (key == "backup_mode") cfg.backupMode = stoi(value);
		else if (key == "hot_backup") cfg.hotBackup = (value == "true");
		else return "ERROR:Unknown key '" + key + "'.";

		SaveConfigs(); // 保存更改
		BroadcastEvent("event=config_changed;config=" + to_string(config_idx) + ";key=" + key + ";value=" + value);
		BroadcastEvent(response_msg);
		return response_msg;
	}
	else if (command == "BACKUP") {
		int config_idx, world_idx;
		string comment_part;
		if (!(ss >> config_idx >> world_idx) || g_appState.configs.find(config_idx) == g_appState.configs.end() || world_idx >= g_appState.configs[config_idx].worlds.size()) {
			BroadcastEvent("ERROR:Invalid arguments. Usage: BACKUP <config_idx> <world_idx> [comment]");
			return "ERROR:Invalid arguments. Usage: BACKUP <config_idx> <world_idx> [comment]";
		}
		getline(ss, comment_part); // 获取剩余部分作为注释
		if (!comment_part.empty() && comment_part.front() == ' ') comment_part.erase(0, 1); // 去除前导空格

		MyFolder world = { g_appState.configs[config_idx].saveRoot + L"\\" + g_appState.configs[config_idx].worlds[world_idx].first, g_appState.configs[config_idx].worlds[world_idx].first, g_appState.configs[config_idx].worlds[world_idx].second, g_appState.configs[config_idx], config_idx, world_idx };

		// 先广播消息，通知模组先保存世界
		BroadcastEvent("event=backup_started;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first));

		// 在后台线程中执行备份，避免阻塞命令处理器
		thread([=]() {
			// 在新线程中再次加锁，因为 g_appState.configs 可能在主线程中被修改
			lock_guard<mutex> thread_lock(g_appState.configsMutex);
			if (g_appState.configs.count(config_idx)) // 确保配置仍然存在
				DoBackup(world, *console, utf8_to_wstring(comment_part));
			}).detach();
		return "OK:Backup started for world '" + wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first) + "'";
	}
	else if (command == "RESTORE") {
		int config_idx, world_idx;
		string backup_file;
		if (!(ss >> config_idx) || g_appState.configs.find(config_idx) == g_appState.configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return error_response(L("BROADCAST_CONFIG_INDEX_ERROR"));
		}
		if (!(ss >> world_idx) || world_idx >= g_appState.configs[config_idx].worlds.size() || world_idx < 0) {
			BroadcastEvent(L("BROADCAST_WORLD_INDEX_ERROR"));
			return error_response(L("BROADCAST_WORLD_INDEX_ERROR"));
		}
		if (!(ss >> backup_file)) {
			BroadcastEvent(L("BROADCAST_MISSING_BACKUP_FILE"));
			return error_response(L("BROADCAST_MISSING_BACKUP_FILE"));
		}

		// In a background thread to avoid blocking
		thread([=]() {
			lock_guard<mutex> thread_lock(g_appState.configsMutex);
			if (g_appState.configs.count(config_idx)) {
				// Default to clean restore (method 0) for remote commands for safety
				DoRestore(g_appState.configs[config_idx], g_appState.configs[config_idx].worlds[world_idx].first, utf8_to_wstring(backup_file), *console, 0);
			}
			}).detach();

		console->AddLog(L("KNOTLINK_COMMAND_SUCCESS"), command.c_str());
		BroadcastEvent("event=restore_started;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first));
		return "OK:Restore started for world '" + wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first) + "'";
	}
	else if (command == "BACKUP_MODS") {
		int config_idx;
		string comment_part;
		if (!(ss >> config_idx) || g_appState.configs.find(config_idx) == g_appState.configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return error_response(L("BROADCAST_CONFIG_INDEX_ERROR"));
		}
		getline(ss, comment_part); // Get rest of the line as comment
		if (!comment_part.empty() && comment_part.front() == ' ') comment_part.erase(0, 1);

		thread([=]() {
			lock_guard<mutex> thread_lock(g_appState.configsMutex);
			if (g_appState.configs.count(config_idx)) {
				filesystem::path tempPath = g_appState.configs[config_idx].saveRoot;
				filesystem::path modsPath = tempPath.parent_path() / "mods";
				DoOthersBackup(g_appState.configs[config_idx], modsPath, utf8_to_wstring(comment_part));
			}
			}).detach();
		BroadcastEvent("event=mods_backup_started;config=" + to_string(config_idx));
		console->AddLog(L("KNOTLINK_COMMAND_SUCCESS"), command.c_str());
		return "OK:Mods backup started.";
	}
	else if (command == "BACKUP_CURRENT") { // 直接调用备份正在运行的世界的函数
		BroadcastEvent("event=pre_hot_backup");
		if (ss.rdbuf()->in_avail() > 0) {
			string comment_part;
			getline(ss, comment_part);
			if (!comment_part.empty() && comment_part.front() == ' ') comment_part.erase(0, 1);
			TriggerHotkeyBackup(comment_part);
		}
		else
			TriggerHotkeyBackup();
		return "OK:Backup Started";
	}
	else if (command == "AUTO_BACKUP") {
		int config_idx, world_idx, interval_minutes;
		// 解析并验证传入的参数
		if (!(ss >> config_idx >> world_idx >> interval_minutes) || g_appState.configs.find(config_idx) == g_appState.configs.end() || world_idx < 0 || world_idx >= g_appState.configs[config_idx].worlds.size()) {
			std::string error_msg = "ERROR:Invalid arguments. Usage: AUTO_BACKUP <config_idx> <world_idx> <interval_minutes>";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		// 验证间隔时间的有效性
		if (interval_minutes < 1) {
			std::string error_msg = "ERROR:Interval must be at least 1 minute.";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		const auto& world_name = g_appState.configs[config_idx].worlds[world_idx].first;
		auto taskKey = std::make_pair(config_idx, world_idx);

		// 检查是否已有任务正在运行，避免重复启动
		if (g_active_auto_backups.count(taskKey)) {
			string error_msg = "ERROR:An auto-backup task is already running for this world.";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		// 创建并启动新的自动备份任务
		console->AddLog("[KnotLink] Received command to start auto-backup for world '%s'.", wstring_to_utf8(world_name).c_str());

		// 从全局Map中获取或创建一个新的任务实例
		AutoBackupTask& task = g_active_auto_backups[taskKey];
		task.stop_flag = false; // 重置停止标记

		// 创建新线程，并传入所有必要的参数。
		// 使用 std::ref 将 stop_flag 的引用传递给线程，以便能远程控制其停止。
		task.worker = thread(AutoBackupThreadFunction, config_idx, world_idx, interval_minutes, console, ref(task.stop_flag));
		// 分离线程，使其在后台独立运行，这样指令可以立刻返回成功信息。
		task.worker.detach();

		// 构造成功信息并广播事件
		std::string success_msg = "OK:Auto-backup started for world '" + wstring_to_utf8(world_name) + "' with an interval of " + std::to_string(interval_minutes) + " minutes.";
		BroadcastEvent("event=auto_backup_started;config=" + std::to_string(config_idx) + ";world=" + wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first) + ";interval=" + std::to_string(interval_minutes));
		console->AddLog("[KnotLink] %s", success_msg.c_str());

		return success_msg;
	}

	else if (command == "STOP_AUTO_BACKUP") {
		int config_idx, world_idx;
		// 解析并验证参数
		if (!(ss >> config_idx >> world_idx) || g_appState.configs.find(config_idx) == g_appState.configs.end() || world_idx < 0 || world_idx >= g_appState.configs[config_idx].worlds.size()) {
			std::string error_msg = "ERROR:Invalid arguments. Usage: STOP_AUTO_BACKUP <config_idx> <world_idx>";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		const auto& world_name = g_appState.configs[config_idx].worlds[world_idx].first;
		auto taskKey = std::make_pair(config_idx, world_idx);

		// 使用互斥锁保护访问
		std::lock_guard<std::mutex> lock(g_appState.task_mutex);

		// 查找指定的任务
		auto it = g_active_auto_backups.find(taskKey);
		if (it == g_active_auto_backups.end()) {
			std::string error_msg = "ERROR:No active auto-backup task found for this world.";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		// 发送停止信号并等待线程结束
		console->AddLog("[KnotLink] Received command to stop auto-backup for world '%s'.", wstring_to_utf8(world_name).c_str());

		// a. 设置原子停止标记为true，通知线程应该退出了
		it->second.stop_flag = true;

		// b. 等待线程执行完毕。因为线程可能正在执行备份或处于休眠期，
		//    所以这里不使用join()来阻塞，AutoBackupThreadFunction内部的循环会检测到stop_flag并自行退出。
		//    在MineBackup主程序退出时，有统一的join逻辑确保所有线程都已结束。

		// c. 从任务列表中移除该任务
		g_active_auto_backups.erase(it);

		// 构造成功信息并广播事件
		std::string success_msg = "OK:Auto-backup task for world '" + wstring_to_utf8(world_name) + "' has been stopped.";
		BroadcastEvent("event=auto_backup_stopped;config=" + std::to_string(config_idx) + ";world_idx=" + std::to_string(world_idx));
		console->AddLog("[KnotLink] %s", success_msg.c_str());

		return success_msg;
	}
	else if (command == "SHUTDOWN_WORLD_SUCCESS") {
		if (g_appState.hotkeyRestoreState == HotRestoreState::WAITING_FOR_MOD) {
			g_appState.isRespond = true;
			return "OK:Acknowledged. Restore will now proceed.";
		}
		return "ERROR:Not currently waiting for a world shutdown signal.";
	}
	else if (command == "ADD_TO_WE") {
		int config_idx, world_idx;
		string backup_file;
		if (!(ss >> config_idx) || g_appState.configs.find(config_idx) == g_appState.configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return error_response(L("BROADCAST_CONFIG_INDEX_ERROR"));
		}
		if (!(ss >> world_idx) || world_idx >= g_appState.configs[config_idx].worlds.size() || world_idx < 0) {
			BroadcastEvent(L("BROADCAST_WORLD_INDEX_ERROR"));
			return error_response(L("BROADCAST_WORLD_INDEX_ERROR"));
		}
		if (!(ss >> backup_file)) {
			BroadcastEvent(L("BROADCAST_MISSING_BACKUP_FILE"));
			return error_response(L("BROADCAST_MISSING_BACKUP_FILE"));
		}

		thread([=]() {
			lock_guard<mutex> thread_lock(g_appState.configsMutex);
			if (g_appState.configs.count(config_idx)) {
				AddBackupToWESnapshots(g_appState.configs[config_idx], g_appState.configs[config_idx].worlds[world_idx].first, utf8_to_wstring(backup_file), *console);
			}
			}).detach();

		console->AddLog(L("KNOTLINK_COMMAND_SUCCESS"), command.c_str());
		BroadcastEvent("event=we_snapshot_completed;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first));
		return "OK:Snapshot completed for world '" + wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first) + "'";
	}
	else if (command == "RESTORE_CURRENT_LATEST") {
		TriggerHotkeyRestore();
	}
	else if (command == "SEND") {

		string comment_part;
		getline(ss, comment_part); // 获取剩余部分
		if (!comment_part.empty() && comment_part.front() == ' ') comment_part.erase(0, 1);
		console->AddLog("send: %s", comment_part.c_str());
		BroadcastEvent(comment_part);
		return "OK:Event Sent";
	}

	return "ERROR:Unknown command '" + command + "'.";
}

void ConsoleLog(Console* console, const char* format, ...) {
	lock_guard<mutex> lock(consoleMutex);

	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, IM_ARRAYSIZE(buf), format, args);
	buf[IM_ARRAYSIZE(buf) - 1] = 0;
	va_end(args);

	// 如果提供了 Console 对象，则将日志添加到其 Items 中
	if (console) {
		console->AddLog("%s", buf);
	}

	// 始终打印到标准输出
	printf("%s\n", buf);
}

void  Console::ExecCommand(const char* command_line)
{
	AddLog("# %s\n", command_line);

	HistoryPos = -1;
	for (int i = History.Size - 1; i >= 0; i--)
		if (Stricmp(History[i], command_line) == 0)
		{
			ImGui::MemFree(History[i]);
			History.erase(History.begin() + i);
			break;
		}
	History.push_back(Strdup(command_line));

	// Process command
	if (Stricmp(command_line, "CLEAR") == 0)
	{
		ClearLog();
	}
	else if (Stricmp(command_line, "HELP") == 0)
	{
		AddLog("Commands:");
		for (int i = 0; i < Commands.Size; i++)
			AddLog("- %s", Commands[i]);
	}
	else if (Stricmp(command_line, "HISTORY") == 0)
	{
		int first = History.Size - 10;
		for (int i = first > 0 ? first : 0; i < History.Size; i++)
			AddLog("%3d: %s\n", i, History[i]);
	}
	else
	{
		std::string result = ProcessCommand(command_line, &console);
		AddLog("-> %s", result.c_str());
	}

	// On command input, we scroll to bottom even if AutoScroll==false
	ScrollToBottom = true;
}