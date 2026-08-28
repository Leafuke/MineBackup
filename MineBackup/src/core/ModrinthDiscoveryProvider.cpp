#include "ModrinthDiscoveryProvider.h"

#include "LauncherDiscoveryUtils.h"
#include "PathIdentity.h"

#include <set>
#include <system_error>

namespace {

// 解析新版 ModrinthApp 数据目录候选
std::vector<std::filesystem::path> ResolveNewModrinthDataRoots() {
	std::vector<std::filesystem::path> candidates;

#ifdef _WIN32
	if (auto appdata = LauncherDiscoveryUtils::ReadEnvironmentPath("APPDATA"); appdata.has_value()) {
		candidates.push_back(*appdata / "ModrinthApp");
	}
#elif defined(__APPLE__)
	if (auto home = LauncherDiscoveryUtils::ResolveHomeDirectory(); home.has_value()) {
		candidates.push_back(*home / "Library" / "Application Support" / "ModrinthApp");
	}
#else
	if (auto xdg = LauncherDiscoveryUtils::ReadEnvironmentPath("XDG_DATA_HOME");
		xdg.has_value() && xdg->is_absolute()) {
		candidates.push_back(*xdg / "ModrinthApp");
	}
	if (auto home = LauncherDiscoveryUtils::ResolveHomeDirectory(); home.has_value()) {
		candidates.push_back(*home / ".local" / "share" / "ModrinthApp");
	}
#endif

	return candidates;
}

// 解析旧版 com.modrinth.theseus 数据目录候选
std::vector<std::filesystem::path> ResolveLegacyModrinthDataRoots() {
	std::vector<std::filesystem::path> candidates;

#ifdef _WIN32
	if (auto appdata = LauncherDiscoveryUtils::ReadEnvironmentPath("APPDATA"); appdata.has_value()) {
		candidates.push_back(*appdata / "com.modrinth.theseus");
	}
#elif defined(__APPLE__)
	if (auto home = LauncherDiscoveryUtils::ResolveHomeDirectory(); home.has_value()) {
		candidates.push_back(*home / "Library" / "Application Support" / "com.modrinth.theseus");
	}
#else
	if (auto xdg = LauncherDiscoveryUtils::ReadEnvironmentPath("XDG_DATA_HOME");
		xdg.has_value() && xdg->is_absolute()) {
		candidates.push_back(*xdg / "com.modrinth.theseus");
	}
	if (auto home = LauncherDiscoveryUtils::ResolveHomeDirectory(); home.has_value()) {
		candidates.push_back(*home / ".local" / "share" / "com.modrinth.theseus");
	}
#endif

	return candidates;
}

} // namespace

std::vector<DiscoveryLocation> ModrinthDiscoveryProvider::DiscoverLocations(
	const MinecraftDiscoveryContext& context,
	std::stop_token stopToken) {
	std::vector<DiscoveryLocation> locations;
	if (stopToken.stop_requested()) return locations;

	std::vector<std::filesystem::path> activeProfilesDirs;
	bool foundNewProfiles = false;
	std::set<std::wstring> seenProfilesDirs;

	// 1. 优先探测新版 ModrinthApp/profiles
	for (const auto& root : ResolveNewModrinthDataRoots()) {
		if (stopToken.stop_requested()) break;

		const auto profilesDir = root / "profiles";
		const auto key = PathIdentity::BuildPathIdentityKey(profilesDir);
		if (!seenProfilesDirs.insert(key).second) continue;

		std::error_code ec;
		const bool exists = std::filesystem::exists(profilesDir, ec);
		if (ec) {
			context.reportDiagnostic({
				"modrinth_profiles_unreadable", Id(), profilesDir, ec});
			foundNewProfiles = true; // 存在但不可读，按规范不自动回退旧版，避免迁移残留
			continue;
		}
		if (exists) {
			if (LauncherDiscoveryUtils::IsDirectory(profilesDir, ec)) {
				activeProfilesDirs.push_back(profilesDir);
				foundNewProfiles = true;
			}
			else if (ec) {
				context.reportDiagnostic({
					"modrinth_profiles_unreadable", Id(), profilesDir, ec});
				foundNewProfiles = true;
			}
		}
	}

	// 2. 仅当新版数据根不存在 profiles 时，回退尝试旧版 com.modrinth.theseus/profiles
	if (!foundNewProfiles && activeProfilesDirs.empty()) {
		for (const auto& root : ResolveLegacyModrinthDataRoots()) {
			if (stopToken.stop_requested()) break;

			const auto profilesDir = root / "profiles";
			const auto key = PathIdentity::BuildPathIdentityKey(profilesDir);
			if (!seenProfilesDirs.insert(key).second) continue;

			std::error_code ec;
			const bool exists = std::filesystem::exists(profilesDir, ec);
			if (ec) {
				context.reportDiagnostic({
					"modrinth_profiles_unreadable", Id(), profilesDir, ec});
				continue;
			}
			if (exists) {
				if (LauncherDiscoveryUtils::IsDirectory(profilesDir, ec)) {
					activeProfilesDirs.push_back(profilesDir);
				}
				else if (ec) {
					context.reportDiagnostic({
						"modrinth_profiles_unreadable", Id(), profilesDir, ec});
				}
			}
		}
	}

	// 3. 枚举 profiles 下的直接子目录作为实例根
	std::set<std::wstring> seenInstances;
	for (const auto& profilesDir : activeProfilesDirs) {
		if (stopToken.stop_requested()) break;

		std::error_code listError;
		const auto children = LauncherDiscoveryUtils::ListChildDirectories(profilesDir, listError);
		if (listError) {
			if (!LauncherDiscoveryUtils::IsMissingFilesystemError(listError)) {
				context.reportDiagnostic({
					"modrinth_profiles_unreadable", Id(), profilesDir, listError});
			}
			continue;
		}

		for (const auto& child : children) {
			if (stopToken.stop_requested()) break;

			std::error_code childError;
			if (!LauncherDiscoveryUtils::IsDirectory(child, childError)) {
				if (childError) {
					context.reportDiagnostic({
						"modrinth_profile_unreadable", Id(), child, childError});
				}
				continue;
			}

			const auto childKey = PathIdentity::BuildPathIdentityKey(child);
			if (!seenInstances.insert(childKey).second) continue;

			DiscoveryLocation location;
			location.path = child;
			location.kind = DiscoveryLocationKind::MinecraftRoot;
			location.suggestedName = child.filename().wstring();
			location.evidence.push_back({
				DiscoveryEvidenceKind::KnownLocation,
				Id(),
				profilesDir
			});

			locations.push_back(std::move(location));
		}
	}

	return locations;
}
