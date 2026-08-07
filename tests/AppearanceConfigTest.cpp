#include "ConfigManager.h"
#include "AppState.h"
#include "Globals.h"

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
	const std::filesystem::path font = root / "test-font.ttf";
	{
		std::ofstream fontFile(font, std::ios::binary);
		fontFile << "test";
	}
	const std::filesystem::path legacy = root / "legacy.ini";
	{
		std::ofstream output(legacy, std::ios::binary);
		output << "[General]\n"
			<< "CurrentConfig=2\n"
			<< "UIScale=1.5\n"
			<< "[Config2]\n"
			<< "Name=Active\n"
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
	FinalizeUiScaleMigration(1.5f);
	Check(g_uiScale == 1.0f && !g_uiScaleMigrationPending && g_uiScaleV2,
		"DPI-derived legacy scale migrates once");

	const std::filesystem::path roundTrip = root / "roundtrip.ini";
	Check(SaveConfigs(roundTrip), "global appearance saves atomically");
	const std::string saved = ReadFile(roundTrip);
	Check(saved.find("UIScaleMode=UserMultiplierV2") != std::string::npos,
		"scale semantic version is persisted");
	Check(saved.find("AppearanceSchema=1") != std::string::npos,
		"appearance schema is persisted");
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

	g_theme = static_cast<int>(ThemeId::ImGuiDark);
	Fontss.clear();
	g_uiScale = 2.0f;
	LoadConfigs(roundTrip);
	Check(g_theme == static_cast<int>(ThemeId::NordLight)
		&& std::filesystem::path(Fontss) == font && g_uiScale == 1.0f
		&& !g_uiScaleMigrationPending,
		"global appearance round-trips without a second migration");

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

	std::filesystem::remove_all(root, error);
	if (failures != 0) {
		std::cerr << failures << " appearance configuration test(s) failed\n";
		return 1;
	}
	std::cout << "All appearance configuration tests passed\n";
	return 0;
}
