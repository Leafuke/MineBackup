#include "KnownMinecraftLocationProvider.h"

#include "PathIdentity.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <utility>

#ifdef _WIN32
#include <wchar.h>
#endif

using namespace std;

namespace {

optional<filesystem::path> ReadEnvironmentPath(string_view name) {
#ifdef _WIN32
	// Windows 环境变量原生值是 UTF-16；_wgetenv 全程保持宽字符，
	// 直接构造 filesystem::path，不经任何窄字符串/代码页转换。
	// APPDATA / LOCALAPPDATA / HOME 变量名均为 ASCII，宽字符名无需 locale 逻辑。
	const wstring wideName(name.begin(), name.end());
	const wchar_t* value = _wgetenv(wideName.c_str());
	return value && *value
		? optional<filesystem::path>(filesystem::path(value))
		: nullopt;
#else
	const string variable(name);
	const char* value = getenv(variable.c_str());
	return value && *value
		? optional<filesystem::path>(filesystem::path(value))
		: nullopt;
#endif
}

bool IsDirectory(const filesystem::path& path, error_code& error) {
	error.clear();
	return filesystem::is_directory(path, error) && !error;
}

vector<filesystem::path> ListChildDirectories(
	const filesystem::path& root,
	error_code& error) {
	vector<filesystem::path> children;
	error.clear();
	filesystem::directory_iterator iterator(
		root, filesystem::directory_options::skip_permission_denied, error);
	if (error) return children;
	const filesystem::directory_iterator end;
	for (; iterator != end; iterator.increment(error)) {
		if (error) break;
		error_code statusError;
		if (iterator->is_directory(statusError) && !statusError) {
			children.push_back(iterator->path());
		}
	}
	return children;
}

MinecraftHostPlatform CurrentPlatform() noexcept {
#ifdef _WIN32
	return MinecraftHostPlatform::Windows;
#elif defined(__APPLE__)
	return MinecraftHostPlatform::MacOS;
#else
	return MinecraftHostPlatform::Linux;
#endif
}

bool IsNumericDirectoryName(const filesystem::path& path) {
	const wstring name = path.filename().wstring();
	return !name.empty() && all_of(name.begin(), name.end(), [](wchar_t character) {
		return character >= L'0' && character <= L'9';
	});
}

bool IsMissing(error_code error) {
	return error == make_error_code(errc::no_such_file_or_directory)
		|| error == make_error_code(errc::not_a_directory);
}

void Report(
	const MinecraftDiscoveryContext& context,
	string code,
	const filesystem::path& path,
	error_code error = {}) {
	if (context.reportDiagnostic) {
		context.reportDiagnostic({std::move(code), "known-locations", path, error});
	}
}

} // namespace

KnownMinecraftLocationProvider::KnownMinecraftLocationProvider(
	KnownMinecraftLocationDependencies dependencies)
	: dependencies_(std::move(dependencies)) {
	if (dependencies_.platform == MinecraftHostPlatform::Current) {
		dependencies_.platform = CurrentPlatform();
	}
	if (!dependencies_.readEnvironmentPath) {
		dependencies_.readEnvironmentPath = ReadEnvironmentPath;
	}
	if (!dependencies_.isDirectory) dependencies_.isDirectory = IsDirectory;
	if (!dependencies_.listChildDirectories) {
		dependencies_.listChildDirectories = ListChildDirectories;
	}
}

string KnownMinecraftLocationProvider::Id() const {
	return "known-locations";
}

vector<DiscoveryLocation> KnownMinecraftLocationProvider::DiscoverLocations(
	const MinecraftDiscoveryContext& context,
	stop_token stopToken) {
	vector<DiscoveryLocation> locations;
	set<wstring> seen;
	const auto add = [&](const filesystem::path& path, DiscoveryLocationKind kind) {
		if (stopToken.stop_requested() || path.empty() || !path.is_absolute()) return;
		error_code error;
		if (!dependencies_.isDirectory(path, error)) {
			if (error && !IsMissing(error)) {
				Report(context, "known_location_unreadable", path, error);
			}
			return;
		}
		const wstring key = PathIdentity::BuildPathIdentityKey(path);
		if (!seen.insert(key).second) return;
		locations.push_back({path, kind, {{
			DiscoveryEvidenceKind::KnownLocation, Id(), path}}});
	};
	// 依赖注入已直接返回 filesystem::path；这里不再有窄字符串二次转换。
	const auto environmentPath = [&](string_view name) -> filesystem::path {
		const auto value = dependencies_.readEnvironmentPath(name);
		return value ? *value : filesystem::path{};
	};

	if (dependencies_.platform == MinecraftHostPlatform::Windows) {
		const auto appData = environmentPath("APPDATA");
		const auto localAppData = environmentPath("LOCALAPPDATA");
		if (!appData.empty()) {
			add(appData / L".minecraft", DiscoveryLocationKind::MinecraftRoot);
			const auto usersRoot = appData / L"Minecraft Bedrock" / L"Users";
			error_code error;
			const auto users = dependencies_.listChildDirectories(usersRoot, error);
			if (error && !IsMissing(error)) {
				Report(context, "known_bedrock_users_unreadable", usersRoot, error);
			}
			for (const auto& user : users) {
				if (stopToken.stop_requested()) break;
				if (IsNumericDirectoryName(user)) {
					add(user / L"games" / L"com.mojang" / L"minecraftWorlds",
						DiscoveryLocationKind::BedrockWorldsRoot);
				}
			}
		}
		if (!localAppData.empty()) {
			add(localAppData / L"Packages" / L"Microsoft.MinecraftUWP_8wekyb3d8bbwe"
				/ L"LocalState" / L"games" / L"com.mojang" / L"minecraftWorlds",
				DiscoveryLocationKind::BedrockWorldsRoot);
		}
	}
	else {
		const auto home = environmentPath("HOME");
		if (!home.empty()) {
			if (dependencies_.platform == MinecraftHostPlatform::MacOS) {
				add(home / L"Library" / L"Application Support" / L"minecraft",
					DiscoveryLocationKind::MinecraftRoot);
			}
			else {
				// Linux 只探测明确约定位置，不向 HOME 做递归扫描。
				add(home / L".minecraft", DiscoveryLocationKind::MinecraftRoot);
				add(home / L".var" / L"app" / L"com.mojang.Minecraft"
					/ L".minecraft", DiscoveryLocationKind::MinecraftRoot);
				add(home / L".local" / L"share" / L"minecraft",
					DiscoveryLocationKind::MinecraftRoot);
				add(home / L".local" / L"share" / L"mcpelauncher" / L"games"
					/ L"com.mojang" / L"minecraftWorlds",
					DiscoveryLocationKind::BedrockWorldsRoot);
				add(home / L".var" / L"app" / L"io.mrarm.mcpelauncher" / L".local"
					/ L"share" / L"mcpelauncher" / L"games" / L"com.mojang"
					/ L"minecraftWorlds", DiscoveryLocationKind::BedrockWorldsRoot);
			}
		}
	}
	return locations;
}
