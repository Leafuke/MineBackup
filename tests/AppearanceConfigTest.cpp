#include "ConfigManager.h"
#include "AppState.h"
#include "AppPaths.h"
#include "Globals.h"
#include "KnownUserFolders.h"
#include "ThemePalette.h"
#include "AppearanceRuntime.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
	if (condition) return;
	++failures;
	std::cerr << "FAIL: " << message << '\n';
}

std::string ReadFile(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

int main() {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	// macOS exposes its temporary directory through /var, a trusted system
	// symlink to /private/var. Resolve that alias before exercising the atomic
	// writer, which intentionally rejects linked ancestors in application paths.
	const std::filesystem::path root =
		std::filesystem::canonical(std::filesystem::temp_directory_path())
		/ ("minebackup-appearance-config-test-" + std::to_string(stamp));
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root);
	AppPaths appPaths;
	appPaths.dataRoot = root / "profile-data";
	SetCurrentAppPaths(appPaths);
	const std::filesystem::path font = root / "test-font.ttf";
	{
		std::ofstream fontFile(font, std::ios::binary);
		fontFile << "test";
	}
	const std::filesystem::path legacy = root / "legacy.ini";
	const std::filesystem::path legacySaveRoot = root / "legacy-saves";
	const std::filesystem::path legacyBackupRoot = root / "legacy-backups";
	{
		std::ofstream output(legacy, std::ios::binary);
		output << "[General]\n"
			<< "CurrentConfig=2\n"
			<< "AutoScanForWorlds=1\n"
			<< "UIScale=1.5\n"
			<< "[Config2]\n"
			<< "Name=Active\n"
			<< "SavePath=" << legacySaveRoot.string() << "\n"
			<< "BackupPath=" << legacyBackupRoot.string() << "\n"
			<< "KeepCount=0\n"
			<< "Theme=5\n"
			<< "Font=" << font.string() << "\n";
	}

	LoadConfigs(legacy);
	Check(g_theme == static_cast<int>(ThemeId::NordLight),
		"active legacy theme migrates to application appearance");
	Check(std::filesystem::path(Fontss) == font,
		"active valid legacy font migrates to application appearance");
	Check(g_uiScaleMigrationPending,
		"legacy scale is pending semantic migration");
	Check(g_AutoScanForWorlds && g_appState.configs.size() == 1,
		"legacy AutoScanForWorlds=1 remains readable without creating configurations");
	Check(g_appState.configs.at(2).keepCount == 0
			&& std::filesystem::path(g_appState.configs.at(2).backupPath)
				== legacyBackupRoot,
		"legacy keepCount and backupPath remain unchanged during onboarding upgrade");
	Check(std::filesystem::path(g_defaultBackupRootPath)
			== KnownUserFolders::Resolver{}.ResolveRecommendedBackupRoot(appPaths),
		"legacy INI without a default backup root receives the in-memory recommendation");
	FinalizeUiScaleMigration(1.5f);
	Check(g_uiScale == 1.0f && !g_uiScaleMigrationPending && g_uiScaleV2,
		"DPI-derived legacy scale migrates once");
	Job job;
	job.jobId = L"11111111-1111-4111-8111-111111111111";
	job.name = "GUI round trip";
	JobStage stage;
	stage.stageId = L"22222222-2222-4222-8222-222222222222";
	stage.name = "Sequential stage";
	JobStep step;
	step.stepId = L"33333333-3333-4333-8333-333333333333";
	step.name = "Explicit process";
	step.type = JobStepType::Process;
	step.process.executable = L"tool";
	step.process.arguments = {L"--value", L"with spaces"};
	stage.steps.push_back(step);
	job.stages.push_back(stage);
	g_appState.jobs.jobs = {job};
	const std::filesystem::path customBackupRoot = root / "custom-backups";
	g_defaultBackupRootPath = customBackupRoot.wstring();

	const std::filesystem::path roundTrip = root / "roundtrip.ini";
	Check(SaveConfigs(roundTrip), "global appearance saves atomically");
	const std::string saved = ReadFile(roundTrip);
	Check(saved.find("UIScaleMode=UserMultiplierV2") != std::string::npos,
		"scale semantic version is persisted");
	Check(saved.find("AppearanceSchema=1") != std::string::npos,
		"appearance schema is persisted");
	Check(saved.find("DefaultBackupRootPath=" + customBackupRoot.string())
			!= std::string::npos,
		"default backup root is persisted");
	Check(saved.find("AutoScanForWorlds=1") != std::string::npos,
		"legacy AutoScanForWorlds remains writable for compatibility");
	Check(saved.find("\nTheme=5\n") != std::string::npos,
		"global theme is persisted");
	Check(saved.find("\nFont=" + font.string() + "\n") != std::string::npos,
		"global font is persisted");
	const std::size_t configSection = saved.find("[Config2]");
	Check(configSection != std::string::npos
		&& saved.find("\nTheme=", configSection) == std::string::npos,
		"legacy per-config theme is no longer written");
	Check(configSection != std::string::npos
		&& saved.find("\nFont=", configSection) == std::string::npos,
		"legacy per-config font is no longer written");
	Check(std::filesystem::exists(root / "jobs.json"),
		"GUI save writes the shared jobs.json document");

	g_theme = static_cast<int>(ThemeId::ImGuiDark);
	Fontss.clear();
	g_uiScale = 2.0f;
	g_defaultBackupRootPath.clear();
	LoadConfigs(roundTrip);
	Check(g_theme == static_cast<int>(ThemeId::NordLight)
		&& std::filesystem::path(Fontss) == font && g_uiScale == 1.0f
		&& !g_uiScaleMigrationPending,
		"global appearance round-trips without a second migration");
	Check(std::filesystem::path(g_defaultBackupRootPath) == customBackupRoot,
		"default backup root round-trips without changing existing configs");
	Check(g_AutoScanForWorlds && g_appState.configs.size() == 1,
		"legacy AutoScanForWorlds=1 round-trips without activating discovery");
	Check(g_appState.configs.at(2).keepCount == 0
			&& std::filesystem::path(g_appState.configs.at(2).backupPath)
				== legacyBackupRoot,
		"default backup root changes do not rewrite an existing configuration");
	Check(g_appState.jobs.jobs.size() == 1
			&& g_appState.jobs.jobs[0].stages[0].steps[0].process.arguments.size() == 2
			&& g_appState.jobs.jobs[0].stages[0].steps[0].process.arguments[1] == L"with spaces",
		"GUI and CLI share the versioned Job document without shell argument loss");

	const std::filesystem::path malformed = root / "malformed.ini";
	{
		std::ofstream output(malformed, std::ios::binary);
		output << "[General]\n"
			<< "Language=x\n"
			<< "WindowWidth=not-a-number\n"
			<< "[Config2]\n"
			<< "ConfigName=Broken\n"
			<< "ConfigId=11111111-1111-1111-1111-111111111111\n"
			<< "ZipLevel=invalid\n"
			<< "[SpCfg3]\n"
			<< "Name=Broken task\n"
			<< "SpecialConfigId=22222222-2222-2222-2222-222222222222\n"
			<< "UnifiedTask=1,name,with,comma,0,0,0,1,2,0,0,15,0,0,0,0\n";
	}
	bool malformedThrew = false;
	try {
		LoadConfigs(malformed);
	}
	catch (...) {
		malformedThrew = true;
	}
	Check(!malformedThrew, "malformed INI values do not escape as exceptions");
	Check(LastConfigLoadHasFatalDiagnostics(),
		"malformed operational INI values produce fatal diagnostics");
	Check(!GetLastConfigLoadDiagnostics().empty(),
		"malformed INI diagnostics retain source context");

	const std::filesystem::path taskRoot = root / "task-migration";
	std::filesystem::create_directories(taskRoot);
	const std::filesystem::path legacyTasks = taskRoot / "config.ini";
	{
		std::ofstream output(legacyTasks, std::ios::binary);
		output << "[General]\nCurrentConfig=3\n"
			<< "[Config2]\n"
			<< "ConfigName=Worlds\n"
			<< "ConfigId=11111111-1111-4111-8111-111111111111\n"
			<< "SavePath=" << (taskRoot / "saves").string() << "\n"
			<< "WorldData=\nworld/subdirectory\ndescription\n*\n"
			<< "BackupPath=" << (taskRoot / "backups").string() << "\n"
			<< "[SpCfg3]\n"
			<< "Name=Automation\n"
			<< "SpecialConfigId=22222222-2222-4222-8222-222222222222\n"
			<< "Command=echo A,B\n"
			<< "UnifiedTask=7,Backup,0,0,0,1,2,0,,,15,0,0,0,0\n";
	}
	LoadConfigs(legacyTasks);
	const std::filesystem::path migratedTasks = taskRoot / "special-tasks.json";
	Check(!LastConfigLoadHasFatalDiagnostics()
			&& !std::filesystem::exists(migratedTasks),
		"GUI load ignores legacy special tasks without migrating or creating files");
	Check(SaveConfigs(legacyTasks), "configuration with ignored legacy sections saves");
	const std::string cleanedTasksIni = ReadFile(legacyTasks);
	Check(cleanedTasksIni.find("[SpCfg3]") != std::string::npos
			&& cleanedTasksIni.find("Command=echo A,B") != std::string::npos
			&& cleanedTasksIni.find("UnifiedTask=") != std::string::npos,
		"GUI save preserves ignored legacy special sections byte-for-byte");

	{
		std::ofstream output(migratedTasks, std::ios::binary | std::ios::trunc);
		output << R"({"schemaVersion":2,"specialConfigs":[]})";
	}
	LoadConfigs(legacyTasks);
	Check(!LastConfigLoadHasFatalDiagnostics()
			&& ReadFile(migratedTasks).find("\"schemaVersion\":2") != std::string::npos,
		"future legacy task schemas are retained but ignored");

	Check(IsValidThemeId(static_cast<int>(ThemeId::ImGuiDark)), "ImGuiDark is valid theme");
	Check(IsValidThemeId(static_cast<int>(ThemeId::VSCodeDark)), "VSCodeDark is valid theme");
	Check(IsValidThemeId(static_cast<int>(ThemeId::SolarizedLight)), "SolarizedLight is valid theme");
	Check(IsValidThemeId(static_cast<int>(ThemeId::SolarizedDark)), "SolarizedDark is valid theme");
	Check(IsValidThemeId(static_cast<int>(ThemeId::SystemAuto)), "SystemAuto is valid theme");
	Check(IsValidThemeId(static_cast<int>(ThemeId::Custom)), "Custom is valid theme");
	Check(!IsValidThemeId(-1), "-1 is invalid theme");
	Check(!IsValidThemeId(12), "12 is invalid theme");

	const std::filesystem::path newThemesIni = root / "new-themes.ini";
	g_theme = static_cast<int>(ThemeId::VSCodeDark);
	Check(SaveConfigs(newThemesIni), "VSCodeDark theme saves");
	g_theme = static_cast<int>(ThemeId::ImGuiDark);
	LoadConfigs(newThemesIni);
	Check(g_theme == static_cast<int>(ThemeId::VSCodeDark), "VSCodeDark theme round-trips");

	g_theme = static_cast<int>(ThemeId::SolarizedLight);
	Check(SaveConfigs(newThemesIni), "SolarizedLight theme saves");
	g_theme = static_cast<int>(ThemeId::ImGuiDark);
	LoadConfigs(newThemesIni);
	Check(g_theme == static_cast<int>(ThemeId::SolarizedLight), "SolarizedLight theme round-trips");

	g_theme = static_cast<int>(ThemeId::SystemAuto);
	g_systemThemeLight = static_cast<int>(ThemeId::NordLight);
	g_systemThemeDark = static_cast<int>(ThemeId::VSCodeDark);
	Check(SaveConfigs(newThemesIni), "SystemAuto theme and subthemes save");
	g_theme = static_cast<int>(ThemeId::ImGuiDark);
	g_systemThemeLight = static_cast<int>(ThemeId::WindowsLight);
	g_systemThemeDark = static_cast<int>(ThemeId::WindowsDark);
	LoadConfigs(newThemesIni);
	Check(g_theme == static_cast<int>(ThemeId::SystemAuto), "SystemAuto theme round-trips");
	Check(g_systemThemeLight == static_cast<int>(ThemeId::NordLight), "SystemThemeLight round-trips");
	Check(g_systemThemeDark == static_cast<int>(ThemeId::VSCodeDark), "SystemThemeDark round-trips");

	{
		std::ofstream out(newThemesIni, std::ios::trunc);
		out << "[General]\nTheme=10\nSystemThemeLight=10\nSystemThemeDark=999\n";
	}
	LoadConfigs(newThemesIni);
	Check(g_systemThemeLight == static_cast<int>(ThemeId::WindowsLight), "SystemThemeLight falls back when set to SystemAuto");
	Check(g_systemThemeDark == static_cast<int>(ThemeId::WindowsDark), "SystemThemeDark falls back when set to out of range");

	const ImVec4 successDark = ThemePalette::GetStatusColor(ThemePalette::StatusColor::Success, true);
	const ImVec4 successLight = ThemePalette::GetStatusColor(ThemePalette::StatusColor::Success, false);
	Check(successDark.w == 1.0f && successLight.w == 1.0f, "ThemePalette status colors have solid alpha");
	const ImVec4 infoLogDark = ThemePalette::GetLogLevelColor(minebackup::logging::LogLevel::Info, true);
	const ImVec4 infoLogLight = ThemePalette::GetLogLevelColor(minebackup::logging::LogLevel::Info, false);
	Check(infoLogDark.x > 0.8f && infoLogLight.x < 0.3f, "Log Info level contrasts correctly between dark and light");

	ImGui::CreateContext();
	g_theme = static_cast<int>(ThemeId::ImGuiLight);
	ApplyTheme();
	const ImVec4 chkBg = ImGui::GetStyle().Colors[ImGuiCol_CheckboxSelectedBg];
	const ImVec4 chkMark = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
	Check(chkBg.w == 1.0f, "CheckboxSelectedBg has solid alpha");
	Check(ImGuiTheme::RelativeLuminance(chkBg) > 0.6f, "CheckboxSelectedBg in ImGuiLight is not black");
	Check(ImGuiTheme::ContrastRatio(chkMark, chkBg) >= 3.0f, "CheckMark contrasts with CheckboxSelectedBg in ImGuiLight");
	ImGui::DestroyContext();

	g_windowWidth = 1440;
	g_windowHeight = 900;
	Check(SaveConfigs(newThemesIni), "Custom window size saves to ini");
	g_windowWidth = 800;
	g_windowHeight = 600;
	LoadConfigs(newThemesIni);
	Check(g_windowWidth == 1440 && g_windowHeight == 900, "Window dimensions round-trip through LoadConfigs/SaveConfigs");

	{
		std::ofstream out(newThemesIni, std::ios::binary | std::ios::trunc);
		out << "[General]\r\nTheme = 10 \r\nSystemThemeLight = 3\r\nSystemThemeDark = 4\r\n";
	}
	g_theme = 0;
	LoadConfigs(newThemesIni);
	Check(g_theme == static_cast<int>(ThemeId::SystemAuto), "Theme parses with CRLF and whitespace padding");
	Check(GetLastConfigLoadDiagnostics().empty(), "No diagnostics for CRLF formatted config with whitespace");

	{
		std::ofstream out(newThemesIni, std::ios::binary | std::ios::trunc);
		out << "[General]\r\nTheme = NordLight\r\nSystemThemeLight = WindowsLight\r\nSystemThemeDark = VSCodeDark\r\n";
	}
	g_theme = 0;
	LoadConfigs(newThemesIni);
	Check(g_theme == static_cast<int>(ThemeId::NordLight), "Theme parses symbolic name NordLight");
	Check(g_systemThemeLight == static_cast<int>(ThemeId::WindowsLight), "SystemThemeLight parses symbolic name WindowsLight");
	Check(g_systemThemeDark == static_cast<int>(ThemeId::VSCodeDark), "SystemThemeDark parses symbolic name VSCodeDark");

	std::filesystem::remove_all(root, error);
	if (failures != 0) {
		std::cerr << failures << " appearance configuration test(s) failed\n";
		return 1;
	}
	std::cout << "All appearance configuration tests passed\n";
	return 0;
}
