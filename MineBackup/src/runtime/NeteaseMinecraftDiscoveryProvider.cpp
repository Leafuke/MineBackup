#include "NeteaseMinecraftDiscoveryProvider.h"

#include "LauncherDiscoveryUtils.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <system_error>
#include <vector>

namespace {

#ifdef _WIN32
// 从 Windows 注册表读取网易启动器的 DownloadPath
std::optional<std::filesystem::path> ReadNeteaseDownloadPath(
	const MinecraftDiscoveryContext& context,
	const std::string& providerId) {
	HKEY hKey = nullptr;
	const LSTATUS openStatus = RegOpenKeyExW(
		HKEY_CURRENT_USER,
		L"Software\\Netease\\MCLauncher",
		0,
		KEY_READ,
		&hKey);

	if (openStatus != ERROR_SUCCESS) {
		if (openStatus != ERROR_FILE_NOT_FOUND && openStatus != ERROR_PATH_NOT_FOUND) {
			context.reportDiagnostic({
				"netease_registry_read_failed",
				providerId,
				{},
				std::error_code(static_cast<int>(openStatus), std::system_category())
			});
		}
		return std::nullopt;
	}

	struct RegKeyCloser {
		HKEY key;
		~RegKeyCloser() {
			if (key != nullptr) {
				RegCloseKey(key);
			}
		}
	} closer{hKey};

	DWORD type = 0;
	DWORD bytesNeeded = 0;
	LSTATUS queryStatus = RegQueryValueExW(
		hKey,
		L"DownloadPath",
		nullptr,
		&type,
		nullptr,
		&bytesNeeded);

	if (queryStatus != ERROR_SUCCESS) {
		if (queryStatus != ERROR_FILE_NOT_FOUND) {
			context.reportDiagnostic({
				"netease_registry_read_failed",
				providerId,
				{},
				std::error_code(static_cast<int>(queryStatus), std::system_category())
			});
		}
		return std::nullopt;
	}

	if (type != REG_SZ && type != REG_EXPAND_SZ) {
		context.reportDiagnostic({
			"netease_registry_read_failed",
			providerId,
			{},
			{}
		});
		return std::nullopt;
	}

	if (bytesNeeded == 0 || bytesNeeded > 64 * 1024) {
		context.reportDiagnostic({
			"netease_download_path_invalid",
			providerId,
			{},
			{}
		});
		return std::nullopt;
	}

	std::vector<wchar_t> buffer(bytesNeeded / sizeof(wchar_t) + 2, L'\0');
	queryStatus = RegQueryValueExW(
		hKey,
		L"DownloadPath",
		nullptr,
		&type,
		reinterpret_cast<LPBYTE>(buffer.data()),
		&bytesNeeded);

	if (queryStatus != ERROR_SUCCESS) {
		context.reportDiagnostic({
			"netease_registry_read_failed",
			providerId,
			{},
			std::error_code(static_cast<int>(queryStatus), std::system_category())
		});
		return std::nullopt;
	}

	std::wstring rawStr(buffer.data());
	while (!rawStr.empty() && rawStr.back() == L'\0') {
		rawStr.pop_back();
	}
	if (rawStr.empty()) {
		context.reportDiagnostic({
			"netease_download_path_invalid",
			providerId,
			{},
			{}
		});
		return std::nullopt;
	}

	if (type == REG_EXPAND_SZ) {
		const auto expanded = LauncherDiscoveryUtils::ExpandEnvironmentString(rawStr);
		if (!expanded.has_value()) {
			context.reportDiagnostic({
				"netease_download_path_expand_failed",
				providerId,
				{},
				std::error_code(static_cast<int>(GetLastError()), std::system_category())
			});
			return std::nullopt;
		}
		rawStr = *expanded;
	}

	std::filesystem::path pathVal(rawStr);
	if (pathVal.empty() || !pathVal.is_absolute()) {
		context.reportDiagnostic({
			"netease_download_path_invalid",
			providerId,
			{},
			{}
		});
		return std::nullopt;
	}

	return pathVal;
}
#endif

} // namespace

NeteaseMinecraftDiscoveryProvider::NeteaseMinecraftDiscoveryProvider(
	NeteaseDiscoveryDependencies dependencies)
	: dependencies_(std::move(dependencies)) {}

std::vector<DiscoveryLocation> NeteaseMinecraftDiscoveryProvider::DiscoverLocations(
	const MinecraftDiscoveryContext& context,
	std::stop_token stopToken) {
	std::vector<DiscoveryLocation> locations;
	if (stopToken.stop_requested()) return locations;

	auto readDownload = dependencies_.readDownloadPath;
#ifdef _WIN32
	if (!readDownload) {
		readDownload = ReadNeteaseDownloadPath;
	}
#endif

	auto readEnv = dependencies_.readEnvironmentPath ? dependencies_.readEnvironmentPath
		: LauncherDiscoveryUtils::ReadEnvironmentPath;
	auto isDir = dependencies_.isDirectory ? dependencies_.isDirectory
		: LauncherDiscoveryUtils::IsDirectory;

	// 1. Java 版发现（通过注册表 DownloadPath 定位 Game\.minecraft）
	if (readDownload) {
		const auto downloadPath = readDownload(context, Id());
		if (downloadPath.has_value() && !stopToken.stop_requested()) {
			const auto javaRoot = *downloadPath / "Game" / ".minecraft";
			std::error_code javaError;
			const bool exists = std::filesystem::exists(javaRoot, javaError);
			if (javaError) {
				context.reportDiagnostic({
					"netease_java_root_unreadable", Id(), javaRoot, javaError});
			}
			else if (exists) {
				if (isDir(javaRoot, javaError)) {
					DiscoveryLocation loc;
					loc.path = javaRoot;
					loc.kind = DiscoveryLocationKind::MinecraftRoot;
					loc.suggestedName = L"网易我的世界 Java版";
					loc.evidence.push_back({
						DiscoveryEvidenceKind::LauncherSettings,
						Id(),
						*downloadPath
					});
					locations.push_back(std::move(loc));
				}
				else if (javaError) {
					context.reportDiagnostic({
						"netease_java_root_unreadable", Id(), javaRoot, javaError});
				}
			}
		}
	}

	if (stopToken.stop_requested()) return locations;

	// 2. 基岩版发现（%APPDATA%\MinecraftPE_Netease\minecraftWorlds）
	if (auto appdata = readEnv("APPDATA"); appdata.has_value()) {
		const auto bedrockRoot = *appdata / "MinecraftPE_Netease" / "minecraftWorlds";
		std::error_code bedrockError;
		const bool exists = std::filesystem::exists(bedrockRoot, bedrockError);
		if (bedrockError) {
			context.reportDiagnostic({
				"netease_bedrock_root_unreadable", Id(), bedrockRoot, bedrockError});
		}
		else if (exists) {
			if (isDir(bedrockRoot, bedrockError)) {
				DiscoveryLocation loc;
				loc.path = bedrockRoot;
				loc.kind = DiscoveryLocationKind::BedrockWorldsRoot;
				loc.suggestedName = L"网易我的世界 基岩版";
				loc.evidence.push_back({
					DiscoveryEvidenceKind::KnownLocation,
					Id(),
					bedrockRoot
				});
				locations.push_back(std::move(loc));
			}
			else if (bedrockError) {
				context.reportDiagnostic({
					"netease_bedrock_root_unreadable", Id(), bedrockRoot, bedrockError});
			}
		}
	}

	return locations;
}
