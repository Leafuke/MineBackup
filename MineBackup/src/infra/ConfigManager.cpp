#include "ConfigManager.h"
#include "AppState.h"
#include "UIHelpers.h"
#include "AppPaths.h"
#include "AtomicFileWriter.h"
#include "FolderRewindFormat.h"
#include "MigrationCoordinator.h"
#include "SpecialConfigPolicy.h"
#include "SpecialTaskDocument.h"
#include "Globals.h"
#include "Logging.h"
#include "LegacyIniConfigCodec.h"
#include "text_to_text.h"
#include "i18n.h"
#include "PlatformCompat.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <set>
#include <optional>
#include <limits>
using namespace std;

namespace {

vector<LegacyIniConfigCodec::Diagnostic> g_configLoadDiagnostics;

filesystem::path SpecialTasksPathForConfig(const filesystem::path& configFile) {
	return configFile.parent_path() / L"special-tasks.json";
}

void RecordConfigDiagnostic(
	LegacyIniConfigCodec::DiagnosticSeverity severity,
	size_t line,
	const wstring& section,
	const wstring& key,
	const string& detail) {
	const char* eventId = severity == LegacyIniConfigCodec::DiagnosticSeverity::Fatal
		? "config.parse.invalid_operational_value"
		: "config.parse.invalid_optional_value";
	g_configLoadDiagnostics.push_back({severity, line, section, key, eventId, detail});
	const string sectionUtf8 = wstring_to_utf8(section);
	const string keyUtf8 = wstring_to_utf8(key);
	if (severity == LegacyIniConfigCodec::DiagnosticSeverity::Fatal) {
		MB_LOG_ERROR(minebackup::logging::LogCategory::Migration, eventId,
			"Invalid configuration value at line {} [{}] {}: {}",
			line, sectionUtf8, keyUtf8, detail);
	}
	else {
		MB_LOG_WARNING(minebackup::logging::LogCategory::Migration, eventId,
			"Invalid optional configuration value at line {} [{}] {}: {}",
			line, sectionUtf8, keyUtf8, detail);
	}
}

void RecordSpecialTaskDiagnostic(const SpecialTaskStorage::Diagnostic& diagnostic) {
	const bool fatal = diagnostic.severity == SpecialTaskStorage::DiagnosticSeverity::Fatal;
	g_configLoadDiagnostics.push_back({
		fatal ? LegacyIniConfigCodec::DiagnosticSeverity::Fatal
			: LegacyIniConfigCodec::DiagnosticSeverity::Warning,
		0,
		L"SpecialTasks",
		diagnostic.taskId,
		diagnostic.eventId,
		diagnostic.detail});
	if (fatal) {
		MB_LOG_ERROR(minebackup::logging::LogCategory::Migration,
			diagnostic.eventId,
			"Special task document error (special_config_id={}, task_id={}): {}",
			wstring_to_utf8(diagnostic.specialConfigId),
			wstring_to_utf8(diagnostic.taskId), diagnostic.detail);
	}
	else {
		MB_LOG_WARNING(minebackup::logging::LogCategory::Migration,
			diagnostic.eventId,
			"Special task migration warning (special_config_id={}, task_id={}): {}",
			wstring_to_utf8(diagnostic.specialConfigId),
			wstring_to_utf8(diagnostic.taskId), diagnostic.detail);
	}
}

} // namespace

const vector<LegacyIniConfigCodec::Diagnostic>& GetLastConfigLoadDiagnostics() {
	return g_configLoadDiagnostics;
}

bool LastConfigLoadHasFatalDiagnostics() {
	return LegacyIniConfigCodec::HasFatalDiagnostics(g_configLoadDiagnostics);
}

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
#elif defined(__APPLE__)
	const wstring cn_candidates[] = {
		L"/System/Library/Fonts/PingFang.ttc",
		L"/System/Library/Fonts/STHeiti Light.ttc",
		L"/System/Library/Fonts/STHeiti Medium.ttc",
		L"/System/Library/Fonts/AppleSDGothicNeo.ttc"
	};
	for (const auto& cand : cn_candidates) {
		if (filesystem::exists(cand)) return cand;
	}
	const wstring en_candidates[] = {
		L"/System/Library/Fonts/SFNS.ttf",
		L"/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
		L"/System/Library/Fonts/Supplemental/Arial.ttf",
		L"/Library/Fonts/Arial.ttf"
	};
	for (const auto& cand : en_candidates) {
		if (filesystem::exists(cand)) return cand;
	}
	return cn_candidates[0];
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

static int NormalizeCompressionLevel(const wstring& method, int level) {
	int minLevel = 1;
	int maxLevel = 9;
	if (_wcsicmp(method.c_str(), L"zstd") == 0) {
		maxLevel = 22;
	}
	if (level < minLevel) return minLevel;
	if (level > maxLevel) return maxLevel;
	return level;
}

static int nextConfigId = 2; // 从 2 开始，因为 1 被向导占用

static bool IsWorldNameAvailable(
	const wstring& world,
	const vector<pair<wstring, wstring>>& worldList) {
	return none_of(worldList.begin(), worldList.end(), [&](const auto& item) {
		return item.first == world;
	});
}

static bool ContainsRuleIgnoreCase(const vector<wstring>& rules, const wstring& rule) {
	return any_of(rules.begin(), rules.end(), [&](const wstring& item) {
		return _wcsicmp(item.c_str(), rule.c_str()) == 0;
		});
}

vector<wstring> DefaultBackupBlacklist() {
	return {
		L"session.lock",
		L"voxy",
		L"DistantHorizons.sqlite",
		L"DistantHorizons.sqlite-shm",
		L"DistantHorizons.sqlite-wal"
	};
}

vector<wstring> DefaultRestoreWhitelist() {
	return {
		L"session.lock",
		L"xaeromap.txt",
		L"soul_archive.json",
		L"voxy",
		L"DistantHorizons.sqlite",
		L"DistantHorizons.sqlite-shm",
		L"DistantHorizons.sqlite-wal"
	};
}

void EnsureDefaultBackupBlacklist(vector<wstring>& blacklist) {
	for (const auto& item : DefaultBackupBlacklist()) {
		if (!ContainsRuleIgnoreCase(blacklist, item)) {
			blacklist.push_back(item);
		}
	}
}

void EnsureDefaultRestoreWhitelist() {
	if (!restoreWhitelist.empty() && ContainsRuleIgnoreCase(restoreWhitelist, L"session.lock")) return;
	restoreWhitelist = DefaultRestoreWhitelist();
}

vector<wstring> BuildEffectiveRestoreWhitelist(const vector<wstring>& userWhitelist) {
	vector<wstring> effective = userWhitelist;
	// session.lock 不能被还原覆盖；UI 可移除显示项，但运行时始终保护它。
	if (!ContainsRuleIgnoreCase(effective, L"session.lock")) {
		effective.push_back(L"session.lock");
	}
	return effective;
}

int CreateNewSpecialConfig(const string& name_hint) {
	int newId = nextConfigId++;
	SpecialConfig sp;
	sp.name = name_hint;
	sp.specialConfigId = FolderRewindFormat::GenerateGuidString();
	EnsureDefaultBackupBlacklist(sp.blacklist);
	EnsureDefaultRestoreWhitelist();
	g_appState.specialConfigs[newId] = sp;
	return newId;
}

int CreateNewNormalConfig(const string& name_hint) {
	int newId = nextConfigId++;
	Config new_cfg;
	new_cfg.name = name_hint;
	new_cfg.configId = FolderRewindFormat::GenerateGuidString();
	// 默认空的路径/世界
	new_cfg.saveRoot.clear();
	new_cfg.backupPath.clear();
	new_cfg.worlds.clear();
	EnsureDefaultBackupBlacklist(new_cfg.blacklist);
	EnsureDefaultRestoreWhitelist();
	g_appState.configs[newId] = new_cfg;
	return newId;
}

void AssignFreshNormalConfigId(int configIndex) {
	auto it = g_appState.configs.find(configIndex);
	if (it == g_appState.configs.end()) return;
	it->second.configId = FolderRewindFormat::GenerateGuidString();
}

void EnsureConfigIds() {
	for (auto& kv : g_appState.configs) {
		kv.second.configId = FolderRewindFormat::EnsureConfigId(kv.second.configId);
	}
}

void LoadConfigs() {
	LoadConfigs(GetAppPaths().ConfigFile());
}

void LoadConfigs(const filesystem::path& filename) {
	lock_guard<mutex> lock(g_appState.configsMutex);
	g_configLoadDiagnostics.clear();
	g_appState.configs.clear();
	g_appState.specialConfigs.clear();
	g_appState.specialConfigMode = false;
	g_theme = static_cast<int>(ThemeId::ImGuiLight);
	g_lastValidTheme = static_cast<int>(ThemeId::ImGuiLight);
	Fontss.clear();
	g_appearanceSchema = 1;
	g_uiScaleV2 = true;
	g_uiScaleMigrationPending = false;
	restoreWhitelist.clear();
	g_logFileLevel = minebackup::logging::LogFileLevel::Info;
	g_logViewLevel = minebackup::logging::LogLevel::Info;
	g_logViewAutoTail = true;
	g_logViewShowTime = false;
	g_logViewShowCategory = false;
	optional<wstring> configuredLogFileLevel;
	optional<wstring> configuredLogViewLevel;
	optional<bool> legacyAutoLog;
	const filesystem::path specialTasksPath = SpecialTasksPathForConfig(filename);
	error_code specialTasksExistsError;
	const bool hasAuthoritativeSpecialTasks = filesystem::exists(
		specialTasksPath, specialTasksExistsError);
	ifstream in(filename, ios::binary);
	if (!in.is_open()) {
		Fontss = GetDefaultFontPath();
		minebackup::logging::SetFileLevel(g_logFileLevel);
		return;
	}
	optional<int> configuredGlobalTheme;
	optional<int> configuredThemeFallback;
	optional<wstring> configuredGlobalFont;
	optional<int> configuredAppearanceSchema;
	bool configuredUiScaleV2 = false;
	bool configuredUiScaleFound = false;
	string line1;
	wstring line, section;
	// cur作为一个指针，指向 g_appState.configs 这个全局 map<int, Config> 中的元素 Config
	Config* cur = nullptr;
	SpecialConfig* spCur = nullptr;
	size_t lineNumber = 0;

	while (getline(in, line1)) {
		++lineNumber;
		line = utf8_to_wstring(line1);
		if (line.empty() || line.front() == L'#') continue;
		if (line.front() == L'[' && line.back() == L']') {
			section = line.substr(1, line.size() - 2);
			spCur = nullptr;
			cur = nullptr;
			if (section.find(L"Config", 0) == 0) {
				int idx = 0;
				if (!LegacyIniConfigCodec::TryParseInt(
						section.substr(6), 1, (numeric_limits<int>::max)(), idx)) {
					RecordConfigDiagnostic(
						LegacyIniConfigCodec::DiagnosticSeverity::Fatal,
						lineNumber, section, L"section", "invalid Config section index");
					section.clear();
					continue;
				}
				g_appState.configs[idx] = Config();
				cur = &g_appState.configs[idx];
			}
			else if (section.find(L"SpCfg", 0) == 0) {
				int idx = 0;
				if (!LegacyIniConfigCodec::TryParseInt(
						section.substr(5), 1, (numeric_limits<int>::max)(), idx)) {
					RecordConfigDiagnostic(
						LegacyIniConfigCodec::DiagnosticSeverity::Fatal,
						lineNumber, section, L"section", "invalid SpCfg section index");
					section.clear();
					continue;
				}
				g_appState.specialConfigs[idx] = SpecialConfig();
				spCur = &g_appState.specialConfigs[idx];
			}
		}
		else {
			auto pos = line.find(L'=');
			if (pos == wstring::npos) continue;
			wstring key = line.substr(0, pos);
			wstring val = line.substr(pos + 1);
			auto readInt = [&](int& target, int minimum, int maximum, bool fatal) {
				int parsed = 0;
				if (LegacyIniConfigCodec::TryParseInt(val, minimum, maximum, parsed)) {
					target = parsed;
					return true;
				}
				RecordConfigDiagnostic(
					fatal ? LegacyIniConfigCodec::DiagnosticSeverity::Fatal
						: LegacyIniConfigCodec::DiagnosticSeverity::Warning,
					lineNumber, section, key,
					"expected an integer in the supported range");
				return false;
			};
			auto readFloat = [&](float& target, float minimum, float maximum, bool fatal) {
				float parsed = 0.0f;
				if (LegacyIniConfigCodec::TryParseFloat(val, minimum, maximum, parsed)) {
					target = parsed;
					return true;
				}
				RecordConfigDiagnostic(
					fatal ? LegacyIniConfigCodec::DiagnosticSeverity::Fatal
						: LegacyIniConfigCodec::DiagnosticSeverity::Warning,
					lineNumber, section, key,
					"expected a finite number in the supported range");
				return false;
			};

			if (cur) { // Inside a [ConfigN] section
				if (key == L"ConfigName") cur->name = wstring_to_utf8(val);
				else if (key == L"ConfigId") cur->configId = FolderRewindFormat::EnsureConfigId(val);
				else if (key == L"PendingLocalBinding") cur->pendingLocalBinding = (val != L"0");
				else if (key == L"SavePath") {
					cur->saveRoot = val;
				}
				else if (key == L"WorldData") {
					while (getline(in, line1) && line1 != "*") {
						++lineNumber;
						line = utf8_to_wstring(line1);
						wstring name = line;
						if (!getline(in, line1)) {
							RecordConfigDiagnostic(
								LegacyIniConfigCodec::DiagnosticSeverity::Fatal,
								lineNumber, section, key, "truncated WorldData entry");
							break;
						}
						++lineNumber;
						if (line1 == "*") {
							RecordConfigDiagnostic(
								LegacyIniConfigCodec::DiagnosticSeverity::Fatal,
								lineNumber, section, key, "world description is missing");
							break;
						}
						line = utf8_to_wstring(line1);
						wstring desc = line;
						cur->worlds.push_back({ name, desc });
					}
					if (filesystem::exists(cur->saveRoot)) {
						error_code scanError;
						for (filesystem::directory_iterator it(cur->saveRoot, scanError), end;
							!scanError && it != end; it.increment(scanError)) {
							const auto& entry = *it;
							if (entry.is_directory() && IsWorldNameAvailable(entry.path().filename().wstring(), cur->worlds))
								cur->worlds.push_back({ entry.path().filename().wstring(), L"" });
						}
						if (scanError) {
							RecordConfigDiagnostic(
								LegacyIniConfigCodec::DiagnosticSeverity::Warning,
								lineNumber, section, key, "world directory scan failed");
						}
					}
				}
				else if (key == L"BackupPath") cur->backupPath = val;
				else if (key == L"ZipProgram") cur->zipPath = val;
				else if (key == L"ZipFormat") cur->zipFormat = val;
				else if (key == L"ZipLevel") readInt(cur->zipLevel, 0, 22, true);
				else if (key == L"ZipMethod") cur->zipMethod = val;
				else if (key == L"KeepCount") readInt(cur->keepCount, 0, 100000, true);
				else if (key == L"SmartBackup") readInt(cur->backupMode, 0, 2, true);
				else if (key == L"RestoreBeforeBackup") cur->backupBefore = (val != L"0");
				else if (key == L"SilenceMode") { /* ignored legacy setting */ }
				else if (key == L"CpuThreads") readInt(cur->cpuThreads, 0, 1024, true);
				else if (key == L"UseLowPriority") cur->useLowPriority = (val != L"0");
				else if (key == L"SkipIfUnchanged") cur->skipIfUnchanged = (val != L"0");
				else if (key == L"MaxSmartBackups") readInt(cur->maxSmartBackupsPerFull, 0, 100000, true);
				else if (key == L"BackupOnStart") cur->backupOnGameStart = (val != L"0");
				else if (key == L"BlacklistItem") cur->blacklist.push_back(val);
				else if (key == L"CloudSyncEnabled") cur->cloudSyncEnabled = (val != L"0");
				else if (key == L"RclonePath") cur->rclonePath = val;
				else if (key == L"RcloneRemotePath") cur->rcloneRemotePath = val;
				else if (key == L"CloudSyncMode") readInt(cur->cloudSyncMode, 0, 1, true);
				else if (key == L"CloudWorkingDirectory") cur->cloudWorkingDirectory = val;
				else if (key == L"CloudTimeoutSeconds") readInt(cur->cloudTimeoutSeconds, 1, 86400, true);
				else if (key == L"CloudRetryCount") readInt(cur->cloudRetryCount, 0, 100, true);
				else if (key == L"CloudSyncHistoryAfterUpload") cur->cloudSyncHistoryAfterUpload = (val != L"0");
				else if (key == L"CloudAutoDownloadBeforeRestore") cur->cloudAutoDownloadBeforeRestore = (val != L"0");
				else if (key == L"CloudLastRunUtc") cur->cloudLastRunUtc = val;
				else if (key == L"CloudLastExitCode") readInt(cur->cloudLastExitCode, (numeric_limits<int>::min)(), (numeric_limits<int>::max)(), false);
				else if (key == L"CloudLastErrorMessage") cur->cloudLastErrorMessage = val;
				else if (key == L"SnapshotPath") cur->snapshotPath = val;
				else if (key == L"OtherPath") cur->othersPath = val;
				else if (key == L"EnableWEIntegration") cur->enableWEIntegration = (val != L"0");
				else if (key == L"WESnapshotPath") cur->weSnapshotPath = val;
				else if (key == L"Theme") {
					readInt(cur->theme, -1, 32, false);
				}
				else if (key == L"Font") {
					cur->fontPath = val;
				}
			}
			else if (spCur) { // Inside a [SpCfgN] section
				if (key == L"Name") spCur->name = wstring_to_utf8(val);
				else if (key == L"SpecialConfigId") spCur->specialConfigId = FolderRewindFormat::EnsureConfigId(val);
				else if (key == L"AutoExecute") {
					spCur->autoExecute = (val != L"0");
				}
				else if (key == L"ExitAfter") spCur->exitAfterExecution = (val != L"0");
				else if (key == L"Theme") readInt(spCur->theme, -1, 32, false);
				else if (key == L"HideWindow") spCur->hideWindow = (val != L"0");
				else if (key == L"RunOnStartup") spCur->runOnStartup = (val != L"0");
				else if (key == L"Command") {
					if (!hasAuthoritativeSpecialTasks) spCur->commands.push_back(val);
				}
				else if (key == L"AutoBackupTask") {
					if (hasAuthoritativeSpecialTasks) continue;
					const auto tokens = LegacyIniConfigCodec::Split(val, L',');
					int values[8]{};
					const pair<int, int> ranges[8] = {
						{-1, (numeric_limits<int>::max)()}, {-1, (numeric_limits<int>::max)()},
						{0, 2}, {1, 525600}, {0, 12}, {0, 31}, {0, 23}, {0, 59}};
					bool valid = tokens.size() == 8;
					for (size_t index = 0; valid && index < tokens.size(); ++index) {
						valid = LegacyIniConfigCodec::TryParseInt(
							tokens[index], ranges[index].first, ranges[index].second, values[index]);
					}
					if (!valid) {
						RecordConfigDiagnostic(
							LegacyIniConfigCodec::DiagnosticSeverity::Fatal,
							lineNumber, section, key,
							"expected exactly 8 valid comma-separated task fields");
					}
					else {
						AutomatedTask task;
						task.configIndex = values[0];
						task.worldIndex = values[1];
						task.backupType = values[2];
						task.intervalMinutes = values[3];
						task.schedMonth = values[4];
						task.schedDay = values[5];
						task.schedHour = values[6];
						task.schedMinute = values[7];
						spCur->tasks.push_back(std::move(task));
					}
				}
				else if (key == L"ZipLevel") readInt(spCur->zipLevel, 0, 22, true);
				else if (key == L"KeepCount") readInt(spCur->keepCount, 0, 100000, true);
				else if (key == L"CpuThreads") readInt(spCur->cpuThreads, 0, 1024, true);
				else if (key == L"UseLowPriority") spCur->useLowPriority = (val != L"0");
				else if (key == L"BackupOnStart") spCur->backupOnGameStart = (val != L"0");
				else if (key == L"BlacklistItem") spCur->blacklist.push_back(val);
				// 新版统一任务系统
				else if (key == L"UnifiedTask") {
					if (hasAuthoritativeSpecialTasks) continue;
					const auto tokens = LegacyIniConfigCodec::Split(val, L',');
					int values[12]{};
					const size_t numericIndices[12] = {0, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14};
					const pair<int, int> ranges[12] = {
						{0, (numeric_limits<int>::max)()}, {0, 2}, {0, 1}, {0, 2},
						{0, 1}, {-1, (numeric_limits<int>::max)()}, {-1, (numeric_limits<int>::max)()},
						{1, 525600}, {0, 12}, {0, 31}, {0, 23}, {0, 59}};
					bool valid = tokens.size() == 15;
					for (size_t index = 0; valid && index < 12; ++index) {
						valid = LegacyIniConfigCodec::TryParseInt(
							tokens[numericIndices[index]], ranges[index].first,
							ranges[index].second, values[index]);
					}
					if (!valid) {
						RecordConfigDiagnostic(
							LegacyIniConfigCodec::DiagnosticSeverity::Fatal,
							lineNumber, section, key,
							"expected exactly 15 valid comma-separated task fields");
					}
					else {
						UnifiedTaskV2 task;
						task.id = values[0];
						task.name = wstring_to_utf8(tokens[1]);
						task.type = static_cast<TaskTypeV2>(values[1]);
						task.executionMode = static_cast<TaskExecMode>(values[2]);
						task.triggerMode = static_cast<TaskTrigger>(values[3]);
						task.enabled = values[4] != 0;
						task.configIndex = values[5];
						task.worldIndex = values[6];
						task.command = tokens[8];
						task.workingDirectory = tokens[9];
						task.intervalMinutes = values[7];
						task.schedMonth = values[8];
						task.schedDay = values[9];
						task.schedHour = values[10];
						task.schedMinute = values[11];
						spCur->unifiedTasks.push_back(std::move(task));
					}
				}
				// 服务模式配置
				else if (key == L"UseServiceMode") spCur->useServiceMode = (val != L"0");
				else if (key == L"ServiceName") spCur->serviceConfig.serviceName = val;
				else if (key == L"ServiceDisplayName") spCur->serviceConfig.serviceDisplayName = val;
				else if (key == L"ServiceAutoStart") spCur->serviceConfig.startWithSystem = (val != L"0");
				else if (key == L"ServiceDelayedStart") spCur->serviceConfig.delayedStart = (val != L"0");
			}
			else if (section == L"General") { // Inside [General] section
				if (key == L"CurrentConfig") {
					readInt(g_appState.currentConfigIndex, 1, (numeric_limits<int>::max)(), false);
				}
				else if (key == L"NextConfigId") {
					readInt(nextConfigId, 2, (numeric_limits<int>::max)(), false);
					int maxId = 0;
					for (auto& kv : g_appState.configs) if (kv.first > maxId) maxId = kv.first;
					for (auto& kv : g_appState.specialConfigs) if (kv.first > maxId) maxId = kv.first;
					if (nextConfigId <= maxId) nextConfigId = maxId + 1;
				}
				else if (key == L"Language") {
					if (val.size() >= 3 && val[2] == L'-')
						val[2] = L'_';
					if (val.size() >= 2) {
						SetLanguage(wstring_to_utf8(val));
					}
					else {
						RecordConfigDiagnostic(
							LegacyIniConfigCodec::DiagnosticSeverity::Warning,
							lineNumber, section, key, "language identifier is too short");
					}
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
				else if (key == L"AutoStartKnotLinkServer") {
					g_autoStartKnotLinkServer = (val != L"0");
				}
				else if (key == L"RunOnStartup") {
					g_RunOnStartup = (val != L"0");
				}
				else if (key == L"IsSafeDelete") {
					isSafeDelete = (val != L"0");
				}
				else if (key == L"AutoBackupInterval") {
					readInt(last_interval, 1, 525600, true);
				}
				else if (key == L"StopAutoBackupOnExit") {
					g_StopAutoBackupOnExit = (val != L"0");
				}
				else if (key == L"SilentStartupToTray") {
					g_SilentStartupToTray = (val != L"0");
				}
				else if (key == L"RestoreWhitelistItem") {
					restoreWhitelist.push_back(val);
				}
				else if (key == L"WindowWidth") {
					readInt(g_windowWidth, 11, 32768, false);
				}
				else if (key == L"WindowHeight") {
					readInt(g_windowHeight, 11, 32768, false);
				}
				else if (key == L"UIScale") {
					configuredUiScaleFound = readFloat(g_uiScale, 0.25f, 4.0f, false);
				}
				else if (key == L"UIScaleMode") {
					configuredUiScaleV2 = (val == L"UserMultiplierV2");
				}
				else if (key == L"AppearanceSchema") {
					int parsed = 1;
					if (readInt(parsed, 1, 100, false)) configuredAppearanceSchema = parsed;
				}
				else if (key == L"Theme") {
					int parsed = 0;
					if (readInt(parsed, -1, 32, false)) configuredGlobalTheme = parsed;
				}
				else if (key == L"ThemeFallback") {
					int parsed = 0;
					if (readInt(parsed, -1, 32, false)) configuredThemeFallback = parsed;
				}
				else if (key == L"Font") {
					configuredGlobalFont = val;
				}
				else if (key == L"AutoScanForWorlds") {
					g_AutoScanForWorlds = (val != L"0");
				}
				else if (key == L"HotkeyBackup") {
					readInt(g_hotKeyBackupId, 0, 100000, false);
				}
				else if (key == L"HotkeyRestore") {
					readInt(g_hotKeyRestoreId, 0, 100000, false);
				}
				else if (key == L"LogFileLevel") {
					configuredLogFileLevel = val;
				}
				else if (key == L"LogViewLevel") {
					configuredLogViewLevel = val;
				}
				else if (key == L"LogViewAutoTail") {
					g_logViewAutoTail = (val != L"0");
				}
				else if (key == L"LogViewShowTime") {
					g_logViewShowTime = (val != L"0");
				}
				else if (key == L"LogViewShowCategory") {
					g_logViewShowCategory = (val != L"0");
				}
				else if (key == L"AutoLog") {
					legacyAutoLog = (val != L"0");
				}
				else if (key == L"CoreValidationPending") {
					g_CoreValidationPending.store(val != L"0");
				}
				else if (key == L"CoreValidationPassed") {
					g_CoreValidationPassed.store(val != L"0");
				}
				else if (key == L"CloseAction") {
					readInt(g_closeAction, 0, 2, false);
				}
				else if (key == L"RememberCloseAction") {
					g_rememberCloseAction = (val != L"0");
				}
			}
		}
	}
	const optional<string> configuredValue = configuredLogFileLevel
		? optional<string>(wstring_to_utf8(*configuredLogFileLevel)) : nullopt;
	const auto logLevelResolution = minebackup::logging::ResolveFileLevel(
		configuredValue
			? optional<string_view>(*configuredValue) : nullopt,
		legacyAutoLog);
	g_logFileLevel = logLevelResolution.level;
	if (logLevelResolution.invalidConfiguredValue) {
		MB_LOG_WARNING(minebackup::logging::LogCategory::Migration,
			"logging.config.invalid_level",
			"Invalid LogFileLevel '{}'; using info.", *configuredValue);
	}
	else if (logLevelResolution.usedLegacyAutoLog) {
		MB_LOG_INFO(minebackup::logging::LogCategory::Migration,
			"logging.config.legacy_auto_log",
			"Migrated legacy AutoLog={} to LogFileLevel={}.",
			*legacyAutoLog ? 1 : 0, minebackup::logging::ToString(g_logFileLevel));
	}
	if (configuredLogViewLevel) {
		bool validLogViewLevel = false;
		g_logViewLevel = minebackup::logging::ParseLogLevel(
			wstring_to_utf8(*configuredLogViewLevel), &validLogViewLevel);
		if (!validLogViewLevel) {
			MB_LOG_WARNING(minebackup::logging::LogCategory::Migration,
				"logging.config.invalid_view_level",
				"Invalid LogViewLevel '{}'; using info.",
				wstring_to_utf8(*configuredLogViewLevel));
		}
	}
	minebackup::logging::SetFileLevel(g_logFileLevel);
	set<wstring> usedConfigIds;
	for (auto& kv : g_appState.configs) {
		Config& cfg = kv.second;
		cfg.zipLevel = NormalizeCompressionLevel(cfg.zipMethod, cfg.zipLevel);
		if (cfg.cloudSyncMode < static_cast<int>(CloudSyncMode::HistoryOnly)
			|| cfg.cloudSyncMode > static_cast<int>(CloudSyncMode::HistoryAndBackups)) {
			cfg.cloudSyncMode = static_cast<int>(CloudSyncMode::HistoryOnly);
		}
		if (cfg.cloudTimeoutSeconds <= 0) cfg.cloudTimeoutSeconds = 600;
		if (cfg.cloudRetryCount < 0) cfg.cloudRetryCount = 0;
		if (cfg.configId.empty()) {
			cfg.configId = MigrationCoordinator::GenerateLegacyConfigId(cfg, kv.first);
			cfg.legacyConfigIdGenerated = true;
		}
		else {
			cfg.configId = FolderRewindFormat::EnsureConfigId(cfg.configId);
		}

		wstring identity = cfg.configId;
		transform(identity.begin(), identity.end(), identity.begin(), ::towlower);
		if (!usedConfigIds.insert(identity).second) {
			do {
				cfg.configId = FolderRewindFormat::GenerateGuidString();
				identity = cfg.configId;
				transform(identity.begin(), identity.end(), identity.begin(), ::towlower);
			} while (!usedConfigIds.insert(identity).second);
			cfg.legacyConfigIdGenerated = true;
			MigrationUnitResult collision;
			collision.unitId = L"startup:config-id-collision:" + to_wstring(kv.first);
			collision.status = MigrationStatus::Succeeded;
			collision.message = L"A duplicate ConfigId was replaced; the first configuration retained its identity.";
			collision.migratedItems = 1;
			MigrationCoordinator::RecordUnit(collision);
		}
	}

	set<wstring> usedSpecialConfigIds;
	for (auto& kv : g_appState.specialConfigs) {
		SpecialConfig& spCfg = kv.second;
		if (spCfg.specialConfigId.empty()) {
			spCfg.specialConfigId = FolderRewindFormat::GenerateGuidString();
			spCfg.legacySpecialConfigIdGenerated = true;
		}
		wstring identity = spCfg.specialConfigId;
		transform(identity.begin(), identity.end(), identity.begin(), ::towlower);
		if (!usedSpecialConfigIds.insert(identity).second) {
			do {
				spCfg.specialConfigId = FolderRewindFormat::GenerateGuidString();
				identity = spCfg.specialConfigId;
				transform(identity.begin(), identity.end(), identity.begin(), ::towlower);
			} while (!usedSpecialConfigIds.insert(identity).second);
			spCfg.legacySpecialConfigIdGenerated = true;
		}
		if (spCfg.zipLevel < 1) spCfg.zipLevel = 1;
		if (spCfg.zipLevel > 22) spCfg.zipLevel = 22;
	}

	if (specialTasksExistsError) {
		RecordSpecialTaskDiagnostic({
			SpecialTaskStorage::DiagnosticSeverity::Fatal,
			"tasks.io.stat_failed", {}, {}, specialTasksExistsError.message()});
	}
	else {
		auto taskLoad = SpecialTaskStorage::Load(specialTasksPath);
		for (const auto& diagnostic : taskLoad.diagnostics) {
			RecordSpecialTaskDiagnostic(diagnostic);
		}
		if (taskLoad.status == SpecialTaskStorage::LoadStatus::Loaded) {
			vector<SpecialTaskStorage::Diagnostic> diagnostics;
			SpecialTaskStorage::ApplyAndValidate(
				taskLoad.document, g_appState.configs, g_appState.specialConfigs, diagnostics);
			for (const auto& diagnostic : diagnostics) RecordSpecialTaskDiagnostic(diagnostic);
		}
		else if (taskLoad.status == SpecialTaskStorage::LoadStatus::Missing
			&& !LastConfigLoadHasFatalDiagnostics()) {
			auto migration = SpecialTaskStorage::MigrateLegacy(
				g_appState.configs, g_appState.specialConfigs);
			for (const auto& diagnostic : migration.diagnostics) {
				RecordSpecialTaskDiagnostic(diagnostic);
			}
			if (migration.success) {
				wstring writeError;
				if (!SpecialTaskStorage::Save(specialTasksPath, migration.document, writeError)) {
					RecordSpecialTaskDiagnostic({
						SpecialTaskStorage::DiagnosticSeverity::Fatal,
						"tasks.migration.write_failed", {}, {},
						wstring_to_utf8(writeError)});
				}
				else {
					vector<SpecialTaskStorage::Diagnostic> diagnostics;
					SpecialTaskStorage::ApplyAndValidate(
						migration.document, g_appState.configs,
						g_appState.specialConfigs, diagnostics);
					for (const auto& diagnostic : diagnostics) RecordSpecialTaskDiagnostic(diagnostic);
					MB_LOG_INFO(minebackup::logging::LogCategory::Migration,
						"tasks.migration.completed",
						"Migrated legacy special tasks to schema version {}.",
						SpecialTaskDocument::SchemaVersion);
				}
			}
		}
	}

	const auto executionPolicy = NormalizeSpecialConfigExecutionPolicy(g_appState.specialConfigs);
	if (executionPolicy.autoExecuteIndex) {
		g_appState.specialConfigMode = true;
		g_appState.currentConfigIndex = *executionPolicy.autoExecuteIndex;
	}
	if (executionPolicy.disabledDuplicateAutoExecute > 0
		|| executionPolicy.disabledDuplicateRunOnStartup > 0) {
		MigrationUnitResult normalized;
		normalized.unitId = L"startup:special-config-exclusivity";
		normalized.status = MigrationStatus::Succeeded;
		normalized.message = L"Duplicate special startup selections were disabled deterministically; the lowest configuration index was retained.";
		normalized.migratedItems = executionPolicy.disabledDuplicateAutoExecute
			+ executionPolicy.disabledDuplicateRunOnStartup;
		MigrationCoordinator::RecordUnit(normalized);
	}

	auto validFontPath = [](const wstring& value) {
		return !value.empty() && value.size() >= 3 && filesystem::exists(value);
	};

	if (configuredGlobalTheme && IsValidThemeId(*configuredGlobalTheme)) {
		g_theme = *configuredGlobalTheme;
	}
	else {
		auto normal = g_appState.configs.find(g_appState.currentConfigIndex);
		auto special = g_appState.specialConfigs.find(g_appState.currentConfigIndex);
		if (normal != g_appState.configs.end() && IsValidThemeId(normal->second.theme)) {
			g_theme = normal->second.theme;
		}
		else if (special != g_appState.specialConfigs.end() && IsValidThemeId(special->second.theme)) {
			g_theme = special->second.theme;
		}
	}
	if (configuredThemeFallback
		&& *configuredThemeFallback >= static_cast<int>(ThemeId::ImGuiDark)
		&& *configuredThemeFallback <= static_cast<int>(ThemeId::NordDark)) {
		g_lastValidTheme = *configuredThemeFallback;
	}
	else if (g_theme != static_cast<int>(ThemeId::Custom)) {
		g_lastValidTheme = g_theme;
	}

	if (configuredGlobalFont && validFontPath(*configuredGlobalFont)) {
		Fontss = *configuredGlobalFont;
	}
	else {
		auto normal = g_appState.configs.find(g_appState.currentConfigIndex);
		if (normal != g_appState.configs.end() && validFontPath(normal->second.fontPath)) {
			Fontss = normal->second.fontPath;
		}
		if (Fontss.empty()) {
			for (const auto& [index, config] : g_appState.configs) {
				(void)index;
				if (validFontPath(config.fontPath)) {
					Fontss = config.fontPath;
					break;
				}
			}
		}
	}
	if (Fontss.empty()) {
		if (configuredGlobalFont && !configuredGlobalFont->empty()) {
			MessageBoxWin(L("WARNING_TITLE"), L("INVALID_FONT_PATH"), 1);
		}
		Fontss = GetDefaultFontPath();
	}

	g_appearanceSchema = configuredAppearanceSchema.value_or(1);
	g_uiScaleV2 = configuredUiScaleV2 || !configuredUiScaleFound;
	g_uiScaleMigrationPending = configuredUiScaleFound && !configuredUiScaleV2;
	g_uiScale = (std::clamp)(g_uiScale, 0.75f, 2.5f);
}

void FinalizeUiScaleMigration(float primaryDpiScale) {
	const UiScaleMigrationResult migration = MigrateUiScale(
		g_uiScale, primaryDpiScale, g_uiScaleMigrationPending);
	g_uiScale = migration.scale;
	g_uiScaleMigrationPending = false;
	g_uiScaleV2 = true;
	g_appearanceSchema = 1;
}

bool SaveConfigs() {
	return SaveConfigs(GetAppPaths().ConfigFile());
}

bool SaveConfigs(const filesystem::path& filename) {
	lock_guard<mutex> lock(g_appState.configsMutex);
	const filesystem::path target(filename);
	for (auto& [index, config] : g_appState.configs) {
		(void)index;
		config.configId = FolderRewindFormat::EnsureConfigId(config.configId);
	}
	for (auto& [index, special] : g_appState.specialConfigs) {
		(void)index;
		special.specialConfigId = FolderRewindFormat::EnsureConfigId(special.specialConfigId);
	}
	const auto taskDocument = SpecialTaskStorage::BuildDocument(g_appState.specialConfigs);
	wstring taskWriteError;
	if (!SpecialTaskStorage::Save(
			SpecialTasksPathForConfig(target), taskDocument, taskWriteError)) {
		MessageBoxWin(L("ERROR_CONFIG_WRITE_FAIL"), L("ERROR_TITLE"), 2);
		return false;
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
	buffer << L"AutoStartKnotLinkServer=" << (g_autoStartKnotLinkServer ? 1 : 0) << L"\n";
	buffer << L"RunOnStartup=" << (g_RunOnStartup ? 1 : 0) << L"\n";
	buffer << L"IsSafeDelete=" << (isSafeDelete ? 1 : 0) << L"\n";
	buffer << L"AutoBackupInterval=" << last_interval << L"\n";
	buffer << L"StopAutoBackupOnExit=" << (g_StopAutoBackupOnExit ? 1 : 0) << L"\n";
	buffer << L"SilentStartupToTray=" << (g_SilentStartupToTray ? 1 : 0) << L"\n";
	buffer << L"AutoScanForWorlds=" << (g_AutoScanForWorlds ? 1 : 0) << L"\n";
	buffer << L"WindowWidth=" << g_windowWidth << L"\n";
	buffer << L"WindowHeight=" << g_windowHeight << L"\n";
	buffer << L"UIScale=" << g_uiScale << L"\n";
	buffer << L"UIScaleMode=UserMultiplierV2\n";
	buffer << L"AppearanceSchema=" << g_appearanceSchema << L"\n";
	buffer << L"Theme=" << g_theme << L"\n";
	buffer << L"ThemeFallback=" << g_lastValidTheme << L"\n";
	buffer << L"Font=" << Fontss << L"\n";
	buffer << L"HotkeyBackup=" << g_hotKeyBackupId << L"\n";
	buffer << L"HotkeyRestore=" << g_hotKeyRestoreId << L"\n";
	buffer << L"LogFileLevel="
		<< utf8_to_wstring(minebackup::logging::ToString(g_logFileLevel)) << L"\n";
	buffer << L"LogViewLevel="
		<< utf8_to_wstring(minebackup::logging::ToString(g_logViewLevel)) << L"\n";
	buffer << L"LogViewAutoTail=" << (g_logViewAutoTail ? 1 : 0) << L"\n";
	buffer << L"LogViewShowTime=" << (g_logViewShowTime ? 1 : 0) << L"\n";
	buffer << L"LogViewShowCategory=" << (g_logViewShowCategory ? 1 : 0) << L"\n";
	buffer << L"CoreValidationPending=" << (g_CoreValidationPending.load() ? 1 : 0) << L"\n";
	buffer << L"CoreValidationPassed=" << (g_CoreValidationPassed.load() ? 1 : 0) << L"\n";
	buffer << L"CloseAction=" << g_closeAction << L"\n";
	buffer << L"RememberCloseAction=" << (g_rememberCloseAction ? 1 : 0) << L"\n";
	for (const auto& item : restoreWhitelist) {
		buffer << L"RestoreWhitelistItem=" << item << L"\n";
	}
	buffer << L"\n";

	for (auto& kv : g_appState.configs) {
		int idx = kv.first;
		Config& c = kv.second;
		buffer << L"[Config" << idx << L"]\n";
		buffer << L"ConfigName=" << utf8_to_wstring(c.name) << L"\n";
		c.configId = FolderRewindFormat::EnsureConfigId(c.configId);
		buffer << L"ConfigId=" << c.configId << L"\n";
		buffer << L"PendingLocalBinding=" << (c.pendingLocalBinding ? 1 : 0) << L"\n";
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
		buffer << L"SkipIfUnchanged=" << (c.skipIfUnchanged ? 1 : 0) << L"\n";
		buffer << L"MaxSmartBackups=" << c.maxSmartBackupsPerFull << L"\n";
		buffer << L"BackupOnStart=" << (c.backupOnGameStart ? 1 : 0) << L"\n";
		buffer << L"CloudSyncEnabled=" << (c.cloudSyncEnabled ? 1 : 0) << L"\n";
		buffer << L"RclonePath=" << c.rclonePath << L"\n";
		buffer << L"RcloneRemotePath=" << c.rcloneRemotePath << L"\n";
		buffer << L"CloudSyncMode=" << c.cloudSyncMode << L"\n";
		buffer << L"CloudWorkingDirectory=" << c.cloudWorkingDirectory << L"\n";
		buffer << L"CloudTimeoutSeconds=" << c.cloudTimeoutSeconds << L"\n";
		buffer << L"CloudRetryCount=" << c.cloudRetryCount << L"\n";
		buffer << L"CloudSyncHistoryAfterUpload=" << (c.cloudSyncHistoryAfterUpload ? 1 : 0) << L"\n";
		buffer << L"CloudAutoDownloadBeforeRestore=" << (c.cloudAutoDownloadBeforeRestore ? 1 : 0) << L"\n";
		buffer << L"CloudLastRunUtc=" << c.cloudLastRunUtc << L"\n";
		buffer << L"CloudLastExitCode=" << c.cloudLastExitCode << L"\n";
		buffer << L"CloudLastErrorMessage=" << c.cloudLastErrorMessage << L"\n";
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
		sc.specialConfigId = FolderRewindFormat::EnsureConfigId(sc.specialConfigId);
		buffer << L"SpecialConfigId=" << sc.specialConfigId << L"\n";
		buffer << L"AutoExecute=" << (sc.autoExecute ? 1 : 0) << L"\n";
		buffer << L"ExitAfter=" << (sc.exitAfterExecution ? 1 : 0) << L"\n";
		buffer << L"HideWindow=" << (sc.hideWindow ? 1 : 0) << L"\n";
		buffer << L"RunOnStartup=" << (sc.runOnStartup ? 1 : 0) << L"\n";
		buffer << L"ZipLevel=" << sc.zipLevel << L"\n";
		buffer << L"KeepCount=" << sc.keepCount << L"\n";
		buffer << L"CpuThreads=" << sc.cpuThreads << L"\n";
		buffer << L"UseLowPriority=" << (sc.useLowPriority ? 1 : 0) << L"\n";
		buffer << L"BackupOnStart=" << (sc.backupOnGameStart ? 1 : 0) << L"\n";
		// 1.16 preserves these local, read-only values solely so a custom-named
		// legacy service remains discoverable until the user removes it. They are
		// excluded from portable configuration and are removed from the model in 1.17.
		buffer << L"UseServiceMode=" << (sc.useServiceMode ? 1 : 0) << L"\n";
		buffer << L"ServiceName=" << sc.serviceConfig.serviceName << L"\n";
		buffer << L"ServiceDisplayName=" << sc.serviceConfig.serviceDisplayName << L"\n";
		buffer << L"ServiceAutoStart=" << (sc.serviceConfig.startWithSystem ? 1 : 0) << L"\n";
		buffer << L"ServiceDelayedStart=" << (sc.serviceConfig.delayedStart ? 1 : 0) << L"\n";
		for (const auto& item : sc.blacklist) {
			buffer << L"BlacklistItem=" << item << L"\n";
		}
		buffer << L"\n\n";
	}

	const string utf8 = wstring_to_utf8(buffer.str());
	if (!AtomicFileWriter::WriteText(target, utf8).success) {
		MessageBoxWin(L("ERROR_CONFIG_WRITE_FAIL"), L("ERROR_TITLE"), 2);
		return false;
	}
	return true;
}

// 在 LoadConfigs/SaveConfigs/CheckForConfigConflicts 等函数关键处调用日志接口
// 例如：

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
