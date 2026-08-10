#pragma once

#include "AppPaths.h"
#include "ArchiveRunner.h"
#include "OperationResult.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

class ICloudPostHook;
class IHotBackupBridge;
class IRuntimeEventSink;

struct BackupRequest {
	Config config;
	WorldRef world;
	std::filesystem::path sourcePath;
	std::wstring displayName;
	std::wstring comment;
	int legacyConfigIndex = -1;
};

enum class HotBackupStatus {
	NotNeeded,
	Coordinated,
	Degraded,
	Rejected
};

struct HotBackupPreparation {
	HotBackupStatus status = HotBackupStatus::NotNeeded;
	std::vector<Diagnostic> diagnostics;
};

struct BackupRuntimeEvent {
	std::string eventId;
	std::vector<std::pair<std::string, std::string>> fields;
};

struct BackupServiceDependencies {
	AppPaths paths;
	std::function<MigrationUnitResult(const BackupRequest&)> ensureMigration;
	std::function<bool(const std::filesystem::path&)> isFileLocked;
	std::function<bool(const HistoryEntry&)> addHistory;
	std::function<bool(const std::wstring&, const std::wstring&)> removeHistory;
	std::function<void(const BackupRequest&, const HistoryEntry&)> enforceRetention;
	std::shared_ptr<ICloudPostHook> cloudPost;
	std::shared_ptr<IHotBackupBridge> hotBackup;
	std::shared_ptr<IRuntimeEventSink> eventSink;
	std::function<ArchiveRunner(
		const std::filesystem::path&,
		const AppPaths&,
		std::stop_token)> archiveRunnerFactory;
	ArchiveRunner::ProcessExecutor processExecutor;
};

class BackupService {
public:
	explicit BackupService(BackupServiceDependencies dependencies);

	BackupResult Run(const BackupRequest& request, std::stop_token stopToken = {}) const;

private:
	BackupResult RunCore(
		const BackupRequest& request,
		std::stop_token stopToken) const;
	BackupServiceDependencies dependencies_;
};
