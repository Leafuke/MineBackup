#include "ConfigIniCodec.h"
#include "ProfileConfigRepository.h"
#include "ProfileTransaction.h"
#include "CompressionPolicy.h"
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
    FlushUiConfigDraft();
    ConfigDraft draft;
    draft.name = name_hint;
    const auto snapshot = SnapshotConfigState();
    const auto resolved = ResolveUniqueConfigDrafts({draft}, GetEffectiveDefaultBackupRoot(), snapshot.configs);
    if (!resolved.empty()) draft = resolved.front();
    Config config = BuildRecommendedConfig(draft, {});
    config.configId = FolderRewindFormat::GenerateGuidString();
    int index;
    {
        lock_guard lock(g_appState.configsMutex);
        index = AllocateNormalConfigIndex();
        g_appState.configs.emplace(index, config);
    }
    ObserveUiConfigInserted(index, config);
    return index;
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
    lock_guard lock(g_appState.configsMutex);
    auto it = g_appState.configs.find(configIndex);
    if (it == g_appState.configs.end()) return;
    it->second.configId = FolderRewindFormat::GenerateGuidString();
}

void EnsureConfigIds() {
    lock_guard lock(g_appState.configsMutex);
	for (auto& kv : g_appState.configs) {
		kv.second.configId = FolderRewindFormat::EnsureConfigId(kv.second.configId);
	}
}

void LoadConfigs() {
	LoadConfigs(GetAppPaths().ConfigFile());
}

void LoadConfigs(const filesystem::path& filename) {
    AppPaths transactionPaths = GetAppPaths();
    transactionPaths.configRoot = filesystem::absolute(filename).parent_path();
    vector<Diagnostic> recoveryDiagnostics;
    if (!ProfileTransaction::Recover(transactionPaths, recoveryDiagnostics)) {
        g_configLoadDiagnostics.clear();
        RecordConfigDiagnostic(LegacyIniConfigCodec::DiagnosticSeverity::Fatal, 0, L"General", L"transaction", "Configuration transaction recovery failed");
        return;
    }

	map<int, Config> loadedConfigs;
    JobDocument loadedJobs;
    int loadedSelected = SelectedConfigIndex();
    int loadedNext = 2;
    auto publish = [&] {
        lock_guard lock(g_appState.configsMutex);
        g_appState.configs = std::move(loadedConfigs);
        g_appState.jobs = std::move(loadedJobs);
        g_appState.currentConfigIndex = loadedSelected;
        nextConfigId = loadedNext;
        g_appState.profileRecoveryRequired.store(false);
    };
	g_configLoadDiagnostics.clear();
	loadedNext = 2;
	loadedConfigs.clear();
	loadedJobs = JobDocument{};
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
        publish();
		return;
	}
    const string contents((istreambuf_iterator<char>(in)), {});
    const auto decoded = ConfigIniCodec::Parse(contents);
    loadedConfigs = decoded.configs;
    for (const auto& diagnostic : decoded.diagnostics) {
        RecordConfigDiagnostic(LegacyIniConfigCodec::DiagnosticSeverity::Fatal, 0, L"Config", L"", diagnostic.detail);
    }
    in.clear();
    in.seekg(0);
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
	// cur作为一个指针，指向 loadedConfigs 这个全局 map<int, Config> 中的元素 Config
	Config* cur = nullptr;
	size_t lineNumber = 0;

	while (getline(in, line1)) {
		++lineNumber;
		while (!line1.empty() && (line1.back() == '\r' || line1.back() == ' ' || line1.back() == '\t')) {
			line1.pop_back();
		}
		line = utf8_to_wstring(line1);
		if (line.empty() || line.front() == L'#') continue;
		if (line.front() == L'[' && line.back() == L']') {
			section = line.substr(1, line.size() - 2);
			while (!section.empty() && (section.back() == L' ' || section.back() == L'\t')) section.pop_back();
			size_t secStart = 0;
			while (secStart < section.size() && (section[secStart] == L' ' || section[secStart] == L'\t')) ++secStart;
			if (secStart > 0) section = section.substr(secStart);
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

				cur = &loadedConfigs[idx];
			}
		}
		else {
			auto pos = line.find(L'=');
			if (pos == wstring::npos) continue;
			wstring key = line.substr(0, pos);
			wstring val = line.substr(pos + 1);
			while (!key.empty() && (key.back() == L' ' || key.back() == L'\t')) key.pop_back();
			size_t keyStart = 0;
			while (keyStart < key.size() && (key[keyStart] == L' ' || key[keyStart] == L'\t')) ++keyStart;
			if (keyStart > 0) key = key.substr(keyStart);

			while (!val.empty() && (val.back() == L' ' || val.back() == L'\t')) val.pop_back();
			size_t valStart = 0;
			while (valStart < val.size() && (val[valStart] == L' ' || val[valStart] == L'\t')) ++valStart;
			if (valStart > 0) val = val.substr(valStart);
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
			auto readTheme = [&](optional<int>& target) {
				int parsed = 0;
				if (LegacyIniConfigCodec::TryParseInt(val, -1, 32, parsed)) {
					target = parsed;
					return true;
				}
				if (val == L"ImGuiDark") target = static_cast<int>(ThemeId::ImGuiDark);
				else if (val == L"ImGuiLight") target = static_cast<int>(ThemeId::ImGuiLight);
				else if (val == L"ImGuiClassic") target = static_cast<int>(ThemeId::ImGuiClassic);
				else if (val == L"WindowsLight") target = static_cast<int>(ThemeId::WindowsLight);
				else if (val == L"WindowsDark") target = static_cast<int>(ThemeId::WindowsDark);
				else if (val == L"NordLight") target = static_cast<int>(ThemeId::NordLight);
				else if (val == L"NordDark") target = static_cast<int>(ThemeId::NordDark);
				else if (val == L"VSCodeDark") target = static_cast<int>(ThemeId::VSCodeDark);
				else if (val == L"SolarizedLight") target = static_cast<int>(ThemeId::SolarizedLight);
				else if (val == L"SolarizedDark") target = static_cast<int>(ThemeId::SolarizedDark);
				else if (val == L"SystemAuto") target = static_cast<int>(ThemeId::SystemAuto);
				else if (val == L"Custom") target = static_cast<int>(ThemeId::Custom);
				else {
					RecordConfigDiagnostic(
						LegacyIniConfigCodec::DiagnosticSeverity::Warning,
						lineNumber, section, key,
						"expected an integer in the supported range");
					return false;
				}
				return true;
			};

            if (cur) {
                if (key == L"WorldData") {
                    while (getline(in, line1)) {
                        ++lineNumber;
                        const auto first = line1.find_first_not_of(" \t\r");
                        const auto last = line1.find_last_not_of(" \t\r");
                        if (first != string::npos && line1.substr(first, last - first + 1) == "*") break;
                    }
                }
				if (key == L"Theme") {
					optional<int> themeVal;
					if (readTheme(themeVal) && themeVal) cur->theme = *themeVal;
				}
				else if (key == L"Font") {
					cur->fontPath = val;
				}
			}
			else if (section == L"General") { // Inside [General] section
				if (key == L"CurrentConfig") {
					readInt(loadedSelected, 1, (numeric_limits<int>::max)(), false);
				}
				else if (key == L"NextConfigId") {
					readInt(loadedNext, 2, (numeric_limits<int>::max)(), false);
					int maxId = 0;
					for (auto& kv : loadedConfigs) if (kv.first > maxId) maxId = kv.first;
					if (loadedNext <= maxId) loadedNext = maxId + 1;
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
					readTheme(configuredGlobalTheme);
				}
				else if (key == L"ThemeFallback") {
					readTheme(configuredThemeFallback);
				}
				else if (key == L"SystemThemeLight") {
					readTheme(configuredSystemThemeLight);
				}
				else if (key == L"SystemThemeDark") {
					readTheme(configuredSystemThemeDark);
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
	if (!loadedConfigs.empty()) {
		const int maximumIndex = loadedConfigs.rbegin()->first;
		if (loadedNext <= maximumIndex) {
			loadedNext = maximumIndex == (numeric_limits<int>::max)()
				? maximumIndex : maximumIndex + 1;
		}
	}
	for (auto& kv : loadedConfigs) {
		Config& cfg = kv.second;
        error_code scanError;
        if (!cfg.saveRoot.empty() && filesystem::is_directory(cfg.saveRoot, scanError)) {
            for (filesystem::directory_iterator it(cfg.saveRoot, scanError), end;
                !scanError && it != end; it.increment(scanError)) {
                if (it->is_directory(scanError) && IsWorldNameAvailable(it->path().filename().wstring(), cfg.worlds))
                    cfg.worlds.push_back({it->path().filename().wstring(), L""});
            }
        }

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
		loadedJobs = jobs.document;
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
		auto normal = loadedConfigs.find(loadedSelected);
		if (normal != loadedConfigs.end() && IsValidThemeId(normal->second.theme)) {
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
		auto normal = loadedConfigs.find(loadedSelected);
		if (normal != loadedConfigs.end() && validFontPath(normal->second.fontPath)) {
			Fontss = normal->second.fontPath;
		}
		if (Fontss.empty()) {
			for (const auto& [index, config] : loadedConfigs) {
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
    publish();
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
    if (g_appState.profileRecoveryRequired.load()) return {ConfigSaveState::RecoveryRequired, L"Reload the profile before saving again."};
	// 内部全程使用 error_code 明确状态的实现，不在 replacement 之后抛出
	// 无法分类的异常；文件系统错误都转换为对应的 ConfigSaveState。
	FlushUiConfigDraft();
    static mutex persistenceMutex;
    lock_guard persistenceLock(persistenceMutex);
    map<int, Config> configs;
    int selectedIndex;
    int nextIndex;
    JobDocument jobs;
    {
        lock_guard lock(g_appState.configsMutex);
        configs = g_appState.configs;
        selectedIndex = g_appState.currentConfigIndex;
        nextIndex = nextConfigId;
        jobs = g_appState.jobs;
    }
	ConfigSaveResult result;
	const filesystem::path target(filename);
    for (const auto& [index, config] : configs) {
        if (!BackupPolicy::IsValid(config.backupMode, config.maxSmartBackupsPerFull)) {
            result.detail = utf8_to_wstring(L("BACKUP_POLICY_INVALID"));
            MB_LOG_ERROR(minebackup::logging::LogCategory::Application, "config.backup_policy.invalid",
                "Invalid backup policy for configuration {}", index);
            return result;
        }
    }

	std::wostringstream buffer;
	buffer << L"[General]\n";
	buffer << L"CurrentConfig=" << selectedIndex << L"\n";
	buffer << L"NextConfigId=" << nextIndex << L"\n";
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

    try {
        vector<Diagnostic> diagnostics;
        if (!JobStorage::ValidateReferences(jobs, configs, diagnostics)) {
            result.detail = L"Job references are invalid.";
            return result;
        }
        const auto document = ProfileConfigRepository(target).Prepare(
            configs, restoreWhitelist, true, wstring_to_utf8(buffer.str()));
        if (!document.success) {
            result.detail = L"Configuration identity or policy is invalid.";
            return result;
        }
        AppPaths paths = GetAppPaths();
        paths.configRoot = filesystem::absolute(target).parent_path();
        result = ProfileTransaction::Commit(paths, {document.content, JobStorage::Serialize(jobs), nullopt}, {}, filesystem::absolute(target));
    } catch (const exception& error) {
        result.state = ConfigSaveState::NotCommitted;
        result.detail = utf8_to_wstring(error.what());
    }
    if (result.state == ConfigSaveState::RecoveryRequired) g_appState.profileRecoveryRequired.store(true);
    if (!result.Durable()) MB_LOG_WARNING(minebackup::logging::LogCategory::Application,
        "config.save.incomplete", "{}", wstring_to_utf8(result.detail));
    return result;
}

// 在 LoadConfigs/SaveConfigs/CheckForConfigConflicts 等函数关键处调用日志接口
// 例如：

void CheckForConfigConflicts() {
	const auto configs = SnapshotConfigState().configs;
	map<wstring, vector<pair<int, wstring>>> worldMap; // Key: World Name, Value: {ConfigIndex, BackupPath}

	for (const auto& conf_pair : configs) {
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
