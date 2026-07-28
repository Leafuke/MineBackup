#include "ConfigManager.h"
#include "Globals.h"

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
	const std::filesystem::path root = std::filesystem::temp_directory_path()
		/ "minebackup-appearance-config-test";
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

	std::filesystem::remove_all(root, error);
	if (failures != 0) {
		std::cerr << failures << " appearance configuration test(s) failed\n";
		return 1;
	}
	std::cout << "All appearance configuration tests passed\n";
	return 0;
}
