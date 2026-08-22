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
	const std::map<std::string, std::filesystem::path>& environment) {
	KnownMinecraftLocationDependencies dependencies;
	dependencies.platform = platform;
	dependencies.readEnvironmentPath = [environment](std::string_view name)
		-> std::optional<std::filesystem::path> {
		const auto found = environment.find(std::string(name));
		return found == environment.end() ? std::nullopt
			: std::optional<std::filesystem::path>(found->second);
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

#ifdef _WIN32
// RAII：临时替换进程环境变量并保证恢复，避免污染同进程内的其他用例。
class ScopedEnvironmentValue {
public:
	ScopedEnvironmentValue(const wchar_t* name, const std::wstring& value)
		: name_(name), previous_(Previous(name)) {
		_wputenv_s(name, value.c_str());
	}

	~ScopedEnvironmentValue() {
		// 写回空值即可删除变量（MSVC CRT 语义），与最初未设置等效。
		_wputenv_s(name_.c_str(), previous_ ? previous_->c_str() : L"");
	}

private:
	static std::optional<std::wstring> Previous(const std::wstring& name) {
		const wchar_t* value = _wgetenv(name.c_str());
		return value && *value ? std::optional<std::wstring>(value) : std::nullopt;
	}

	std::wstring name_;
	std::optional<std::wstring> previous_;
};

// 用真实环境变量 + 默认 readEnvironmentPath 验证生产读取路径：
// Unicode 用户名目录不能因 ANSI 代码页转换而损坏。
// 夹具名额外包含 emoji：它无法被任何 ANSI 代码页表示，
// 保证旧实现（getenv 窄字符读取）在任何区域设置的机器上都会失败。
void TestWindowsUnicodeEnvironmentDefault() {
	const auto stamp =
		std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::canonical(
		std::filesystem::temp_directory_path())
		/ (L"MineBackupKnownUnicode-" + std::to_wstring(stamp));
	const auto roaming = root / L"用户数据🎮" / L"Roaming";
	const auto local = root / L"用户数据🎮" / L"Local";
	const auto javaRoot = roaming / L".minecraft";
	std::filesystem::create_directories(javaRoot);

	// 不注入 mock：覆盖默认实现（_wgetenv -> filesystem::path）本身。
	ScopedEnvironmentValue appData(L"APPDATA", roaming.wstring());
	ScopedEnvironmentValue localAppData(L"LOCALAPPDATA", local.wstring());

	KnownMinecraftLocationDependencies dependencies;
	dependencies.platform = MinecraftHostPlatform::Windows;
	KnownMinecraftLocationProvider provider(dependencies);
	const auto locations = provider.DiscoverLocations({}, {});
	Expect(Contains(locations, javaRoot, DiscoveryLocationKind::MinecraftRoot),
		"Windows default environment reader must preserve Unicode APPDATA paths");
	Expect(locations.size() == 1,
		"Unicode APPDATA fixture should yield exactly the Java root");

	std::error_code error;
	std::filesystem::remove_all(root, error);
}
#endif

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
		{{"APPDATA", appData}, {"LOCALAPPDATA", localAppData}}));
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
		MinecraftHostPlatform::Linux, {{"HOME", home}}));
	const auto linuxLocations = linux.DiscoverLocations({}, {});
	Expect(Contains(linuxLocations, linuxJava, DiscoveryLocationKind::MinecraftRoot)
			&& Contains(linuxLocations, linuxBedrock, DiscoveryLocationKind::BedrockWorldsRoot),
		"Linux should retain bounded Java and Bedrock launcher locations");

	std::stop_source stopped;
	stopped.request_stop();
	Expect(linux.DiscoverLocations({}, stopped.get_token()).empty(),
		"known-location discovery should honor cancellation");

#ifdef _WIN32
	TestWindowsUnicodeEnvironmentDefault();
#endif

	std::error_code error;
	std::filesystem::remove_all(root, error);
	if (failures != 0) return 1;
	std::cout << "All known Minecraft location tests passed\n";
	return 0;
}
