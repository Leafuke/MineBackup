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
std::mutex consoleMutex;				// ����̨ģʽ����


string ProcessCommand(const string& commandStr, Console* console) {
	stringstream ss(commandStr);
	string command;
	ss >> command;

	auto error_response = [&](const string& msg) {
		BroadcastEvent(L("KNOTLINK_COMMAND_ERROR") + msg);
		console->AddLog(L("KNOTLINK_COMMAND_ERROR"), command.c_str(), msg.c_str());
		return "ERROR:" + msg;
		};

	// ʹ�� lock_guard ȷ���ں����������ڷ��� g_appState.configs ���̰߳�ȫ��
	lock_guard<mutex> lock(g_appState.configsMutex);

	if (command == "LIST_CONFIGS") {
		string result = "OK:";
		for (const auto& pair : g_appState.configs) {
			result += to_string(pair.first) + "," + pair.second.name + ";";
		}
		if (!result.empty()) result.pop_back(); // �Ƴ�����';'
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
		if (!result.empty()) result.pop_back(); // �Ƴ�����';'

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

		SaveConfigs(); // �������
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
		getline(ss, comment_part); // ��ȡʣ�ಿ����Ϊע��
		if (!comment_part.empty() && comment_part.front() == ' ') comment_part.erase(0, 1); // ȥ��ǰ���ո�

		// �ȹ㲥��Ϣ��֪ͨģ���ȱ�������
		BroadcastEvent("event=backup_started;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first));

		// �ں�̨�߳���ִ�б��ݣ����������������
		thread([=]() {
			// �����߳����ٴμ�������Ϊ g_appState.configs ���������߳��б��޸�
			lock_guard<mutex> thread_lock(g_appState.configsMutex);
			if (g_appState.configs.count(config_idx)) // ȷ��������Ȼ����
				DoBackup(g_appState.configs[config_idx], g_appState.configs[config_idx].worlds[world_idx], *console, utf8_to_wstring(comment_part));
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
	else if (command == "BACKUP_CURRENT") { // ֱ�ӵ��ñ����������е�����ĺ���
		BroadcastEvent("event=pre_hot_backup");
		TriggerHotkeyBackup();
		return "OK:Backup Started";
	}
	else if (command == "AUTO_BACKUP") {
		int config_idx, world_idx, interval_minutes;
		// ��������֤����Ĳ���
		if (!(ss >> config_idx >> world_idx >> interval_minutes) || g_appState.configs.find(config_idx) == g_appState.configs.end() || world_idx < 0 || world_idx >= g_appState.configs[config_idx].worlds.size()) {
			std::string error_msg = "ERROR:Invalid arguments. Usage: AUTO_BACKUP <config_idx> <world_idx> <interval_minutes>";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		// ��֤���ʱ�����Ч��
		if (interval_minutes < 1) {
			std::string error_msg = "ERROR:Interval must be at least 1 minute.";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		const auto& world_name = g_appState.configs[config_idx].worlds[world_idx].first;
		auto taskKey = std::make_pair(config_idx, world_idx);

		// ����Ƿ����������������У������ظ�����
		if (g_active_auto_backups.count(taskKey)) {
			string error_msg = "ERROR:An auto-backup task is already running for this world.";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		// �����������µ��Զ���������
		console->AddLog("[KnotLink] Received command to start auto-backup for world '%s'.", wstring_to_utf8(world_name).c_str());

		// ��ȫ��Map�л�ȡ�򴴽�һ���µ�����ʵ��
		AutoBackupTask& task = g_active_auto_backups[taskKey];
		task.stop_flag = false; // ����ֹͣ���

		// �������̣߳����������б�Ҫ�Ĳ�����
		// ʹ�� std::ref �� stop_flag �����ô��ݸ��̣߳��Ա���Զ�̿�����ֹͣ��
		task.worker = thread(AutoBackupThreadFunction, config_idx, world_idx, interval_minutes, console, ref(task.stop_flag));
		// �����̣߳�ʹ���ں�̨�������У�����ָ��������̷��سɹ���Ϣ��
		task.worker.detach();

		// ����ɹ���Ϣ���㲥�¼�
		std::string success_msg = "OK:Auto-backup started for world '" + wstring_to_utf8(world_name) + "' with an interval of " + std::to_string(interval_minutes) + " minutes.";
		BroadcastEvent("event=auto_backup_started;config=" + std::to_string(config_idx) + ";world=" + wstring_to_utf8(g_appState.configs[config_idx].worlds[world_idx].first) + ";interval=" + std::to_string(interval_minutes));
		console->AddLog("[KnotLink] %s", success_msg.c_str());

		return success_msg;
	}

	else if (command == "STOP_AUTO_BACKUP") {
		int config_idx, world_idx;
		// ��������֤����
		if (!(ss >> config_idx >> world_idx) || g_appState.configs.find(config_idx) == g_appState.configs.end() || world_idx < 0 || world_idx >= g_appState.configs[config_idx].worlds.size()) {
			std::string error_msg = "ERROR:Invalid arguments. Usage: STOP_AUTO_BACKUP <config_idx> <world_idx>";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		const auto& world_name = g_appState.configs[config_idx].worlds[world_idx].first;
		auto taskKey = std::make_pair(config_idx, world_idx);

		// ʹ�û�������������
		std::lock_guard<std::mutex> lock(g_appState.task_mutex);

		// ����ָ��������
		auto it = g_active_auto_backups.find(taskKey);
		if (it == g_active_auto_backups.end()) {
			std::string error_msg = "ERROR:No active auto-backup task found for this world.";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		// ����ֹͣ�źŲ��ȴ��߳̽���
		console->AddLog("[KnotLink] Received command to stop auto-backup for world '%s'.", wstring_to_utf8(world_name).c_str());

		// a. ����ԭ��ֹͣ���Ϊtrue��֪ͨ�߳�Ӧ���˳���
		it->second.stop_flag = true;

		// b. �ȴ��߳�ִ����ϡ���Ϊ�߳̿�������ִ�б��ݻ��������ڣ�
		//    �������ﲻʹ��join()��������AutoBackupThreadFunction�ڲ���ѭ�����⵽stop_flag�������˳���
		//    ��MineBackup�������˳�ʱ����ͳһ��join�߼�ȷ�������̶߳��ѽ�����

		// c. �������б����Ƴ�������
		g_active_auto_backups.erase(it);

		// ����ɹ���Ϣ���㲥�¼�
		std::string success_msg = "OK:Auto-backup task for world '" + wstring_to_utf8(world_name) + "' has been stopped.";
		BroadcastEvent("event=auto_backup_stopped;config=" + std::to_string(config_idx) + ";world_idx=" + std::to_string(world_idx));
		console->AddLog("[KnotLink] %s", success_msg.c_str());

		return success_msg;
	}
	else if (command == "SHUT_DOWN_WORLD_SUCCESS") {
		g_appState.isRespond = true;
		return "OK:Start Restore";
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

	// ����ṩ�� Console ��������־��ӵ��� Items ��
	if (console) {
		console->AddLog("%s", buf);
	}

	// ʼ�մ�ӡ����׼���
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

