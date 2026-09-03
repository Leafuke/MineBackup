#include "ConfigManager.h"
#include "AppState.h"
#include "UIHelpers.h"
#include "AppPaths.h"
#include "AtomicFileWriter.h"
#include "ConfigFactory.h"
#include "FolderRewindFormat.h"
#include "JobDocument.h"
#include "MigrationCoordinator.h"
#include "Globals.h"
#include "Logging.h"
#include "LegacyIniConfigCodec.h"
#include "KnownUserFolders.h"
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

filesystem::path RecommendedBackupRoot() {
	try {
		return KnownUserFolders::Resolver{}.ResolveRecommendedBackupRoot(GetAppPaths());
	}
	catch (...) {
		// 独立配置解析测试可能尚未初始化 AppPaths；禁止用当前工作目录作为隐式回退。
		return {};
	}
}

filesystem::path JobsPathForConfig(const filesystem::path& configFile) {
	return configFile.parent_path() / L"jobs.json";
}

string ReadIgnoredSpecialSections(const filesystem::path& configFile) {
	ifstream input(configFile, ios::binary);
	if (!input.is_open()) return {};
	string output;
	bool capture = false;
	for (string line; getline(input, line);) {
		string normalized = line;
		if (!normalized.empty() && normalized.back() == '\r') normalized.pop_back();
		if (normalized.size() >= 2 && normalized.front() == '[' && normalized.back() == ']') {
			capture = normalized.rfind("[SpCfg", 0) == 0;
		}
		if (capture) output += line + "\n";
	}
	return output;
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

} // namespace

filesystem::path GetEffectiveDefaultBackupRoot() {
	const filesystem::path configured(g_defaultBackupRootPath);
	if (!configured.empty() && configured.is_absolute()) {
		return configured.lexically_normal();
	}

	const filesystem::path recommended = RecommendedBackupRoot();
	if (!recommended.empty() && recommended.is_absolute()) {
		return recommended.lexically_normal();
	}
	return {};
}

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
	return RecommendedConfigBackupBlacklist();
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

int CreateNewNormalConfig(const string& name_hint) {
	ConfigDraft draft;
	draft.name = name_hint;
	const auto resolved = ResolveUniqueConfigDrafts(
		{draft}, GetEffectiveDefaultBackupRoot(), g_appState.configs);
	if (!resolved.empty()) draft = resolved.front();

	const int newId = AllocateNormalConfigIndex();
	Config new_cfg = BuildRecommendedConfig(draft, {});
	new_cfg.configId = FolderRewindFormat::GenerateGuidString();
	g_appState.configs[newId] = new_cfg;
	return newId;
}

NormalConfigIndexAllocatorState SnapshotNormalConfigIndexAllocator() {
	return {nextConfigId};
}

void RestoreNormalConfigIndexAllocator(NormalConfigIndexAllocatorState state) {
	nextConfigId = (max)(state.nextIndex, 2);
}

int AllocateNormalConfigIndex() {
	if (nextConfigId == (numeric_limits<int>::max)()
		&& g_appState.configs.contains(nextConfigId)) {
		throw overflow_error("No normal configuration index remains");
	}
	const int allocated = nextConfigId;
	if (nextConfigId < (numeric_limits<int>::max)()) ++nextConfigId;
	return allocated;
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
	nextConfigId = 2;
	g_appState.configs.clear();
	g_appState.jobs = JobDocument{};
	g_theme = static_cast<int>(ThemeId::NordLight);
	g_lastValidTheme = static_cast<int>(ThemeId::NordLight);
	Fontss.clear();
	g_appearanceSchema = 1;
	g_uiScaleV2 = true;
	g_uiScaleMigrationPending = false;
	restoreWhitelist.clear();
	g_defaultBackupRootPath = RecommendedBackupRoot().wstring();
	g_logFileLevel = minebackup::logging::LogFileLevel::Info;
	g_logViewLevel = minebackup::logging::LogLevel::Info;
	g_logViewAutoTail = true;
	g_logViewShowTime = false;
	g_logViewShowCategory = false;
	optional<wstring> configuredLogFileLevel;
	optional<wstring> configuredLogViewLevel;
	optional<bool> legacyAutoLog;
	bool configuredRestoreWhitelist = false;
	ifstream in(filename, ios::binary);
	if (!in.is_open()) {
		EnsureDefaultRestoreWhitelist();
		Fontss = GetDefaultFontPath();
		minebackup::logging::SetFileLevel(g_logFileLevel);
		return;
	}
	optional<int> configuredGlobalTheme;
	optional<int> configuredThemeFallback;
	optional<int> configuredSystemThemeLight;
	optional<int> configuredSystemThemeDark;
	optional<wstring> configuredGlobalFont;
	optional<int> configuredAppearanceSchema;
	bool configuredUiScaleV2 = false;
	bool configuredUiScaleFound = false;
	string line1;
	wstring line, section;
	// cur作为一个指针，指向 g_appState.configs 这个全局 map<int, Config> 中的元素 Config
	Config* cur = nullptr;
	size_t lineNumber = 0;

	while (getline(in, line1)) {
		++lineNumber;
		line = utf8_to_wstring(line1);
		if (line.empty() || line.front() == L'#') continue;
		if (line.front() == L'[' && line.back() == L']') {
			section = line.substr(1, line.size() - 2);
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
			else if (section == L"General") { // Inside [General] section
				if (key == L"CurrentConfig") {
					readInt(g_appState.currentConfigIndex, 1, (numeric_limits<int>::max)(), false);
				}
				else if (key == L"NextConfigId") {
					readInt(nextConfigId, 2, (numeric_limits<int>::max)(), false);
					int maxId = 0;
					for (auto& kv : g_appState.configs) if (kv.first > maxId) maxId = kv.first;
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
					configuredRestoreWhitelist = true;
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
				else if (key == L"SystemThemeLight") {
					int parsed = 0;
					if (readInt(parsed, -1, 32, false)) configuredSystemThemeLight = parsed;
				}
				else if (key == L"SystemThemeDark") {
					int parsed = 0;
					if (readInt(parsed, -1, 32, false)) configuredSystemThemeDark = parsed;
				}
				else if (key == L"Font") {
					configuredGlobalFont = val;
				}
				else if (key == L"AutoScanForWorlds") {
					// 仅保留旧字段的兼容读写；世界发现必须由显式发现流程触发。
					g_AutoScanForWorlds = (val != L"0");
				}
				else if (key == L"DefaultBackupRootPath") {
					g_defaultBackupRootPath = val;
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
	if (!configuredRestoreWhitelist) EnsureDefaultRestoreWhitelist();
	set<wstring> usedConfigIds;
	if (!g_appState.configs.empty()) {
		const int maximumIndex = g_appState.configs.rbegin()->first;
		if (nextConfigId <= maximumIndex) {
			nextConfigId = maximumIndex == (numeric_limits<int>::max)()
				? maximumIndex : maximumIndex + 1;
		}
	}
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

	const auto jobs = JobStorage::Load(JobsPathForConfig(filename));
	if (jobs.status == JobStorage::LoadStatus::Loaded) {
		g_appState.jobs = jobs.document;
	}
	else if (jobs.status != JobStorage::LoadStatus::Missing) {
		for (const auto& diagnostic : jobs.diagnostics) {
			MB_LOG_ERROR(minebackup::logging::LogCategory::Application,
				diagnostic.eventId, "{}", diagnostic.detail);
		}
	}

	auto validFontPath = [](const wstring& value) {
		return !value.empty() && value.size() >= 3 && filesystem::exists(value);
	};

	if (configuredGlobalTheme && IsValidThemeId(*configuredGlobalTheme)) {
		g_theme = *configuredGlobalTheme;
	}
	else {
		auto normal = g_appState.configs.find(g_appState.currentConfigIndex);
		if (normal != g_appState.configs.end() && IsValidThemeId(normal->second.theme)) {
			g_theme = normal->second.theme;
		}
	}
	if (configuredThemeFallback
		&& *configuredThemeFallback >= static_cast<int>(ThemeId::ImGuiDark)
		&& *configuredThemeFallback <= static_cast<int>(ThemeId::SystemAuto)) {
		g_lastValidTheme = *configuredThemeFallback;
	}
	else if (g_theme != static_cast<int>(ThemeId::Custom)) {
		g_lastValidTheme = g_theme;
	}

	if (configuredSystemThemeLight
		&& IsValidThemeId(*configuredSystemThemeLight)
		&& *configuredSystemThemeLight != static_cast<int>(ThemeId::SystemAuto)) {
		g_systemThemeLight = *configuredSystemThemeLight;
	}
	else {
		g_systemThemeLight = static_cast<int>(ThemeId::WindowsLight);
	}
	if (configuredSystemThemeDark
		&& IsValidThemeId(*configuredSystemThemeDark)
		&& *configuredSystemThemeDark != static_cast<int>(ThemeId::SystemAuto)) {
		g_systemThemeDark = *configuredSystemThemeDark;
	}
	else {
		g_systemThemeDark = static_cast<int>(ThemeId::WindowsDark);
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
	// 布尔契约：只要逻辑 commit 已发生（含“已替换但持久化未确认”）即返回 true。
	return SaveConfigsDetailed().Committed();
}

bool SaveConfigs(const filesystem::path& filename) {
	return SaveConfigsDetailed(filename).Committed();
}

ConfigSaveResult SaveConfigsDetailed() {
	return SaveConfigsDetailed(GetAppPaths().ConfigFile());
}

ConfigSaveResult SaveConfigsDetailed(const filesystem::path& filename) {
	// 内部全程使用 error_code 明确状态的实现，不在 replacement 之后抛出
	// 无法分类的异常；文件系统错误都转换为对应的 ConfigSaveState。
	lock_guard<mutex> lock(g_appState.configsMutex);
	ConfigSaveResult result;
	const filesystem::path target(filename);
	for (auto& [index, config] : g_appState.configs) {
		(void)index;
		config.configId = FolderRewindFormat::EnsureConfigId(config.configId);
	}
	wstring jobsWriteError;
	if (!JobStorage::Save(JobsPathForConfig(target), g_appState.jobs, jobsWriteError)) {
		MB_LOG_ERROR(minebackup::logging::LogCategory::Application,
			"jobs.write_failed", "{}", wstring_to_utf8(jobsWriteError));
		MessageBoxWin(L("ERROR_CONFIG_WRITE_FAIL"), L("ERROR_TITLE"), 2);
		result.detail = jobsWriteError;
		return result; // NotCommitted
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
	buffer << L"DefaultBackupRootPath=" << g_defaultBackupRootPath << L"\n";
	buffer << L"WindowWidth=" << g_windowWidth << L"\n";
	buffer << L"WindowHeight=" << g_windowHeight << L"\n";
	buffer << L"UIScale=" << g_uiScale << L"\n";
	buffer << L"UIScaleMode=UserMultiplierV2\n";
	buffer << L"AppearanceSchema=" << g_appearanceSchema << L"\n";
	buffer << L"Theme=" << g_theme << L"\n";
	buffer << L"ThemeFallback=" << g_lastValidTheme << L"\n";
	buffer << L"SystemThemeLight=" << g_systemThemeLight << L"\n";
	buffer << L"SystemThemeDark=" << g_systemThemeDark << L"\n";
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

	string utf8 = wstring_to_utf8(buffer.str());
	const string ignoredSpecial = ReadIgnoredSpecialSections(target);
	if (!ignoredSpecial.empty()) utf8 += "\n" + ignoredSpecial;
	const auto write = AtomicFileWriter::WriteText(target, utf8);
	if (write.commitState == AtomicFileWriter::WriteCommitState::NotReplaced) {
		// config.ini 从未被替换：这次保存逻辑上什么都没有发生。
		MB_LOG_ERROR(minebackup::logging::LogCategory::Application,
			"config.write_not_committed", "{}", wstring_to_utf8(write.error));
		MessageBoxWin(L("ERROR_CONFIG_WRITE_FAIL"), L("ERROR_TITLE"), 2);
		result.detail = write.error;
		return result; // NotCommitted
	}
	if (!write.IsDurable()) {
		// config.ini 已替换（commit point 已越过），仅目录同步未确认。
		// 不显示误导性的“写入失败”，不尝试用 .bak 反向覆盖，
		// 更不允许业务层按“未提交”回滚内存状态。
		result.state = ConfigSaveState::CommittedNotDurable;
		result.detail = write.error;
		MB_LOG_WARNING(minebackup::logging::LogCategory::Application,
			"config.write_committed_not_durable", "{}", wstring_to_utf8(write.error));
		return result;
	}
	result.state = ConfigSaveState::CommittedDurably;
	return result;
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
