#pragma once

#include "AppPaths.h"
#include "JobModels.h"
#include "OperationResult.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct ServerProfileManifest {
	static constexpr int SchemaVersion = 1;
	int schemaVersion = SchemaVersion;
	std::vector<std::wstring> restorePreserve;
	std::vector<Config> configs;
	JobDocument jobs;
};

enum class ProfileManifestStatus {
	Loaded,
	Missing,
	Invalid,
	UnsupportedSchema,
	IoError
};

struct ProfileManifestLoadResult {
	ProfileManifestStatus status = ProfileManifestStatus::Missing;
	ServerProfileManifest manifest;
	std::vector<Diagnostic> diagnostics;

	bool IsLoaded() const noexcept { return status == ProfileManifestStatus::Loaded; }
};

enum class ProfileDiffAction {
	Add,
	Update,
	Remove
};

struct ProfileDiffItem {
	std::string kind;
	std::wstring stableId;
	ProfileDiffAction action = ProfileDiffAction::Add;
	std::size_t orphanHistoryCount = 0;
};

struct ProfileApplyPlan {
	OperationCode code = OperationCode::InvalidProfile;
	std::map<int, Config> configs;
	JobDocument jobs;
	std::vector<std::wstring> restorePreserve;
	std::vector<ProfileDiffItem> diff;
	std::vector<Diagnostic> diagnostics;
};

struct ProfileApplyResult {
	OperationCode code = OperationCode::InvalidProfile;
	std::vector<ProfileDiffItem> diff;
	std::vector<Diagnostic> diagnostics;
};

namespace ProfileManifest {

ServerProfileManifest CreateTemplate();
ProfileManifestLoadResult Parse(
	const std::string& content,
	const std::filesystem::path& manifestDirectory);
ProfileManifestLoadResult Load(const std::filesystem::path& path);
std::string Serialize(const ServerProfileManifest& manifest);

ProfileApplyPlan Plan(
	const AppPaths& paths,
	const ServerProfileManifest& manifest,
	bool prune);
ProfileApplyResult Apply(const AppPaths& paths, const ProfileApplyPlan& plan);
ProfileManifestLoadResult Export(const AppPaths& paths);

const char* ToString(ProfileDiffAction action) noexcept;

} // namespace ProfileManifest
