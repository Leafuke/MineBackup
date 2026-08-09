#pragma once

#include "DataModels.h"
#include "OperationResult.h"

#include <filesystem>
#include <map>
#include <optional>

enum class ProfileCatalogStatus {
	Loaded,
	Missing,
	Invalid,
	MigrationRequired
};

struct ProfileConfigCatalog {
	std::map<int, Config> configs;

	const Config* FindConfig(const std::wstring& configId) const;
	std::map<int, Config> ConfigSnapshot() const { return configs; }
};

struct ProfileCatalogLoadResult {
	ProfileCatalogStatus status = ProfileCatalogStatus::Missing;
	ProfileConfigCatalog catalog;
	std::vector<Diagnostic> diagnostics;

	bool IsLoaded() const { return status == ProfileCatalogStatus::Loaded; }
};

class ProfileConfigCatalogLoader {
public:
	static ProfileCatalogLoadResult Load(
		const std::filesystem::path& configFile);
};

