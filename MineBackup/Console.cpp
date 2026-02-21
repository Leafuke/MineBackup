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

	// 使用 unique_lock 确保访问 g_appState.configs 是线程安全的（可在线程分派前提前释放）
	unique_lock<mutex> lock(g_appState.configsMutex);

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
		filesystem::path backupDir = JoinPath(cfg.backupPath, cfg.worlds[world_idx].first);
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

		MyFolder world = { JoinPath(g_appState.configs[config_idx].saveRoot, g_appState.configs[config_idx].worlds[world_idx].first).wstring(), g_appState.configs[config_idx].worlds[world_idx].first, g_appState.configs[config_idx].worlds[world_idx].second, g_appState.configs[config_idx], config_idx, world_idx };

		string worldNameUtf8 = wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first);

		// 先广播消息，通知模组先保存世界
		BroadcastEvent("event=backup_started;config=" + to_string(config_idx) + ";world=" + worldNameUtf8);

		// 释放锁后再启动后台线程，避免新线程与当前线程争用同一互斥锁
		lock.unlock();

		// 在后台线程中执行备份，避免阻塞命令处理器
		thread([=]() {
			lock_guard<mutex> thread_lock(g_appState.configsMutex);
			if (g_appState.configs.count(config_idx)) // 确保配置仍然存在
				DoBackup(world, *console, utf8_to_wstring(comment_part));
			}).detach();
		return "OK:Backup started for world '" + worldNameUtf8 + "'";
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

		string worldNameUtf8 = wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first);

		console->AddLog(L("KNOTLINK_COMMAND_SUCCESS"), command.c_str());
		BroadcastEvent("event=restore_started;config=" + to_string(config_idx) + ";world=" + worldNameUtf8);

		// 释放锁后再启动后台线程
		lock.unlock();

		// In a background thread to avoid blocking
		thread([=]() {
			lock_guard<mutex> thread_lock(g_appState.configsMutex);
			if (g_appState.configs.count(config_idx)) {
				DoRestore(g_appState.configs[config_idx], g_appState.configs[config_idx].worlds[world_idx].first, utf8_to_wstring(backup_file), *console, 0);
			}
			}).detach();

		return "OK:Restore started for world '" + worldNameUtf8 + "'";
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

		BroadcastEvent("event=mods_backup_started;config=" + to_string(config_idx));
		console->AddLog(L("KNOTLINK_COMMAND_SUCCESS"), command.c_str());

		// 释放锁后再启动后台线程
		lock.unlock();

		thread([=]() {
			lock_guard<mutex> thread_lock(g_appState.configsMutex);
			if (g_appState.configs.count(config_idx)) {
				filesystem::path tempPath = g_appState.configs[config_idx].saveRoot;
				filesystem::path modsPath = tempPath.parent_path() / "mods";
				DoOthersBackup(g_appState.configs[config_idx], modsPath, utf8_to_wstring(comment_part));
			}
			}).detach();
		return "OK:Mods backup started.";
	}
	else if (command == "BACKUP_CURRENT") { // 直接调用备份正在运行的世界的函数
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

		// 使用互斥锁保护访问
		std::lock_guard<std::mutex> lock(g_appState.task_mutex);

		// 检查是否已有任务正在运行，避免重复启动
		if (g_appState.g_active_auto_backups.count(taskKey)) {
			string error_msg = "ERROR:An auto-backup task is already running for this world.";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		// 创建并启动新的自动备份任务
		console->AddLog("[KnotLink] Received command to start auto-backup for world '%s'.", wstring_to_utf8(world_name).c_str());

		// 从全局Map中获取或创建一个新的任务实例
		AutoBackupTask& task = g_appState.g_active_auto_backups[taskKey];
		task.stop_flag = false; // 重置停止标记

		// 创建新线程，并传入所有必要的参数。
		// 使用 std::ref 将 stop_flag 的引用传递给线程，以便能远程控制其停止。
		task.worker = thread(AutoBackupThreadFunction, config_idx, world_idx, interval_minutes, console, ref(task.stop_flag));
		// 注意：不再使用 detach()，以便程序退出时可以正确等待线程结束

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

		wstring world_name = g_appState.configs[config_idx].worlds[world_idx].first;
		auto taskKey = std::make_pair(config_idx, world_idx);

		// 释放 configsMutex 后再获取 task_mutex，避免持有 configsMutex 时 join 工作线程导致死锁
		lock.unlock();

		// 使用互斥锁保护访问
		std::lock_guard<std::mutex> task_lock(g_appState.task_mutex);

		// 查找指定的任务
		auto it = g_appState.g_active_auto_backups.find(taskKey);
		if (it == g_appState.g_active_auto_backups.end()) {
			std::string error_msg = "ERROR:No active auto-backup task found for this world.";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		// 发送停止信号并等待线程结束
		console->AddLog("[KnotLink] Received command to stop auto-backup for world '%s'.", wstring_to_utf8(world_name).c_str());

		// a. 设置原子停止标记为true，通知线程应该退出了
		it->second.stop_flag = true;

		// b. 等待线程执行完毕
		if (it->second.worker.joinable())
			it->second.worker.join();

		// c. 从任务列表中移除该任务
		g_appState.g_active_auto_backups.erase(it);

		// 构造成功信息并广播事件
		std::string success_msg = "OK:Auto-backup task for world '" + wstring_to_utf8(world_name) + "' has been stopped.";
		BroadcastEvent("event=auto_backup_stopped;config=" + std::to_string(config_idx) + ";world_idx=" + std::to_string(world_idx));
		console->AddLog("[KnotLink] %s", success_msg.c_str());

		return success_msg;
	}
	else if (command == "SHUTDOWN_WORLD_SUCCESS") {
		// 兼容旧版联动模组: 世界保存并退出完成
		if (g_appState.hotkeyRestoreState == HotRestoreState::WAITING_FOR_MOD) {
			g_appState.isRespond = true;
			// 同时触发新的条件变量机制
			g_appState.knotLinkMod.notifyFlag(&KnotLinkModInfo::worldSaveAndExitComplete);
			return "OK:Acknowledged. Restore will now proceed.";
		}
		return "ERROR:Not currently waiting for a world shutdown signal.";
	}
	else if (command == "HANDSHAKE_RESPONSE") {
		// 联动模组的握手回应: HANDSHAKE_RESPONSE <mod_version>
		// 用于确认模组存在并检查版本兼容性
		string mod_version;
		if (!(ss >> mod_version)) {
			return "ERROR:Missing mod version. Usage: HANDSHAKE_RESPONSE <mod_version>";
		}

		auto& mod = g_appState.knotLinkMod;
		mod.modDetected = true;
		mod.modVersion = mod_version;
		mod.versionCompatible = KnotLinkModInfo::IsVersionCompatible(
			mod_version, KnotLinkModInfo::MIN_MOD_VERSION);

		if (mod.versionCompatible) {
			console->AddLog(L("KNOTLINK_HANDSHAKE_OK"), mod_version.c_str());
		}
		else {
			console->AddLog(L("KNOTLINK_HANDSHAKE_VERSION_MISMATCH"),
				mod_version.c_str(), KnotLinkModInfo::MIN_MOD_VERSION);
		}

		// 唤醒等待握手的线程
		mod.notifyFlag(&KnotLinkModInfo::handshakeReceived);

		BroadcastEvent("event=handshake_ack;status=" +
			string(mod.versionCompatible ? "compatible" : "incompatible") +
			";mod_version=" + mod_version);
		return "OK:Handshake received. Version " + mod_version +
			(mod.versionCompatible.load() ? " (compatible)" : " (incompatible)");
	}
	else if (command == "WORLD_SAVED") {
		// 联动模组通知: 世界已保存完毕 (用于热备份流程)
		// 主程序在发送 pre_hot_backup 后等待此消息，确认世界数据已完整保存
		auto& mod = g_appState.knotLinkMod;
		mod.notifyFlag(&KnotLinkModInfo::worldSaveComplete);
		console->AddLog(L("KNOTLINK_WORLD_SAVED"));
		BroadcastEvent("event=world_save_acknowledged;");
		return "OK:World save acknowledged. Snapshot will be created.";
	}
	else if (command == "WORLD_SAVE_AND_EXIT_COMPLETE") {
		// 联动模组通知: 世界已保存并退出完毕 (用于热还原流程)
		// 与 SHUTDOWN_WORLD_SUCCESS 功能相同，但是新版规范命令名
		if (g_appState.hotkeyRestoreState == HotRestoreState::WAITING_FOR_MOD) {
			g_appState.isRespond = true;
			g_appState.knotLinkMod.notifyFlag(&KnotLinkModInfo::worldSaveAndExitComplete);
			console->AddLog(L("KNOTLINK_WORLD_SAVE_EXIT_COMPLETE"));
			return "OK:Acknowledged. Restore will now proceed.";
		}
		return "ERROR:Not currently waiting for a world save-and-exit signal.";
	}
	else if (command == "REJOIN_RESULT") {
		// 联动模组通知: 重进世界的结果
		// REJOIN_RESULT success 或 REJOIN_RESULT failure [reason]
		string result_str;
		if (!(ss >> result_str)) {
			return "ERROR:Missing result. Usage: REJOIN_RESULT <success|failure> [reason]";
		}

		auto& mod = g_appState.knotLinkMod;
		bool success = (result_str == "success");
		mod.rejoinSuccess = success;
		mod.notifyFlag(&KnotLinkModInfo::rejoinResponseReceived);

		if (success) {
			console->AddLog(L("KNOTLINK_REJOIN_SUCCESS"));
			BroadcastEvent("event=rejoin_acknowledged;status=success;");
		}
		else {
			string reason;
			getline(ss, reason);
			if (!reason.empty() && reason.front() == ' ') reason.erase(0, 1);
			console->AddLog(L("KNOTLINK_REJOIN_FAILED"), reason.c_str());
			BroadcastEvent("event=rejoin_acknowledged;status=failure;reason=" + reason);
		}
		return "OK:Rejoin result received: " + result_str;
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

		string worldNameUtf8 = wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first);
		console->AddLog(L("KNOTLINK_COMMAND_SUCCESS"), command.c_str());
		BroadcastEvent("event=we_snapshot_completed;config=" + to_string(config_idx) + ";world=" + worldNameUtf8);

		// 释放锁后再启动后台线程
		lock.unlock();

		thread([=]() {
			lock_guard<mutex> thread_lock(g_appState.configsMutex);
			if (g_appState.configs.count(config_idx)) {
				AddBackupToWESnapshots(g_appState.configs[config_idx], g_appState.configs[config_idx].worlds[world_idx].first, utf8_to_wstring(backup_file), *console);
			}
			}).detach();

		return "OK:Snapshot completed for world '" + worldNameUtf8 + "'";
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