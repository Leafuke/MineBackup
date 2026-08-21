#pragma once

#include "DataModels.h"
#include "OperationResult.h"
#include "ProfileConfigCatalog.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct ProfileConfigSnapshot {
	ProfileCatalogStatus status = ProfileCatalogStatus::Missing;
	std::map<int, Config> configs;
	std::vector<std::wstring> restorePreserve;
	std::vector<Diagnostic> diagnostics;

	bool IsUsable() const noexcept {
		return status == ProfileCatalogStatus::Loaded
			|| status == ProfileCatalogStatus::Missing;
	}
};

struct ProfileConfigWriteResult {
	bool success = false;
	std::filesystem::path backupPath;
	std::vector<Diagnostic> diagnostics;
};

// Shared persistence boundary for the server-owned part of config.ini.  It
// updates Config sections and restore-preserve rules while retaining desktop
// settings, legacy sections and unknown extension keys byte-for-byte where
// they are not owned by the server schema.
class ProfileConfigRepository {
public:
	explicit ProfileConfigRepository(std::filesystem::path configFile);

	ProfileConfigSnapshot Load() const;
	ProfileConfigWriteResult Save(
		const std::map<int, Config>& configs,
		const std::vector<std::wstring>& restorePreserve,
		bool pruneMissingConfigs) const;

	const std::filesystem::path& Path() const noexcept { return configFile_; }

private:
	std::filesystem::path configFile_;
};
