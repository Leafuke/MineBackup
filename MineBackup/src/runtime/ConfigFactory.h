#pragma once

#include "DataModels.h"
#include "MinecraftTypes.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct ConfigDraft {
	std::string name;
	MinecraftEdition edition = MinecraftEdition::Unknown;
	std::filesystem::path saveRoot;
	std::vector<std::pair<std::wstring, std::wstring>> worlds;
	std::filesystem::path backupPath;
};

struct ConfigFactoryContext {
	std::filesystem::path resolvedSevenZip;
};

Config BuildRecommendedConfig(
	const ConfigDraft& draft,
	const ConfigFactoryContext& context);

std::optional<ConfigDraft> BuildCustomFolderDraft(
	const std::filesystem::path& folder);

std::vector<ConfigDraft> ResolveUniqueConfigDrafts(
	const std::vector<ConfigDraft>& drafts,
	const std::filesystem::path& defaultBackupRoot,
	const std::map<int, Config>& existingConfigs);

std::vector<std::wstring> RecommendedConfigBackupBlacklist();
