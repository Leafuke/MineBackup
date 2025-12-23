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
#include <stdexcept>
using namespace std;

static wstring GetDefaultFontPath() {
#ifdef _WIN32
	if (g_CurrentLang == "zh_CN") {
		const wstring cn_candidates[] = {
			L"C:\\Windows\\Fonts\\msyh.ttc",
			L"C:\\Windows\\Fonts\\msyh.ttf",
			L"C:\\Windows\\Fonts\\msjh.ttc",
			L"C:\\Windows\\Fonts\\msjh.ttf",
			L"C:\\Windows\\Fonts\\SegoeUI.ttf"
		};
		for (const auto& cand : cn_candidates) {
			if (filesystem::exists(cand)) return cand;
		}
	}
	const wstring en_candidates[] = {
		L"C:\\Windows\\Fonts\\SegoeUI.ttf"
	};
	for (const auto& cand : en_candidates) {
		if (filesystem::exists(cand)) return cand;
	}
	return en_candidates[0];
#else
	const wstring candidates[] = {
		L"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
		L"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
		L"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
	};
	for (const auto& cand : candidates) {
		if (filesystem::exists(cand)) return cand;
	}
	return candidates[sizeof(candidates) / sizeof(candidates[0]) - 1];
#endif
}

static int nextConfigId = 2; // 从 2 开始，因为 1 被向导占用
extern int g_hotKeyBackupId , g_hotKeyRestoreId;

extern bool g_RunOnStartup;
extern bool g_enableKnotLink;
extern bool g_CheckForUpdates;
extern bool g_ReceiveNotices;
extern bool g_StopAutoBackupOnExit;
extern bool isSilence;
extern bool isSafeDelete;
extern bool g_AutoScanForWorlds;
extern bool g_autoLogEnabled;
extern wstring Fontss;
extern string g_NoticeLastSeenVersion;
extern vector<wstring> restoreWhitelist;
extern int last_interval;
extern int g_windowHeight, g_windowWidth;
extern float g_uiScale;

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
	ifstream in(filename.c_str(), ios::binary);
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
				else if (key == L"EnableWEIntegration") cur->enableWEIntegration = (val != L"0");
				else if (key == L"WESnapshotPath") cur->weSnapshotPath = val;
				else if (key == L"Theme") {
					cur->theme = stoi(val);
					//ApplyTheme(cur->theme); 这个要转移至有gui之后，否则会直接导致崩溃
				}
				else if (key == L"Font") {
					cur->fontPath = val;
					Fontss = val;
					auto applyDefaultFont = [&]() {
						Fontss = GetDefaultFontPath();
						cur->fontPath = Fontss;
					};
					if (val.empty()) {
						applyDefaultFont();
					}
					else if (val.size() < 3 || !filesystem::exists(val)) { // 字体没有会导致崩溃，所以这里做个兜底
						MessageBoxWin("Warning", "Invalid font path!\nPlease check and reload.", 1);
						GetUserDefaultUILanguageWin();
						applyDefaultFont();
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
				else if (key == L"ReceiveNotices") {
					g_ReceiveNotices = (val != L"0");
				}
				else if (key == L"NoticeLastSeen") {
					g_NoticeLastSeenVersion = wstring_to_utf8(val);
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
				else if (key == L"StopAutoBackupOnExit") {
					g_StopAutoBackupOnExit = (val != L"0");
				}
				else if (key == L"RestoreWhitelistItem") {
					restoreWhiteList = true;
					restoreWhitelist.push_back(val);
				}
				else if (key == L"WindowWidth") {
					if (stoi(val) > 10) {
						g_windowWidth = stoi(val);
					}
				}
				else if (key == L"WindowHeight") {
					if (stoi(val) > 10) {
						g_windowHeight = stoi(val);
					}
				}
				else if (key == L"UIScale") {
					g_uiScale = stof(val);
				}
				else if (key == L"AutoScanForWorlds") {
					g_AutoScanForWorlds = (val != L"0");
				}
				else if (key == L"HotkeyBackup") {
					g_hotKeyBackupId = stoi(val);
				}
				else if (key == L"HotkeyRestore") {
					g_hotKeyRestoreId = stoi(val);
				}
				else if (key == L"AutoLog") {
					g_autoLogEnabled = (val != L"0");
				}
			}
		}
	}
	if (!restoreWhiteList) {
		restoreWhitelist.push_back(L"fake_player.gca.json");
		restoreWhitelist.push_back(L"xaeromap.txt");
		restoreWhitelist.push_back(L"soul_archive.json");
		restoreWhitelist.push_back(L"level.dat");
		restoreWhitelist.push_back(L"level.dat_old");
	}
}

void SaveConfigs(const wstring& filename) {
	lock_guard<mutex> lock(g_appState.configsMutex);
	ofstream out{std::filesystem::path(filename), ios::binary};
	if (!out.is_open()) {
		MessageBoxWin(L("ERROR_CONFIG_WRITE_FAIL"), L("ERROR_TITLE"), 2);
		return;
	}

	std::wostringstream buffer;
	buffer << L"[General]\n";
	buffer << L"CurrentConfig=" << g_appState.currentConfigIndex << L"\n";
	buffer << L"NextConfigId=" << nextConfigId << L"\n";
	buffer << L"Language=" << utf8_to_wstring(g_CurrentLang) << L"\n";
	buffer << L"CheckForUpdates=" << (g_CheckForUpdates ? 1 : 0) << L"\n";
	buffer << L"ReceiveNotices=" << (g_ReceiveNotices ? 1 : 0) << L"\n";
	buffer << L"NoticeLastSeen=" << utf8_to_wstring(g_NoticeLastSeenVersion) << L"\n";
	buffer << L"EnableKnotLink=" << (g_enableKnotLink ? 1 : 0) << L"\n";
	buffer << L"RunOnStartup=" << (g_RunOnStartup ? 1 : 0) << L"\n";
	buffer << L"IsSafeDelete=" << (isSafeDelete ? 1 : 0) << L"\n";
	buffer << L"AutoBackupInterval=" << last_interval << L"\n";
	buffer << L"StopAutoBackupOnExit=" << (g_StopAutoBackupOnExit ? 1 : 0) << L"\n";
	buffer << L"AutoScanForWorlds=" << (g_AutoScanForWorlds ? 1 : 0) << L"\n";
	buffer << L"WindowWidth=" << g_windowWidth << L"\n";
	buffer << L"WindowHeight=" << g_windowHeight << L"\n";
	buffer << L"UIScale=" << g_uiScale << L"\n";
	buffer << L"HotkeyBackup=" << g_hotKeyBackupId << L"\n";
	buffer << L"HotkeyRestore=" << g_hotKeyRestoreId << L"\n";
	buffer << L"AutoLog=" << (g_autoLogEnabled ? 1 : 0) << L"\n";
	for (const auto& item : restoreWhitelist) {
		buffer << L"RestoreWhitelistItem=" << item << L"\n";
	}
	buffer << L"\n";

	for (auto& kv : g_appState.configs) {
		int idx = kv.first;
		Config& c = kv.second;
		buffer << L"[Config" << idx << L"]\n";
		buffer << L"ConfigName=" << utf8_to_wstring(c.name) << L"\n";
		buffer << L"SavePath=" << c.saveRoot << L"\n";
		buffer << L"# One line for name, one line for description, terminated by '*'\n";
		buffer << L"WorldData=\n";
		for (auto& p : c.worlds)
			buffer << p.first << L"\n" << p.second << L"\n";
		buffer << L"*\n";
		buffer << L"BackupPath=" << c.backupPath << L"\n";
		buffer << L"ZipProgram=" << c.zipPath << L"\n";
		buffer << L"ZipFormat=" << c.zipFormat << L"\n";
		buffer << L"ZipLevel=" << c.zipLevel << L"\n";
		buffer << L"ZipMethod=" << c.zipMethod << L"\n";
		buffer << L"CpuThreads=" << c.cpuThreads << L"\n";
		buffer << L"UseLowPriority=" << (c.useLowPriority ? 1 : 0) << L"\n";
		buffer << L"KeepCount=" << c.keepCount << L"\n";
		buffer << L"SmartBackup=" << c.backupMode << L"\n";
		buffer << L"RestoreBeforeBackup=" << (c.backupBefore ? 1 : 0) << L"\n";
		buffer << L"HotBackup=" << (c.hotBackup ? 1 : 0) << L"\n";
		buffer << L"SilenceMode=" << (isSilence ? 1 : 0) << L"\n";
		buffer << L"Theme=" << c.theme << L"\n";
		buffer << L"Font=" << c.fontPath << L"\n";
		buffer << L"BackupNaming=" << c.folderNameType << L"\n";
		buffer << L"SilenceMode=" << (isSilence ? 1 : 0) << L"\n";
		buffer << L"SkipIfUnchanged=" << (c.skipIfUnchanged ? 1 : 0) << L"\n";
		buffer << L"MaxSmartBackups=" << c.maxSmartBackupsPerFull << L"\n";
		buffer << L"BackupOnStart=" << (c.backupOnGameStart ? 1 : 0) << L"\n";
		buffer << L"CloudSyncEnabled=" << (c.cloudSyncEnabled ? 1 : 0) << L"\n";
		buffer << L"RclonePath=" << c.rclonePath << L"\n";
		buffer << L"RcloneRemotePath=" << c.rcloneRemotePath << L"\n";
		buffer << L"SnapshotPath=" << c.snapshotPath << L"\n";
		buffer << L"OtherPath=" << c.othersPath << L"\n";
		buffer << L"EnableWEIntegration=" << (c.enableWEIntegration ? 1 : 0) << L"\n";
		buffer << L"WESnapshotPath=" << c.weSnapshotPath << L"\n";
		for (const auto& item : c.blacklist) {
			buffer << L"BlacklistItem=" << item << L"\n";
		}
		buffer << L"\n";
	}

	for (auto& kv : g_appState.specialConfigs) {
		int idx = kv.first;
		SpecialConfig& sc = kv.second;
		buffer << L"[SpCfg" << idx << L"]\n";
		buffer << L"Name=" << utf8_to_wstring(sc.name) << L"\n";
		buffer << L"AutoExecute=" << (sc.autoExecute ? 1 : 0) << L"\n";
		for (const auto& cmd : sc.commands) buffer << L"Command=" << cmd << L"\n";
		for (const auto& task : sc.tasks) {
			buffer << L"AutoBackupTask=" << task.configIndex << L"," << task.worldIndex << L"," << task.backupType
				<< L"," << task.intervalMinutes << L"," << task.schedMonth << L"," << task.schedDay
				<< L"," << task.schedHour << L"," << task.schedMinute << L"\n";
		}
		buffer << L"ExitAfter=" << (sc.exitAfterExecution ? 1 : 0) << L"\n";
		buffer << L"HideWindow=" << (sc.hideWindow ? 1 : 0) << L"\n";
		buffer << L"RunOnStartup=" << (sc.runOnStartup ? 1 : 0) << L"\n";
		buffer << L"ZipLevel=" << sc.zipLevel << L"\n";
		buffer << L"KeepCount=" << sc.keepCount << L"\n";
		buffer << L"CpuThreads=" << sc.cpuThreads << L"\n";
		buffer << L"UseLowPriority=" << (sc.useLowPriority ? 1 : 0) << L"\n";
		buffer << L"HotBackup=" << (sc.hotBackup ? 1 : 0) << L"\n";
		buffer << L"BackupOnStart=" << (sc.backupOnGameStart ? 1 : 0) << L"\n";
		buffer << L"Theme=" << sc.theme << L"\n";
		for (const auto& item : sc.blacklist) {
			buffer << L"BlacklistItem=" << item << L"\n";
		}
		buffer << L"\n\n";
	}

	const string utf8 = wstring_to_utf8(buffer.str());
	out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

// 在 LoadConfigs/SaveConfigs/CheckForConfigConflicts 等函数关键处调用日志接口
// 例如：
// WriteLogEntry("Configs loaded from " + filename, LogLevel::Info);
// WriteLogEntry("Configs saved to " + wstring_to_utf8(filename), LogLevel::Info);
// WriteLogEntry("Config conflict detected: " + wstring_to_utf8(conflictDetails), LogLevel::Warning);

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
						wchar_t buffer[CONSTANT2];
						swprintf_s(buffer, CONSTANT2, L"\n\nConfig:%d and Config:%d \n World:%s \n Path:%s",
							entries[i].first,
							entries[j].first,
							map_pair.first.c_str(),
							entries[i].second.c_str());
						conflictDetails += buffer;
						break;
					}
				}
			}
			if (ifConf)
				break;
		}
	}
	if (ifConf) {
		string finalMessage;
		//strncpy_s(finalMessage, L("CONFIG_CONFLICT_MESSAGE"),100);
		finalMessage = L("CONFIG_CONFLICT_MESSAGE") + wstring_to_utf8(conflictDetails);
		MessageBoxWin(L("CONFIG_CONFLICT_TITLE"), finalMessage, 1);
	}

}