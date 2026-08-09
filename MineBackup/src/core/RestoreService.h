#pragma once

#include "AppPaths.h"
#include "ArchiveRunner.h"
#include "BackupService.h"
#include "OperationResult.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <vector>

enum class RestoreMode {
	Clean,
	Overwrite
};

struct RestoreRequest {
	Config config;
	WorldRef world;
	std::filesystem::path archive;
	std::vector<std::wstring> restorePreserve;
	RestoreMode mode = RestoreMode::Clean;
};

struct RestorePlan {
	OperationCode code = OperationCode::RestoreFailed;
	std::filesystem::path targetWorld;
	std::filesystem::path selectedArchive;
	std::vector<std::filesystem::path> archiveChain;
	RestoreMode mode = RestoreMode::Clean;
	std::size_t checkedArchiveCount = 0;
	bool usesExactSmartPlan = false;
	std::vector<Diagnostic> diagnostics;
};

struct RestoreResult {
	OperationCode code = OperationCode::RestoreFailed;
	RestorePlan plan;
	bool dryRun = false;
	bool rollbackAttempted = false;
	bool rollbackSucceeded = false;
	std::optional<BackupResult> safetyBackup;
	std::vector<Diagnostic> diagnostics;
};

struct RestoreServiceDependencies {
	AppPaths paths;
	std::function<bool(const std::filesystem::path&)> isWorldOccupied;
	std::function<BackupResult(const BackupRequest&, std::stop_token)> backupBeforeRestore;
	std::function<ArchiveRunner(
		const std::filesystem::path&,
		const AppPaths&,
		std::stop_token)> archiveRunnerFactory;
};

class RestoreService {
public:
	explicit RestoreService(RestoreServiceDependencies dependencies);

	RestorePlan Verify(
		const RestoreRequest& request,
		std::stop_token stopToken = {}) const;
	RestoreResult Run(
		const RestoreRequest& request,
		bool dryRun,
		std::stop_token stopToken = {}) const;

private:
	RestorePlan BuildAndVerify(
		const RestoreRequest& request,
		bool requireColdWorld,
		std::stop_token stopToken) const;

	RestoreServiceDependencies dependencies_;
};

const char* ToString(RestoreMode mode) noexcept;
