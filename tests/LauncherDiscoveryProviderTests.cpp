#include "LauncherDiscoveryUtils.h"
#include "NeteaseMinecraftDiscoveryProvider.h"
#include "HmclDiscoveryProvider.h"
#include "PrismLauncherDiscoveryProvider.h"
#include "ModrinthDiscoveryProvider.h"
#include "PathIdentity.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
	if (condition) return;
	++failures;
	std::cerr << "[FAIL] " << message << '\n';
}

std::filesystem::path TemporaryRoot() {
	return std::filesystem::temp_directory_path()
		/ ("MineBackupLauncherTest-"
			+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void Touch(const std::filesystem::path& path, const std::string& content = "") {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
}

class ScopedEnvVar {
public:
	ScopedEnvVar(const std::wstring& name, const std::wstring& value)
		: name_(name) {
#ifdef _WIN32
		const DWORD len = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
		if (len > 0) {
			std::wstring prev(len, L'\0');
			GetEnvironmentVariableW(name_.c_str(), prev.data(), len);
			prev.resize(len - 1);
			previous_ = prev;
		}
		SetEnvironmentVariableW(name_.c_str(), value.c_str());
		_wputenv_s(name_.c_str(), value.c_str());
#else
		std::string n(name_.begin(), name_.end());
		const char* prev = std::getenv(n.c_str());
		if (prev != nullptr) {
			previous_ = std::wstring(prev, prev + std::strlen(prev));
		}
		std::string v(value.begin(), value.end());
		setenv(n.c_str(), v.c_str(), 1);
#endif
	}

	~ScopedEnvVar() {
#ifdef _WIN32
		if (previous_.has_value()) {
			SetEnvironmentVariableW(name_.c_str(), previous_->c_str());
			_wputenv_s(name_.c_str(), previous_->c_str());
		}
		else {
			SetEnvironmentVariableW(name_.c_str(), nullptr);
			_wputenv_s(name_.c_str(), L"");
		}
#else
		std::string n(name_.begin(), name_.end());
		if (previous_.has_value()) {
			std::string v(previous_->begin(), previous_->end());
			setenv(n.c_str(), v.c_str(), 1);
		}
		else {
			unsetenv(n.c_str());
		}
#endif
	}

private:
	std::wstring name_;
	std::optional<std::wstring> previous_;
};

#ifdef _WIN32
void TestWindowsEnvironmentExpansion() {
	// 1. 无环境变量的普通路径
	const std::wstring rawNormal = L"C:\\MineBackup\\Tests";
	const auto expandedNormal = LauncherDiscoveryUtils::ExpandEnvironmentString(rawNormal);
	Expect(expandedNormal.has_value(), "plain string expansion should succeed");
	Expect(*expandedNormal == rawNormal, "plain string expansion should not alter characters");

	// 2. 空字符串
	const auto expandedEmpty = LauncherDiscoveryUtils::ExpandEnvironmentString(L"");
	Expect(expandedEmpty.has_value() && expandedEmpty->empty(), "empty string expansion should return empty");

	// 3. 环境变量展开（如 %TEMP%）
	const std::wstring rawWithEnv = L"%TEMP%\\MineBackupSub";
	const auto expandedWithEnv = LauncherDiscoveryUtils::ExpandEnvironmentString(rawWithEnv);
	Expect(expandedWithEnv.has_value(), "environment variable expansion should succeed");
	Expect(!expandedWithEnv->empty(), "expanded string should not be empty");
	Expect(expandedWithEnv->find(L'%') == std::wstring::npos, "expanded string should not contain % variable name");
	Expect(expandedWithEnv->length() == wcslen(expandedWithEnv->c_str()), "expanded string length must not include trailing null");

	// 4. Unicode / Emoji 路径展开
	const std::wstring rawUnicode = L"%TEMP%\\我的世界🎮";
	const auto expandedUnicode = LauncherDiscoveryUtils::ExpandEnvironmentString(rawUnicode);
	Expect(expandedUnicode.has_value(), "unicode expansion should succeed");
	Expect(expandedUnicode->find(L"我的世界🎮") != std::wstring::npos, "unicode characters and emoji must be preserved");
}
#endif

void TestNeteaseProvider(const std::filesystem::path& root) {
	const auto javaBase = root / "NeteaseJava";
	const auto javaRoot = javaBase / "Game" / ".minecraft";
	const auto appData = root / "NeteaseAppData";
	const auto bedrockRoot = appData / "MinecraftPE_Netease" / "minecraftWorlds";

	Touch(javaRoot / "saves" / "World1" / "level.dat");
	Touch(bedrockRoot / "BedrockWorld" / "level.dat");

	// 1. Java 版与基岩版均存在
	NeteaseDiscoveryDependencies deps;
	deps.readDownloadPath = [&](const MinecraftDiscoveryContext&, const std::string&) -> std::optional<std::filesystem::path> {
		return javaBase;
	};
	deps.readEnvironmentPath = [&](std::string_view name) -> std::optional<std::filesystem::path> {
		if (name == "APPDATA") return appData;
		return std::nullopt;
	};
	deps.isDirectory = [](const std::filesystem::path& path, std::error_code& ec) {
		return LauncherDiscoveryUtils::IsDirectory(path, ec);
	};

	NeteaseMinecraftDiscoveryProvider provider(deps);
	auto locations = provider.DiscoverLocations({}, {});
	Expect(locations.size() == 2, "Netease provider should discover both Java and Bedrock locations");
	if (locations.size() >= 2) {
		Expect(locations[0].kind == DiscoveryLocationKind::MinecraftRoot
			&& locations[0].suggestedName == L"网易我的世界 Java版"
			&& PathIdentity::PathsEqual(locations[0].path, javaRoot),
			"Netease Java location should have correct path, kind and suggestedName");
		Expect(locations[0].evidence.size() == 1
			&& locations[0].evidence[0].kind == DiscoveryEvidenceKind::LauncherSettings
			&& locations[0].evidence[0].providerId == "netease-minecraft",
			"Netease Java location should have LauncherSettings evidence");
		Expect(locations[1].kind == DiscoveryLocationKind::BedrockWorldsRoot
			&& locations[1].suggestedName == L"网易我的世界 基岩版"
			&& PathIdentity::PathsEqual(locations[1].path, bedrockRoot),
			"Netease Bedrock location should have correct path, kind and suggestedName");
		Expect(locations[1].evidence.size() == 1
			&& locations[1].evidence[0].kind == DiscoveryEvidenceKind::KnownLocation
			&& locations[1].evidence[0].providerId == "netease-minecraft",
			"Netease Bedrock location should have KnownLocation evidence");
	}

	// 2. 仅 Java 路径存在且 Game/.minecraft 不存在
	NeteaseDiscoveryDependencies depsMissingJava;
	depsMissingJava.readDownloadPath = [&](const MinecraftDiscoveryContext&, const std::string&) -> std::optional<std::filesystem::path> {
		return root / "NonExistent";
	};
	depsMissingJava.readEnvironmentPath = [](std::string_view) -> std::optional<std::filesystem::path> {
		return std::nullopt;
	};
	NeteaseMinecraftDiscoveryProvider providerMissing(depsMissingJava);
	auto missingLocations = providerMissing.DiscoverLocations({}, {});
	Expect(missingLocations.empty(), "Netease provider should not discover missing Java game root");

	// 3. 取消请求
	std::stop_source stop;
	stop.request_stop();
	auto cancelled = provider.DiscoverLocations({}, stop.get_token());
	Expect(cancelled.empty(), "Netease provider should honor cancellation stop token");
}

void TestHmclProvider(const std::filesystem::path& root) {
	const auto hmclHome = root / "HMCLHome";
	const auto gameDir1 = root / "HMCLGame1";
	const auto gameDir2 = root / "HMCLGame2";
	std::filesystem::create_directories(gameDir1);
	std::filesystem::create_directories(gameDir2);

	const std::string jsonContent = "{\n"
		"  \"directories\": [\n"
		"    {\"name\": \"My Modpack\", \"path\": \"" + (root / "HMCLGame1").generic_string() + "\"},\n"
		"    {\"name\": {\"default\": \"Localized Pack\", \"zh_cn\": \"本地化整合包\"}, \"path\": \"" + (root / "HMCLGame2").generic_string() + "\"}\n"
		"  ]\n"
		"}";
	Touch(hmclHome / "config" / "user-game-directories.json", jsonContent);

	ScopedEnvVar appDataEnv(L"APPDATA", (root / "Roaming").wstring());
	ScopedEnvVar homeEnv(L"HOME", (root / "Home").wstring());
	ScopedEnvVar xdgEnv(L"XDG_DATA_HOME", (root / "XdgData").wstring());
	ScopedEnvVar hmclEnv(L"HMCL_USER_HOME", hmclHome.wstring());

	HmclDiscoveryProvider provider;
	auto locations = provider.DiscoverLocations({}, {});
	Expect(locations.size() == 2, "HMCL provider should discover both configured game directories");
	if (locations.size() >= 2) {
		Expect(locations[0].suggestedName == L"My Modpack"
			&& PathIdentity::PathsEqual(locations[0].path, gameDir1),
			"HMCL string name should be parsed correctly");
		Expect(locations[1].suggestedName == L"Localized Pack"
			&& PathIdentity::PathsEqual(locations[1].path, gameDir2),
			"HMCL localized object default name should be parsed correctly");
	}
}

void TestPrismProvider(const std::filesystem::path& root) {
	const auto prismData = root / "PrismData";
	const auto customInstances = root / "CustomPrismInstances";
	const auto instance1 = customInstances / "InstanceA";
	const auto instance2 = customInstances / "InstanceB";

	Touch(prismData / "prismlauncher.cfg", "InstanceDir=" + customInstances.generic_string() + "\n");
	Touch(instance1 / "instance.cfg", "name=Fabric 1.20\n");
	std::filesystem::create_directories(instance1 / "minecraft");
	Touch(instance2 / "instance.cfg", "name=\n");
	std::filesystem::create_directories(instance2 / ".minecraft");

	ScopedEnvVar appDataEnv(L"APPDATA", (root / "Roaming").wstring());
	ScopedEnvVar userProfileEnv(L"USERPROFILE", (root / "UserProfile").wstring());
	ScopedEnvVar homeEnv(L"HOME", (root / "Home").wstring());
	ScopedEnvVar xdgEnv(L"XDG_DATA_HOME", (root / "XdgData").wstring());

#ifdef _WIN32
	const auto activePrism = root / "Roaming" / "PrismLauncher";
#elif defined(__APPLE__)
	const auto activePrism = root / "Home" / "Library" / "Application Support" / "PrismLauncher";
#else
	const auto activePrism = root / "XdgData" / "PrismLauncher";
#endif
	std::filesystem::create_directories(activePrism.parent_path());
	std::filesystem::copy(prismData, activePrism, std::filesystem::copy_options::recursive);

	PrismLauncherDiscoveryProvider provider;
	auto locations = provider.DiscoverLocations({}, {});
	Expect(locations.size() == 2, "Prism provider should discover instances in custom InstanceDir");
	if (locations.size() >= 2) {
		Expect(locations[0].suggestedName == L"Fabric 1.20"
			&& PathIdentity::PathsEqual(locations[0].path, instance1 / "minecraft"),
			"Prism instance.cfg name and modern 'minecraft' directory should be preferred");
		Expect(locations[1].suggestedName == L"InstanceB"
			&& PathIdentity::PathsEqual(locations[1].path, instance2 / ".minecraft"),
			"Prism instance fallback name and legacy '.minecraft' directory should be used");
	}
}

void TestModrinthProvider(const std::filesystem::path& root) {
	const auto appData = root / "ModrinthAppData";
	ScopedEnvVar appDataEnv(L"APPDATA", appData.wstring());
	ScopedEnvVar homeEnv(L"HOME", (root / "Home").wstring());
	ScopedEnvVar xdgEnv(L"XDG_DATA_HOME", (root / "XdgData").wstring());

#ifdef _WIN32
	const auto newProfiles = appData / "ModrinthApp" / "profiles";
	const auto legacyProfiles = appData / "com.modrinth.theseus" / "profiles";
#elif defined(__APPLE__)
	const auto newProfiles = root / "Home" / "Library" / "Application Support" / "ModrinthApp" / "profiles";
	const auto legacyProfiles = root / "Home" / "Library" / "Application Support" / "com.modrinth.theseus" / "profiles";
#else
	const auto newProfiles = root / "XdgData" / "ModrinthApp" / "profiles";
	const auto legacyProfiles = root / "XdgData" / "com.modrinth.theseus" / "profiles";
#endif

	const auto newInst = newProfiles / "ModpackNew";
	const auto legacyInst = legacyProfiles / "ModpackOld";

	Touch(legacyInst / "saves" / "OldWorld" / "level.dat");

	// 1. 当只有旧版存在时，回退探测旧版 com.modrinth.theseus/profiles
	ModrinthDiscoveryProvider providerLegacy;
	auto locations1 = providerLegacy.DiscoverLocations({}, {});
	Expect(locations1.size() == 1 && PathIdentity::PathsEqual(locations1[0].path, legacyInst),
		"Modrinth provider should fallback to legacy com.modrinth.theseus profiles when new version is absent");

	// 2. 当新版 ModrinthApp/profiles 存在时，即使为空也不回退旧版
	std::filesystem::create_directories(newProfiles);
	ModrinthDiscoveryProvider providerNewEmpty;
	auto locations2 = providerNewEmpty.DiscoverLocations({}, {});
	Expect(locations2.empty(),
		"Modrinth provider must NOT fallback to legacy directory when new ModrinthApp profiles directory exists");

	// 3. 当新版 ModrinthApp/profiles 包含实例时正常发现
	Touch(newInst / "saves" / "NewWorld" / "level.dat");
	ModrinthDiscoveryProvider providerNew;
	auto locations3 = providerNew.DiscoverLocations({}, {});
	Expect(locations3.size() == 1 && PathIdentity::PathsEqual(locations3[0].path, newInst),
		"Modrinth provider should discover profiles in new ModrinthApp");
}

} // namespace

int main() {
	const auto root = TemporaryRoot();
	std::error_code error;
	std::filesystem::create_directories(root);

#ifdef _WIN32
	TestWindowsEnvironmentExpansion();
#endif
	TestNeteaseProvider(root);
	TestHmclProvider(root);
	TestPrismProvider(root);
	TestModrinthProvider(root);

	std::filesystem::remove_all(root, error);
	if (failures == 0) {
		std::cout << "[PASS] Launcher discovery provider tests\n";
		return 0;
	}
	return 1;
}
