#include "PrismLauncherDiscoveryProvider.h"

#include "LauncherDiscoveryUtils.h"
#include "PathIdentity.h"
#include "text_to_text.h"

#include <cctype>
#include <set>
#include <system_error>

namespace {

// ASCII 不区分大小写字符串比对
bool EqualsAsciiIgnoreCase(std::string_view left, std::string_view right) {
	if (left.size() != right.size()) return false;
	for (std::size_t i = 0; i < left.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(left[i])) !=
			std::tolower(static_cast<unsigned char>(right[i]))) {
			return false;
		}
	}
	return true;
}

// 辅助去除字符串首尾空白及包围引号
std::string_view Trim(std::string_view s) {
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
		s.remove_prefix(1);
	}
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
		s.remove_suffix(1);
	}
	if (s.size() >= 2) {
		if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')) {
			s.remove_prefix(1);
			s.remove_suffix(1);
			s = Trim(s);
		}
	}
	return s;
}

// 简单的 INI 键值提取
std::optional<std::string> FindIniValue(std::string_view content, std::string_view targetKey) {
	std::size_t pos = 0;
	while (pos < content.size()) {
		std::size_t endOfLine = content.find_first_of("\r\n", pos);
		if (endOfLine == std::string_view::npos) endOfLine = content.size();
		std::string_view line = content.substr(pos, endOfLine - pos);
		line = Trim(line);
		if (!line.empty() && line.front() != '#' && line.front() != ';') {
			const std::size_t eqPos = line.find('=');
			if (eqPos != std::string_view::npos) {
				const std::string_view key = Trim(line.substr(0, eqPos));
				const std::string_view val = Trim(line.substr(eqPos + 1));
				if (EqualsAsciiIgnoreCase(key, targetKey)) {
					return std::string(val);
				}
			}
		}
		pos = endOfLine;
		while (pos < content.size() && (content[pos] == '\r' || content[pos] == '\n')) {
			++pos;
		}
	}
	return std::nullopt;
}

// 解析 Prism Launcher 数据根目录候选路径
std::vector<std::filesystem::path> ResolvePrismDataRootCandidates() {
	std::vector<std::filesystem::path> candidates;

#ifdef _WIN32
	if (auto appdata = LauncherDiscoveryUtils::ReadEnvironmentPath("APPDATA"); appdata.has_value()) {
		candidates.push_back(*appdata / "PrismLauncher");
	}
	if (auto userProfile = LauncherDiscoveryUtils::ReadEnvironmentPath("USERPROFILE"); userProfile.has_value()) {
		candidates.push_back(*userProfile / "scoop" / "persist" / "prismlauncher");
	}
#elif defined(__APPLE__)
	if (auto home = LauncherDiscoveryUtils::ResolveHomeDirectory(); home.has_value()) {
		candidates.push_back(*home / "Library" / "Application Support" / "PrismLauncher");
	}
#else
	if (auto xdg = LauncherDiscoveryUtils::ReadEnvironmentPath("XDG_DATA_HOME");
		xdg.has_value() && xdg->is_absolute()) {
		candidates.push_back(*xdg / "PrismLauncher");
	}
	if (auto home = LauncherDiscoveryUtils::ResolveHomeDirectory(); home.has_value()) {
		candidates.push_back(*home / ".local" / "share" / "PrismLauncher");
		// Flatpak 安装路径
		candidates.push_back(
			*home / ".var" / "app" / "org.prismlauncher.PrismLauncher" / "data" / "PrismLauncher");
	}
#endif

	return candidates;
}

// 解析实例目录路径
std::filesystem::path ResolveInstanceDir(
	const std::filesystem::path& dataRoot,
	const MinecraftDiscoveryContext& context,
	const std::string& providerId,
	bool& hasGlobalConfigEvidence,
	std::filesystem::path& globalConfigPath) {
	hasGlobalConfigEvidence = false;
	const auto configPath = dataRoot / "prismlauncher.cfg";
	const auto defaultInstances = dataRoot / "instances";
	const auto configResult = LauncherDiscoveryUtils::ReadBoundedTextFile(configPath);

	if (configResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::Missing) {
		// 配置文件不存在属于正常情况，静默回退默认 instances
		return defaultInstances;
	}
	if (configResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::TooLarge) {
		context.reportDiagnostic({
			"prism_config_too_large", providerId, configPath, {}});
		return defaultInstances;
	}
	if (configResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::ReadFailed
		|| configResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::NotRegular) {
		context.reportDiagnostic({
			"prism_config_read_failed", providerId, configPath, configResult.filesystemError});
		return defaultInstances;
	}

	// 配置文件成功读取，尝试解析 InstanceDir 键
	auto instanceDirValue = FindIniValue(configResult.contents, "InstanceDir");
	if (!instanceDirValue.has_value() || instanceDirValue->empty()) {
		return defaultInstances;
	}

#ifdef _WIN32
	const std::filesystem::path parsedPath(utf8_to_wstring(*instanceDirValue));
#else
	const std::filesystem::path parsedPath(*instanceDirValue);
#endif

	if (parsedPath.empty()) {
		context.reportDiagnostic({
			"prism_instance_dir_invalid", providerId, configPath, {}});
		return defaultInstances;
	}

	globalConfigPath = configPath;
	hasGlobalConfigEvidence = true;

	if (parsedPath.is_absolute()) {
		return parsedPath;
	}
	return dataRoot / parsedPath;
}

} // namespace

std::vector<DiscoveryLocation> PrismLauncherDiscoveryProvider::DiscoverLocations(
	const MinecraftDiscoveryContext& context,
	std::stop_token stopToken) {
	std::vector<DiscoveryLocation> locations;
	if (stopToken.stop_requested()) return locations;

	const auto candidates = ResolvePrismDataRootCandidates();
	std::set<std::wstring> seenRoots;
	std::set<std::wstring> seenGameRoots;

	for (const auto& dataRoot : candidates) {
		if (stopToken.stop_requested()) break;

		std::error_code rootError;
		if (!LauncherDiscoveryUtils::IsDirectory(dataRoot, rootError)) {
			continue;
		}

		const auto rootKey = PathIdentity::BuildPathIdentityKey(dataRoot);
		if (!seenRoots.insert(rootKey).second) continue;

		bool hasGlobalConfigEvidence = false;
		std::filesystem::path globalConfigPath;
		const auto instanceDir = ResolveInstanceDir(
			dataRoot, context, Id(), hasGlobalConfigEvidence, globalConfigPath);

		std::error_code listError;
		const auto childDirs = LauncherDiscoveryUtils::ListChildDirectories(instanceDir, listError);
		if (listError) {
			if (!LauncherDiscoveryUtils::IsMissingFilesystemError(listError)) {
				context.reportDiagnostic({
					"prism_instances_unreadable", Id(), instanceDir, listError});
			}
			continue;
		}

		for (const auto& child : childDirs) {
			if (stopToken.stop_requested()) break;

			const auto instanceCfgPath = child / "instance.cfg";
			const auto cfgResult = LauncherDiscoveryUtils::ReadBoundedTextFile(instanceCfgPath);

			if (cfgResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::Missing) {
				// 缺少 instance.cfg 说明不是 Prism 实例目录，静默跳过
				continue;
			}
			if (cfgResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::TooLarge) {
				context.reportDiagnostic({
					"prism_instance_config_too_large", Id(), instanceCfgPath, {}});
			}
			else if (cfgResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::ReadFailed
				|| cfgResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::NotRegular) {
				context.reportDiagnostic({
					"prism_instance_config_read_failed", Id(), instanceCfgPath, cfgResult.filesystemError});
			}

			// 检查游戏根目录：优先 Prism 9+ 的 minecraft 目录，回退旧版 .minecraft
			std::error_code dirError;
			std::filesystem::path gameRoot;
			if (LauncherDiscoveryUtils::IsDirectory(child / "minecraft", dirError)) {
				gameRoot = child / "minecraft";
			}
			else if (LauncherDiscoveryUtils::IsDirectory(child / ".minecraft", dirError)) {
				gameRoot = child / ".minecraft";
			}
			else {
				// 未找到游戏子目录，跳过
				continue;
			}

			// 提取实例名称
			std::optional<std::wstring> suggestedName;
			if (cfgResult.status == LauncherDiscoveryUtils::BoundedTextFileStatus::Loaded) {
				auto nameVal = FindIniValue(cfgResult.contents, "name");
				if (nameVal.has_value() && !nameVal->empty()) {
					suggestedName = utf8_to_wstring(*nameVal);
				}
			}
			if (!suggestedName.has_value() || suggestedName->empty()) {
				suggestedName = child.filename().wstring();
			}

			const auto gameRootKey = PathIdentity::BuildPathIdentityKey(gameRoot);
			if (!seenGameRoots.insert(gameRootKey).second) {
				continue;
			}

			DiscoveryLocation location;
			location.path = gameRoot;
			location.kind = DiscoveryLocationKind::MinecraftRoot;
			location.suggestedName = std::move(suggestedName);
			location.evidence.push_back({
				DiscoveryEvidenceKind::LauncherSettings,
				Id(),
				instanceCfgPath
			});
			if (hasGlobalConfigEvidence) {
				location.evidence.push_back({
					DiscoveryEvidenceKind::LauncherSettings,
					Id(),
					globalConfigPath
				});
			}

			locations.push_back(std::move(location));
		}
	}

	return locations;
}
