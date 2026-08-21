#include "KnownMinecraftLocationProvider.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
	if (condition) return;
	++failures;
	std::cerr << "FAIL: " << message << '\n';
}

KnownMinecraftLocationDependencies Dependencies(
	MinecraftHostPlatform platform,
	const std::map<std::string, std::string>& environment) {
	KnownMinecraftLocationDependencies dependencies;
	dependencies.platform = platform;
	dependencies.readEnvironment = [environment](std::string_view name)
		-> std::optional<std::string> {
		const auto found = environment.find(std::string(name));
		return found == environment.end() ? std::nullopt
			: std::optional<std::string>(found->second);
	};
	return dependencies;
}

bool Contains(
	const std::vector<DiscoveryLocation>& locations,
	const std::filesystem::path& path,
	DiscoveryLocationKind kind) {
	return std::any_of(locations.begin(), locations.end(), [&](const auto& location) {
		return location.path == path && location.kind == kind;
	});
}

} // namespace

int main() {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path())
		/ ("MineBackupKnownMinecraft-" + std::to_string(stamp));
	const auto appData = root / "roaming";
	const auto localAppData = root / "local";
	const auto javaRoot = appData / ".minecraft";
	const auto launcherBedrock = appData / "Minecraft Bedrock" / "Users" / "123"
		/ "games" / "com.mojang" / "minecraftWorlds";
	const auto uwpBedrock = localAppData / "Packages"
		/ "Microsoft.MinecraftUWP_8wekyb3d8bbwe" / "LocalState" / "games"
		/ "com.mojang" / "minecraftWorlds";
	std::filesystem::create_directories(javaRoot);
	std::filesystem::create_directories(launcherBedrock);
	std::filesystem::create_directories(uwpBedrock);
	std::filesystem::create_directories(
		appData / "Minecraft Bedrock" / "Users" / "not-numeric");

	KnownMinecraftLocationProvider windows(Dependencies(
		MinecraftHostPlatform::Windows,
		{{"APPDATA", appData.string()}, {"LOCALAPPDATA", localAppData.string()}}));
	std::vector<DiscoveryDiagnostic> diagnostics;
	const auto locations = windows.DiscoverLocations(
		{[&](DiscoveryDiagnostic diagnostic) { diagnostics.push_back(std::move(diagnostic)); }}, {});
	Expect(Contains(locations, javaRoot, DiscoveryLocationKind::MinecraftRoot),
		"Windows should return the standard Java root");
	Expect(Contains(locations, launcherBedrock, DiscoveryLocationKind::BedrockWorldsRoot)
			&& Contains(locations, uwpBedrock, DiscoveryLocationKind::BedrockWorldsRoot),
		"Windows should preserve Launcher and UWP Bedrock locations");
	Expect(locations.size() == 3,
		"known locations should not recurse through unrelated user directories");

	const auto home = root / "home";
	const auto linuxJava = home / ".minecraft";
	const auto linuxBedrock = home / ".local" / "share" / "mcpelauncher" / "games"
		/ "com.mojang" / "minecraftWorlds";
	std::filesystem::create_directories(linuxJava);
	std::filesystem::create_directories(linuxBedrock);
	KnownMinecraftLocationProvider linux(Dependencies(
		MinecraftHostPlatform::Linux, {{"HOME", home.string()}}));
	const auto linuxLocations = linux.DiscoverLocations({}, {});
	Expect(Contains(linuxLocations, linuxJava, DiscoveryLocationKind::MinecraftRoot)
			&& Contains(linuxLocations, linuxBedrock, DiscoveryLocationKind::BedrockWorldsRoot),
		"Linux should retain bounded Java and Bedrock launcher locations");

	std::stop_source stopped;
	stopped.request_stop();
	Expect(linux.DiscoverLocations({}, stopped.get_token()).empty(),
		"known-location discovery should honor cancellation");

	std::error_code error;
	std::filesystem::remove_all(root, error);
	if (failures != 0) return 1;
	std::cout << "All known Minecraft location tests passed\n";
	return 0;
}
