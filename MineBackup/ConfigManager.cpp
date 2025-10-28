#include "ConfigManager.h"
#include "AppState.h"
#include "text_to_text.h"
#include "i18n.h"
#ifdef _WIN32
#include "Platform_win.h"
#else
#include "Platform_linux.h"
#endif
#include <filesystem>
#include <fstream>
#include <sstream>
using namespace std;

static int nextConfigId = 2; // 从 2 开始，因为 1 被向导占用

extern bool g_RunOnStartup;
extern bool g_enableKnotLink;
extern bool g_CheckForUpdates;
extern bool isSilence;
extern bool isSafeDelete;
extern wstring Fontss;
extern vector<wstring> restoreWhitelist;
extern int last_interval;

bool checkWorldName(const wstring& world, const vector<pair<wstring, wstring>>& worldList);
bool IsFileLocked(const wstring& path);

int CreateNewSpecialConfig(const string& name_hint) {
	int newId = nextConfigId++;
	SpecialConfig sp;
	sp.name = name_hint;
	g_appState.specialConfigs[newId] = sp;
	return newId;
}

int CreateNewNormalConfig(const string& name_hint) {
	int newId = nextConfigId++;
	Config new_cfg;
	new_cfg.name = name_hint;
	// 默认空的路径/世界
	new_cfg.saveRoot.clear();
	new_cfg.backupPath.clear();
	new_cfg.worlds.clear();
	// 其他默认值可在此设置
	g_appState.configs[newId] = new_cfg;
	return newId;
}

void LoadConfigs(const string& filename) {
	lock_guard<mutex> lock(g_appState.configsMutex);
	g_appState.configs.clear();
	g_appState.specialConfigs.clear();
	ifstream in(filename, ios::binary);
	if (!in.is_open()) return;
	string line1;
	wstring line, section;
	// cur作为一个指针，指向 g_appState.configs 这个全局 map<int, Config> 中的元素 Config
	Config* cur = nullptr;
	SpecialConfig* spCur = nullptr;
	bool restoreWhiteList = false;

	while (getline(in, line1)) {
		line = utf8_to_wstring(line1);
		if (line.empty() || line.front() == L'#') continue;
		if (line.front() == L'[' && line.back() == L']') {
			section = line.substr(1, line.size() - 2);
			spCur = nullptr;
			cur = nullptr;
			if (section.find(L"Config", 0) == 0) {
				int idx = stoi(section.substr(6));
				g_appState.configs[idx] = Config();
				cur = &g_appState.configs[idx];
			}
			else if (section.find(L"SpCfg", 0) == 0) {
				int idx = stoi(section.substr(5));
				g_appState.specialConfigs[idx] = SpecialConfig();
				spCur = &g_appState.specialConfigs[idx];
			}
		}
		else {
			auto pos = line.find(L'=');
			if (pos == wstring::npos) continue;
			wstring key = line.substr(0, pos);
			wstring val = line.substr(pos + 1);

			if (cur) { // Inside a [ConfigN] section
				if (key == L"ConfigName") cur->name = wstring_to_utf8(val);
				else if (key == L"SavePath") {
					cur->saveRoot = val;
				}
				else if (key == L"WorldData") {
					while (getline(in, line1) && line1 != "*") {
						line = utf8_to_wstring(line1);
						wstring name = line;
						if (!getline(in, line1) || line1 == "*") break;
						line = utf8_to_wstring(line1);
						wstring desc = line;
						cur->worlds.push_back({ name, desc });
					}
					if (filesystem::exists(cur->saveRoot)) {
						for (auto& entry : filesystem::directory_iterator(cur->saveRoot)) {
							if (entry.is_directory() && checkWorldName(entry.path().filename().wstring(), cur->worlds))
								cur->worlds.push_back({ entry.path().filename().wstring(), L"" });
						}
					}
				}
				else if (key == L"BackupPath") cur->backupPath = val;
				else if (key == L"ZipProgram") cur->zipPath = val;
				else if (key == L"ZipFormat") cur->zipFormat = val;
				else if (key == L"ZipLevel") cur->zipLevel = stoi(val);
				else if (key == L"ZipMethod") cur->zipMethod = val;
				else if (key == L"KeepCount") cur->keepCount = stoi(val);
				else if (key == L"SmartBackup") cur->backupMode = stoi(val);
				else if (key == L"RestoreBeforeBackup") cur->backupBefore = (val != L"0");
				else if (key == L"HotBackup") cur->hotBackup = (val != L"0");
				else if (key == L"SilenceMode") isSilence = (val != L"0");
				else if (key == L"BackupNaming") cur->folderNameType = stoi(val);
				else if (key == L"SilenceMode") isSilence = (val != L"0");
				else if (key == L"CpuThreads") cur->cpuThreads = stoi(val);
				else if (key == L"UseLowPriority") cur->useLowPriority = (val != L"0");
				else if (key == L"SkipIfUnchanged") cur->skipIfUnchanged = (val != L"0");
				else if (key == L"MaxSmartBackups") cur->maxSmartBackupsPerFull = stoi(val);
				else if (key == L"BackupOnStart") cur->backupOnGameStart = (val != L"0");
				else if (key == L"BlacklistItem") cur->blacklist.push_back(val);
				else if (key == L"CloudSyncEnabled") cur->cloudSyncEnabled = (val != L"0");
				else if (key == L"RclonePath") cur->rclonePath = val;
				else if (key == L"RcloneRemotePath") cur->rcloneRemotePath = val;
				else if (key == L"SnapshotPath") cur->snapshotPath = val;
				else if (key == L"OtherPath") cur->othersPath = val;
				else if (key == L"Theme") {
					cur->theme = stoi(val);
					//ApplyTheme(cur->theme); 这个要转移至有gui之后，否则会直接导致崩溃
				}
				else if (key == L"Font") {
					cur->fontPath = val;
					Fontss = val;
					if (val.size() < 3 || !filesystem::exists(val)) { // 字体没有会导致崩溃，所以这里做个兜底
						GetUserDefaultUILanguageWin();
#ifdef _WIN32
						if (g_CurrentLang == "zh_CN") {
							if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttc"))
								Fontss = L"C:\\Windows\\Fonts\\msyh.ttc";
							else if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttf"))
								Fontss = L"C:\\Windows\\Fonts\\msyh.ttf";
						}
						else {
							Fontss = L"C:\\Windows\\Fonts\\SegoeUI.ttf";
						}
#else
						if (filesystem::exists("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"))
							Fontss = L"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
						else if (filesystem::exists("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"))
							Fontss = L"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc";
						else
							Fontss = L"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
					}
				}
			}
			else if (spCur) { // Inside a [SpCfgN] section
				if (key == L"Name") spCur->name = wstring_to_utf8(val);
				else if (key == L"AutoExecute") {
					spCur->autoExecute = (val != L"0");
					if (spCur->autoExecute)
						g_appState.specialConfigMode = true;
				}
				else if (key == L"ExitAfter") spCur->exitAfterExecution = (val != L"0");
				else if (key == L"Theme") spCur->theme = stoi(val);
				else if (key == L"HideWindow") spCur->hideWindow = (val != L"0");
				else if (key == L"RunOnStartup") spCur->runOnStartup = (val != L"0");
				else if (key == L"Command") spCur->commands.push_back(val);
				else if (key == L"AutoBackupTask") {
					wstringstream ss(val);
					AutomatedTask task;
					wchar_t delim;
					ss >> task.configIndex >> delim >> task.worldIndex >> delim >> task.backupType >> delim >> task.intervalMinutes >> delim >> task.schedMonth >> delim >> task.schedDay >> delim >> task.schedHour >> delim >> task.schedMinute;
					spCur->tasks.push_back(task);
				}
				else if (key == L"ZipLevel") spCur->zipLevel = stoi(val);
				else if (key == L"KeepCount") spCur->keepCount = stoi(val);
				else if (key == L"CpuThreads") spCur->cpuThreads = stoi(val);
				else if (key == L"UseLowPriority") spCur->useLowPriority = (val != L"0");
				else if (key == L"HotBackup") spCur->hotBackup = (val != L"0");
				else if (key == L"BackupOnStart") spCur->backupOnGameStart = (val != L"0");
				else if (key == L"BlacklistItem") spCur->blacklist.push_back(val);
			}
			else if (section == L"General") { // Inside [General] section
				if (key == L"CurrentConfig") {
					g_appState.currentConfigIndex = stoi(val);
				}
				else if (key == L"NextConfigId") {
					nextConfigId = stoi(val);
					int maxId = 0;
					for (auto& kv : g_appState.configs) if (kv.first > maxId) maxId = kv.first;
					for (auto& kv : g_appState.specialConfigs) if (kv.first > maxId) maxId = kv.first;
					if (nextConfigId <= maxId) nextConfigId = maxId + 1;
				}
				else if (key == L"Language") {
					if (val[2] == L'-')
						val[2] = L'_';
					g_CurrentLang = wstring_to_utf8(val);
				}
				else if (key == L"CheckForUpdates") {
					g_CheckForUpdates = (val != L"0");
				}
				else if (key == L"EnableKnotLink") {
					g_enableKnotLink = (val != L"0");
				}
				else if (key == L"RunOnStartup") {
					g_RunOnStartup = (val != L"0");
				}
				else if (key == L"IsSafeDelete") {
					isSafeDelete = (val != L"0");
				}
				else if (key == L"AutoBackupInterval") {
					last_interval = stoi(val);
				}
				else if (key == L"RestoreWhitelistItem") {
					restoreWhiteList = true;
					restoreWhitelist.push_back(val);
				}
			}
		}
	}
	if (!restoreWhiteList) {
		restoreWhitelist.push_back(L"fake_player.gca.json");
	}
}

void SaveConfigs(const wstring& filename) {
	lock_guard<mutex> lock(g_appState.configsMutex);
	wofstream out(filename, ios::binary);
	if (!out.is_open()) {
		MessageBoxW(nullptr, utf8_to_wstring(L("ERROR_CONFIG_WRITE_FAIL")).c_str(), utf8_to_wstring(L("ERROR_TITLE")).c_str(), MB_OK | MB_ICONERROR);
		return;
	}
	//out.imbue(locale("chs"));//不能用这个，变ANSI啦
	out.imbue(locale(out.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
	//out.imbue(locale(out.getloc(), new codecvt_utf8<wchar_t>));//将UTF8转为UTF，现在C++17也不对了……但是我们有define！
	out << L"[General]\n";
	out << L"CurrentConfig=" << g_appState.currentConfigIndex << L"\n";
	out << L"NextConfigId=" << nextConfigId << L"\n";
	out << L"Language=" << utf8_to_wstring(g_CurrentLang) << L"\n";
	out << L"CheckForUpdates=" << (g_CheckForUpdates ? 1 : 0) << L"\n";
	out << L"EnableKnotLink=" << (g_enableKnotLink ? 1 : 0) << L"\n";
	out << L"RunOnStartup=" << (g_RunOnStartup ? 1 : 0) << L"\n";
	out << L"IsSafeDelete=" << (isSafeDelete ? 1 : 0) << L"\n";
	out << L"AutoBackupInterval=" << last_interval << L"\n";
	for (const auto& item : restoreWhitelist) {
		out << L"RestoreWhitelistItem=" << item << L"\n";
	}
	out << "\n";

	for (auto& kv : g_appState.configs) {
		int idx = kv.first;
		Config& c = kv.second;
		out << L"[Config" << idx << L"]\n";
		out << L"ConfigName=" << utf8_to_wstring(c.name) << L"\n";
		out << L"SavePath=" << c.saveRoot << L"\n";
		out << L"# One line for name, one line for description, terminated by '*'\n";
		out << L"WorldData=\n";
		for (auto& p : c.worlds)
			out << p.first << L"\n" << p.second << L"\n";
		out << L"*\n";
		out << L"BackupPath=" << c.backupPath << L"\n";
		out << L"ZipProgram=" << c.zipPath << L"\n";
		out << L"ZipFormat=" << c.zipFormat << L"\n";
		out << L"ZipLevel=" << c.zipLevel << L"\n";
		out << L"ZipMethod=" << c.zipMethod << L"\n";
		out << L"CpuThreads=" << c.cpuThreads << L"\n";
		out << L"UseLowPriority=" << (c.useLowPriority ? 1 : 0) << L"\n";
		out << L"KeepCount=" << c.keepCount << L"\n";
		out << L"SmartBackup=" << c.backupMode << L"\n";
		out << L"RestoreBeforeBackup=" << (c.backupBefore ? 1 : 0) << L"\n";
		out << L"HotBackup=" << (c.hotBackup ? 1 : 0) << L"\n";
		out << L"SilenceMode=" << (isSilence ? 1 : 0) << L"\n";
		out << L"Theme=" << c.theme << L"\n";
		out << L"Font=" << c.fontPath << L"\n";
		out << L"BackupNaming=" << c.folderNameType << L"\n";
		out << L"SilenceMode=" << (isSilence ? 1 : 0) << L"\n";
		out << L"SkipIfUnchanged=" << (c.skipIfUnchanged ? 1 : 0) << L"\n";
		out << L"MaxSmartBackups=" << c.maxSmartBackupsPerFull << L"\n";
		out << L"BackupOnStart=" << (c.backupOnGameStart ? 1 : 0) << L"\n";
		out << L"CloudSyncEnabled=" << (c.cloudSyncEnabled ? 1 : 0) << L"\n";
		out << L"RclonePath=" << c.rclonePath << L"\n";
		out << L"RcloneRemotePath=" << c.rcloneRemotePath << L"\n";
		out << L"SnapshotPath=" << c.snapshotPath << L"\n";
		out << L"OtherPath=" << c.othersPath << L"\n";
		for (const auto& item : c.blacklist) {
			out << L"BlacklistItem=" << item << L"\n";
		}
		out << L"\n";
	}

	for (auto& kv : g_appState.specialConfigs) {
		int idx = kv.first;
		SpecialConfig& sc = kv.second;
		out << L"[SpCfg" << idx << L"]\n";
		out << L"Name=" << utf8_to_wstring(sc.name) << L"\n";
		out << L"AutoExecute=" << (sc.autoExecute ? 1 : 0) << L"\n";
		for (const auto& cmd : sc.commands) out << L"Command=" << cmd << L"\n";
		// 新的任务结构
		for (const auto& task : sc.tasks) {
			out << L"AutoBackupTask=" << task.configIndex << L"," << task.worldIndex << L"," << task.backupType
				<< L"," << task.intervalMinutes << L"," << task.schedMonth << L"," << task.schedDay
				<< L"," << task.schedHour << L"," << task.schedMinute << L"\n";
		}
		out << L"ExitAfter=" << (sc.exitAfterExecution ? 1 : 0) << L"\n";
		out << L"HideWindow=" << (sc.hideWindow ? 1 : 0) << L"\n";
		out << L"RunOnStartup=" << (sc.runOnStartup ? 1 : 0) << L"\n";
		out << L"ZipLevel=" << sc.zipLevel << L"\n";
		out << L"KeepCount=" << sc.keepCount << L"\n";
		out << L"CpuThreads=" << sc.cpuThreads << L"\n";
		out << L"UseLowPriority=" << (sc.useLowPriority ? 1 : 0) << L"\n";
		out << L"HotBackup=" << (sc.hotBackup ? 1 : 0) << L"\n";
		out << L"BackupOnStart=" << (sc.backupOnGameStart ? 1 : 0) << L"\n";
		out << L"Theme=" << sc.theme << L"\n";
		for (const auto& item : sc.blacklist) {
			out << L"BlacklistItem=" << item << L"\n";
		}
		out << L"\n\n";
	}
}

void CheckForConfigConflicts() {
	lock_guard<mutex> lock(g_appState.configsMutex);
	map<wstring, vector<pair<int, wstring>>> worldMap; // Key: World Name, Value: {ConfigIndex, BackupPath}

	for (const auto& conf_pair : g_appState.configs) {
		int config_idx = conf_pair.first;
		const Config& cfg = conf_pair.second;
		for (const auto& world_pair : cfg.worlds) {
			const wstring& worldName = world_pair.first;
			worldMap[worldName].push_back({ config_idx, cfg.backupPath });
		}
	}

	wstring conflictDetails = L"";
	bool ifConf = false;

	for (const auto& map_pair : worldMap) {
		const vector<pair<int, wstring>>& entries = map_pair.second;
		if (entries.size() > 1) { // 如果有多个配置使用同一个世界名
			for (size_t i = 0; i < entries.size(); ++i) {
				for (size_t j = i + 1; j < entries.size(); ++j) { // 比较每对配置
					if (entries[i].second == entries[j].second && !entries[i].second.empty()) {
						ifConf = true;
						break;
						//wchar_t buffer[512];
						/*swprintf_s(buffer, 50, L"%d plus %d is %d", 10, 20, (10 + 20));
						swprintf_s(buffer, CONSTANT1, L(L("CONFIG_CONFLICT_ENTRY")),
							entries[i].first,
							entries[j].first,
							map_pair.first.c_str(),
							entries[i].second.c_str());*/
							//conflictDetails += buffer;
					}
				}
			}
			if (ifConf)
				break;
		}
	}
	if (ifConf) {
		wchar_t finalMessage[CONSTANT1];
		//strncpy_s(finalMessage, L("CONFIG_CONFLICT_MESSAGE"),100);
		swprintf_s(finalMessage, CONSTANT1, utf8_to_wstring(L("CONFIG_CONFLICT_MESSAGE")).c_str());
		MessageBoxW(nullptr, finalMessage, utf8_to_wstring(L("CONFIG_CONFLICT_TITLE")).c_str(), MB_OK | MB_ICONWARNING);
	}

}