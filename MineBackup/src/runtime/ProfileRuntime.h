#pragma once

#include "AppPaths.h"
#include "HistoryRepository.h"
#include "HotRestoreCoordinator.h"
#include "JobModels.h"
#include "OperationResult.h"
#include "ProfileConfigCatalog.h"
#include "RestoreService.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
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

class ProfileKnotLinkCommands;

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
	bool NetworkEnabled() const noexcept;
	bool KnotLinkRunning() const;
	std::size_t ActiveKnotLinkOperationCount();

	const AppPaths& Paths() const noexcept;
	const ProfileConfigCatalog& Catalog() const;
	const JobDocument& Jobs() const;
	const HistoryRepository& History() const;
	ProfileConfigCatalog CatalogSnapshot() const;
	std::vector<HistoryEntry> HistorySnapshot(const std::wstring& configId) const;
	std::vector<std::wstring> RestorePreserveSnapshot() const;
	bool SetBackupImportant(
		const std::wstring& configId,
		const std::wstring& worldPath,
		const std::wstring& backupFile,
		bool important);

	std::optional<BackupRequest> ResolveBackup(
		const std::wstring& configId,
		const std::wstring& worldPath,
		const std::wstring& comment = {}) const;
	ProfileRuntimePreflight PreflightBackup(
		const BackupRequest& request,
		std::stop_token stopToken = {}) const;
	BackupResult RunBackupRequest(
		const BackupRequest& request,
		std::stop_token stopToken = {},
		bool noNetwork = false) const;
	BackupResult RunBackup(
		const std::wstring& configId,
		const std::wstring& worldPath,
		const std::wstring& comment = {},
		std::stop_token stopToken = {},
		bool noNetwork = false) const;
	JobRunResult RunJob(
		const std::wstring& jobId,
		std::stop_token stopToken = {},
		bool noNetwork = false) const;
	RestorePlan Verify(
		const RestoreRequest& request,
		std::stop_token stopToken = {}) const;
	RestoreResult Restore(
		const RestoreRequest& request,
		bool dryRun,
		std::stop_token stopToken = {},
		bool noNetwork = false) const;
	HotRestoreResult RunHotRestore(
		const HotRestoreRequest& hotRequest,
		const RestoreRequest& restoreRequest,
		std::stop_token stopToken = {});

private:
	std::optional<BackupRequest> ResolveBackupUnlocked(
		const std::wstring& configId,
		const std::wstring& worldPath,
		const std::wstring& comment) const;
	BackupResult RunBackupRequestUnlocked(
		const BackupRequest& request,
		std::stop_token stopToken,
		bool noNetwork) const;
	struct Implementation;
	AppPaths paths_;
	ProfileRuntimeDependencies dependencies_;
	mutable std::recursive_mutex operationMutex_;
	mutable std::recursive_mutex stateMutex_;
	std::unique_ptr<Implementation> implementation_;
	std::unique_ptr<ProfileKnotLinkCommands> knotLinkCommands_;
	std::atomic<bool> knotLinkRunning_{false};
};
