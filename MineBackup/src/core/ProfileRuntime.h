#pragma once

#include "AppPaths.h"
#include "HistoryRepository.h"
#include "JobModels.h"
#include "OperationResult.h"
#include "ProfileConfigCatalog.h"
#include "RestoreService.h"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

struct ProfileRuntimeDependencies {
	bool noNetwork = false;
	std::function<bool(std::stop_token, std::wstring&)> ensureSevenZip;
};

struct ProfileRuntimeInitialization {
	OperationCode code = OperationCode::InvalidProfile;
	std::vector<Diagnostic> diagnostics;
};

struct ProfileRuntimePreflight {
	OperationCode code = OperationCode::Success;
	std::vector<Diagnostic> diagnostics;
};

// Owns the mutable services for one profile. A command-line invocation may
// keep it for one operation; serve keeps the same object for its lifetime.
class ProfileRuntime {
public:
	ProfileRuntime(AppPaths paths, ProfileRuntimeDependencies dependencies = {});
	~ProfileRuntime();
	ProfileRuntime(const ProfileRuntime&) = delete;
	ProfileRuntime& operator=(const ProfileRuntime&) = delete;

	ProfileRuntimeInitialization Reload();
	bool IsReady() const noexcept;

	const AppPaths& Paths() const noexcept;
	const ProfileConfigCatalog& Catalog() const;
	const JobDocument& Jobs() const;
	const HistoryRepository& History() const;

	std::optional<BackupRequest> ResolveBackup(
		const std::wstring& configId,
		const std::wstring& worldPath,
		const std::wstring& comment = {}) const;
	ProfileRuntimePreflight PreflightBackup(
		const BackupRequest& request,
		std::stop_token stopToken = {}) const;
	BackupResult RunBackup(
		const std::wstring& configId,
		const std::wstring& worldPath,
		const std::wstring& comment = {},
		std::stop_token stopToken = {}) const;
	JobRunResult RunJob(
		const std::wstring& jobId,
		std::stop_token stopToken = {}) const;
	RestorePlan Verify(
		const RestoreRequest& request,
		std::stop_token stopToken = {}) const;
	RestoreResult Restore(
		const RestoreRequest& request,
		bool dryRun,
		std::stop_token stopToken = {}) const;

private:
	struct Implementation;
	AppPaths paths_;
	ProfileRuntimeDependencies dependencies_;
	std::unique_ptr<Implementation> implementation_;
};
