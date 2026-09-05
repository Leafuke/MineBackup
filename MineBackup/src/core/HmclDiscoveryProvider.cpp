#include "HmclDiscoveryProvider.h"

#include "LauncherDiscoveryUtils.h"
#include "PathIdentity.h"
#include "text_to_text.h"
#include "json.hpp"

#include <set>
#include <system_error>

namespace {

// 解析 HMCL user home 候选路径并去重
std::vector<std::filesystem::path> ResolveHmclUserHomeCandidates() {
	std::vector<std::filesystem::path> candidates;

	// 1. 优先检查 HMCL_USER_HOME 环境变量覆盖（必须为绝对路径）
	if (auto envOverride = LauncherDiscoveryUtils::ReadEnvironmentPath("HMCL_USER_HOME");
		envOverride.has_value() && envOverride->is_absolute()) {
		candidates.push_back(std::move(*envOverride));
	}

	// 2. 各操作系统的标准候选路径
#ifdef _WIN32
	if (auto appdata = LauncherDiscoveryUtils::ReadEnvironmentPath("APPDATA"); appdata.has_value()) {
		candidates.push_back(*appdata / ".hmcl");
	}
#elif defined(__APPLE__)
	if (auto home = LauncherDiscoveryUtils::ResolveHomeDirectory(); home.has_value()) {
		candidates.push_back(*home / "Library" / "Application Support" / "hmcl");
	}
#else
	if (auto xdg = LauncherDiscoveryUtils::ReadEnvironmentPath("XDG_DATA_HOME");
		xdg.has_value() && xdg->is_absolute()) {
		candidates.push_back(*xdg / "hmcl");
	}
	if (auto home = LauncherDiscoveryUtils::ResolveHomeDirectory(); home.has_value()) {
		candidates.push_back(*home / ".local" / "share" / "hmcl");
	}
#endif
	return candidates;
}

// 解析 HMCL 条目中的 name 字段（支持纯字符串与本地化多语言对象）
std::optional<std::wstring> ParseHmclName(const nlohmann::json& entry) {
	if (!entry.contains("name")) return std::nullopt;
	const auto& nameField = entry["name"];

	if (nameField.is_string()) {
		const std::string val = nameField.get<std::string>();
		if (!val.empty()) {
			std::wstring wide = utf8_to_wstring(val);
			if (!wide.empty()) return wide;
		}
		return std::nullopt;
	}

	if (nameField.is_object()) {
		// 优先取 default 字段
		if (nameField.contains("default") && nameField["default"].is_string()) {
			const std::string val = nameField["default"].get<std::string>();
			if (!val.empty()) {
				std::wstring wide = utf8_to_wstring(val);
				if (!wide.empty()) return wide;
			}
		}
		// 否则取首个非空有效字符串
		for (auto it = nameField.begin(); it != nameField.end(); ++it) {
			if (it.value().is_string()) {
				const std::string val = it.value().get<std::string>();
				if (!val.empty()) {
					std::wstring wide = utf8_to_wstring(val);
					if (!wide.empty()) return wide;
				}
			}
		}
	}
	return std::nullopt;
}

} // namespace

std::vector<DiscoveryLocation> HmclDiscoveryProvider::DiscoverLocations(
	const MinecraftDiscoveryContext& context,
	std::stop_token stopToken) {
	std::vector<DiscoveryLocation> locations;
	if (stopToken.stop_requested()) return locations;

	const auto candidateHomes = ResolveHmclUserHomeCandidates();
	std::set<std::wstring> seenHomes;
	std::set<std::wstring> seenGameDirectories;

	for (const auto& home : candidateHomes) {
		if (stopToken.stop_requested()) break;
		const auto homeKey = PathIdentity::BuildPathIdentityKey(home);
		if (!seenHomes.insert(homeKey).second) continue;

		const auto configPath = home / "config" / "user-game-directories.json";
		const auto fileResult = LauncherDiscoveryUtils::ReadBoundedTextFile(configPath);

		if (fileResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::Missing) {
			// 配置文件不存在属于正常情况，静默跳过
			continue;
		}
		if (fileResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::TooLarge) {
			context.reportDiagnostic({
				"hmcl_config_too_large", Id(), configPath, {}});
			continue;
		}
		if (fileResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::ReadFailed
			|| fileResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::NotRegular) {
			context.reportDiagnostic({
				"hmcl_config_read_failed", Id(), configPath, fileResult.filesystemError});
			continue;
		}

		// 解析 JSON
		nlohmann::json rootJson = nlohmann::json::parse(fileResult.contents, nullptr, false);
		if (rootJson.is_discarded() || !rootJson.is_object()) {
			context.reportDiagnostic({
				"hmcl_config_malformed", Id(), configPath, {}});
			continue;
		}

		if (!rootJson.contains("directories") || !rootJson["directories"].is_array()) {
			continue;
		}

		for (const auto& entry : rootJson["directories"]) {
			if (stopToken.stop_requested()) break;

			if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string()) {
				context.reportDiagnostic({
					"hmcl_game_directory_invalid", Id(), configPath, {}});
				continue;
			}

			const std::string rawPath = entry["path"].get<std::string>();
			if (rawPath.empty()) {
				context.reportDiagnostic({
					"hmcl_game_directory_invalid", Id(), configPath, {}});
				continue;
			}

#ifdef _WIN32
			const std::filesystem::path dirPath(utf8_to_wstring(rawPath));
#else
			const std::filesystem::path dirPath(rawPath);
#endif

			if (dirPath.empty() || !dirPath.is_absolute()) {
				context.reportDiagnostic({
					"hmcl_game_directory_invalid", Id(), configPath, {}});
				continue;
			}

			std::error_code dirError;
			const bool exists = std::filesystem::exists(dirPath, dirError);
			if (dirError) {
				context.reportDiagnostic({
					"hmcl_game_directory_unreadable", Id(), dirPath, dirError});
				continue;
			}
			if (!exists) {
				// 候选游戏目录不存在是正常情况，静默跳过
				continue;
			}
			if (!LauncherDiscoveryUtils::IsDirectory(dirPath, dirError)) {
				if (dirError) {
					context.reportDiagnostic({
						"hmcl_game_directory_unreadable", Id(), dirPath, dirError});
				}
				continue;
			}

			const auto dirKey = PathIdentity::BuildPathIdentityKey(dirPath);
			if (!seenGameDirectories.insert(dirKey).second) {
				continue;
			}

			DiscoveryLocation location;
			location.path = dirPath;
			location.kind = DiscoveryLocationKind::MinecraftRoot;
			location.suggestedName = ParseHmclName(entry);
			location.evidence.push_back({
				DiscoveryEvidenceKind::LauncherSettings,
				Id(),
				configPath
			});

			locations.push_back(std::move(location));
		}
	}

	return locations;
}
