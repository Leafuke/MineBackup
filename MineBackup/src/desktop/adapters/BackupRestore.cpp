#include "RuntimeRetentionService.h"
#include "BackupManager.h"
#include "BackupManagerInternal.h"

#include "ArchiveRunner.h"
#include "AppPaths.h"
#include "Broadcast.h"
#include "CloudSyncService.h"
#include "ConfigManager.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "GameSessionManager.h"
#include "Globals.h"
#include "HistoryManager.h"
#include "HotRestoreCoordinator.h"
#include "Logging.h"
#include "MigrationCoordinator.h"
#include "DesktopPlatform.h"
#include "RestoreService.h"
#include "RestoreWorkspace.h"
#include "WorldIdentity.h"
#include "PathIdentity.h"
#include "text_to_text.h"
#include "i18n.h"
#include "TaskCoordinator.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

using namespace std;
using namespace BackupManagerInternal;

#define RESTORE_INFO(...) MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Restore, "restore.progress", __VA_ARGS__)
#define RESTORE_WARNING(...) MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Restore, "restore.warning", __VA_ARGS__)
#define RESTORE_ERROR(...) MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Restore, "restore.error", __VA_ARGS__)

namespace {

constexpr const wchar_t* kDeletedOnlyMarkerDirectory =
	FolderRewindFormat::kInternalRestoreMarkerDirectoryName;

void CleanupInternalRestoreMarkers(const filesystem::path& targetDirectory) {
	for (const wchar_t* marker : {kDeletedOnlyMarkerDirectory, L"__MineBackup_Internal"}) {
		error_code ec;
		const filesystem::path internalDirectory = targetDirectory / marker;
		if (!filesystem::exists(internalDirectory, ec) || ec) continue;
		ClearReadonlyAttributesRecursively(internalDirectory);
		filesystem::remove_all(internalDirectory, ec);
	}
}

static bool ValidateRestoreArchives(const vector<filesystem::path>& archives, const Config& config) {
	RESTORE_INFO(L("LOG_VERIFYING_BACKUPS"));
	const auto runner=ArchiveRunner::Resolve(config.zipPath,GetAppPaths(),TaskCoordinator::CurrentStopToken());
	for (const auto& backup : archives) {
  string memberError;
  if (!runner.ValidateMembers(backup,memberError,config.useLowPriority)) {
   RESTORE_ERROR(L("RESTORE_ARCHIVE_PATHS_UNSUPPORTED"),memberError.c_str()); return false;
  }

		if (!RunInternalProcess(MakeInternalProcess(config.zipPath,
			{L"t", backup.wstring(), L"-y"}, {}, config.useLowPriority))) {
			RESTORE_ERROR(L("ERROR_BACKUP_CORRUPTED"), wstring_to_utf8(backup.filename().wstring()).c_str());
			return false;
		}
	}
	RESTORE_INFO(L("LOG_BACKUP_VERIFICATION_PASSED"));
	return true;
}

static bool ApplyRestoreChain(const vector<filesystem::path>& backupsToApply, const filesystem::path& destinationFolder,
	const Config& config, const vector<wstring>& filesToExtract = {}) {
	for (size_t i = 0; i < backupsToApply.size(); ++i) {
		const auto& backup = backupsToApply[i];
		RESTORE_INFO(L("RESTORE_STEPS"), i + 1, backupsToApply.size(), wstring_to_utf8(backup.filename().wstring()).c_str());
		vector<wstring> arguments = {L"x", backup.wstring(), L"-o" + destinationFolder.wstring(), L"-y"};
		arguments.insert(arguments.end(), filesToExtract.begin(), filesToExtract.end());
		if (!RunInternalProcess(MakeInternalProcess(config.zipPath, std::move(arguments), {}, config.useLowPriority))) {
			return false;
		}
	}
	return true;
}

RestoreServiceDependencies DesktopVerificationDependencies() {
    RestoreServiceDependencies dependencies; dependencies.paths = GetAppPaths();
    dependencies.repairArchiveChain = [](const RestoreRequest& request, stop_token token) {
        if (token.stop_requested()) return;
        const auto configs = SnapshotConfigState().configs;
        auto found = find_if(configs.begin(), configs.end(), [&](const auto& pair) { return pair.second.configId == request.config.configId; });
        const int index = found == configs.end() ? -1 : found->first;
        const auto archive = request.archive.filename().wstring();
        const auto entries = GetHistoryRepository().EntriesForConfig(request.config.configId);
        const auto entry = find_if(entries->begin(), entries->end(), [&](const HistoryEntry& item) {
            return WorldIdentity::Matches(request.config, request.world.relativePath, item, archive);
        });
        if (request.config.cloudAutoDownloadBeforeRestore && index >= 0 && entry != entries->end())
            EnsureRestoreChainAvailable(request.config, index, *entry);
        if (!token.stop_requested()) MigrationCoordinator::EnsureWorldMigrated(request.config, index,
            request.world.relativePath, (filesystem::path(request.config.saveRoot) / request.world.relativePath).wstring());
    };
    return dependencies;
}

bool RunSharedManagedRestore(
	const Config& config,
	const wstring& worldName,
	const wstring& backupFile,
	RestoreMode mode,
	const vector<wstring>* restoreWhitelistOverride,
	const string& requestId,
	const RestoreSafetyBackup* safetyBackup,
    const RestorePlan* preparedPlan) {
	const string operationId = requestId.empty()
		? wstring_to_utf8(FolderRewindFormat::GenerateGuidString()) : requestId;
	minebackup::logging::ScopedLogContext operationContext{{
		{"operation_id", operationId},
		{"config_id", wstring_to_utf8(config.configId)},
		{"world", wstring_to_utf8(worldName)}}};
	auto fail = [&](string reason) {
		BroadcastEvent("event=restore_failed;config_id=" + wstring_to_utf8(config.configId)
			+ ";world=" + wstring_to_utf8(worldName) + ";error=" + reason
			+ (requestId.empty() ? "" : ";request_id=" + requestId));
		return false;
	};
	if (g_appState.profileRecoveryRequired.load()) return fail("profile_recovery_required");
	if (config.pendingLocalBinding) {
		RESTORE_WARNING("Restore is disabled until local paths are bound.");
		return fail("binding_required");
	}

	const filesystem::path destination = JoinPath(config.saveRoot, worldName);
	if (IsWorldOccupied(destination)) {
		RESTORE_WARNING(L("LOG_RESTORE_ACTIVE_WORLD_BLOCKED"),
			wstring_to_utf8(worldName).c_str());
		return fail("world_occupied");
	}
	const int configIndex = ResolveConfigIndexForCloud(config);

	RestoreRequest request;
	request.config = config;
	request.world = {config.configId, worldName};
	request.archive = backupFile;
	request.mode = mode;
	request.restorePreserve = restoreWhitelistOverride
		? *restoreWhitelistOverride : SettingsState().restoreWhitelist;
	RestoreServiceDependencies dependencies = DesktopVerificationDependencies();
    if (preparedPlan) dependencies.repairArchiveChain = {};
	dependencies.isWorldOccupied = IsWorldOccupied;
	dependencies.backupBeforeRestore = [config, worldName, configIndex](
		const BackupRequest&, stop_token stopToken, BackupExecutionOptions options) {
		wstring description;
		const auto found = find_if(config.worlds.begin(), config.worlds.end(),
			[&](const auto& world) { return world.first == worldName; });
		if (found != config.worlds.end()) description = found->second;
		MyFolder world{JoinPath(config.saveRoot, worldName).wstring(), worldName,
			description, config, configIndex, -1};
		return RunDesktopBackup(
			world, L"Automatic backup before restore", stopToken, options);
	};
	dependencies.enforceRetention = [](
        const BackupRequest& request, const HistoryEntry& entry, stop_token token) {
        RuntimeRetentionService retention(GetHistoryRepository(), GetAppPaths().HistoryFile(),
            SnapshotConfigState().configs, GetAppPaths());
        retention.Enforce(request, entry, token);
    };
	RESTORE_INFO(L("LOG_RESTORE_START_HEADER"));
	RESTORE_INFO(L("LOG_RESTORE_PREPARE"), wstring_to_utf8(worldName).c_str());
	RESTORE_INFO(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(backupFile).c_str());
	RestoreService service(std::move(dependencies));
	const auto restored = service.Run(
		request, false, TaskCoordinator::CurrentStopToken(),
		RestoreExecutionOptions{safetyBackup ? optional<RestoreSafetyBackup>(*safetyBackup) : nullopt});
	for (const auto& diagnostic : restored.diagnostics) {
		if (diagnostic.severity == DiagnosticSeverity::Error) {
			RESTORE_ERROR("%s: %s", diagnostic.eventId.c_str(), diagnostic.detail.c_str());
		}
		else if (diagnostic.severity == DiagnosticSeverity::Warning) {
			RESTORE_WARNING("%s: %s", diagnostic.eventId.c_str(), diagnostic.detail.c_str());
		}
		else {
			RESTORE_INFO("%s: %s", diagnostic.eventId.c_str(), diagnostic.detail.c_str());
		}
	}
	if (!IsSuccessful(restored.code)) return fail(ToString(restored.code));

	RESTORE_INFO(L("LOG_RESTORE_END_HEADER"));
	BroadcastEvent("event=restore_success;config_id=" + wstring_to_utf8(config.configId)
		+ ";world=" + wstring_to_utf8(worldName) + ";backup="
		+ wstring_to_utf8(backupFile)
		+ (requestId.empty() ? "" : ";request_id=" + requestId));
	return true;
}

} // namespace

RestorePlan PreflightDesktopRestore(const Config& config, const wstring& worldName,
    const wstring& backupFile, int restoreMethod, stop_token token) {
    RestoreRequest request; request.config = config; request.world = {config.configId, worldName}; request.archive = backupFile;
    request.mode = restoreMethod == 0 ? RestoreMode::Clean : RestoreMode::Overwrite;
    const auto mode = restoreMethod == 2 ? RestoreVerificationMode::Reverse
        : restoreMethod == 3 ? RestoreVerificationMode::LegacyForward : RestoreVerificationMode::Managed;
    return RestoreService(DesktopVerificationDependencies()).Verify(request, token, mode);
}

namespace {
bool PrepareRestoreSafety(const Config& config,const wstring& worldName,optional<RestoreSafetyBackup>& safety) {
 if(safety) return IsSuccessful(safety->result.code);
 if(!config.backupBefore) return true;
 MyFolder world{JoinPath(config.saveRoot,worldName).wstring(),worldName,L"",config,ResolveConfigIndexForCloud(config),-1};
 RestoreSafetyBackup prepared; prepared.request.config=config; prepared.request.world={config.configId,worldName}; prepared.request.sourcePath=world.path;
 prepared.result=RunDesktopBackup(world,L"BeforeRestore",TaskCoordinator::CurrentStopToken(),BackupExecutionOptions{.deferRetention=true});
 if(!IsSuccessful(prepared.result.code)) return false; safety=std::move(prepared); return true;
}
void FinishRestoreSafety(const optional<RestoreSafetyBackup>& safety) {
 if(!safety || !safety->result.historyEntry || TaskCoordinator::CurrentStopToken().stop_requested()) return;
 map<int,Config> configs; {auto configAccess = g_appState.configuration.Write();configs=configAccess.ReadConfigs();}
 RuntimeRetentionService retention(GetHistoryRepository(),GetAppPaths().HistoryFile(),configs,GetAppPaths());
 retention.Enforce(safety->request,*safety->result.historyEntry,TaskCoordinator::CurrentStopToken());
}
}

bool DoRestore2(const Config& config, const wstring& worldName, const filesystem::path& fullBackupPath, int restoreMethod) {
	minebackup::logging::ScopedLogContext operationContext{{
		"operation_id", wstring_to_utf8(FolderRewindFormat::GenerateGuidString())},
		{"config_id", wstring_to_utf8(config.configId)},
		{"world", wstring_to_utf8(worldName)}};
	if (config.pendingLocalBinding) {
		RESTORE_WARNING("Restore is disabled until local paths are bound.");
		return false;
	}
	WorldIdentity::Value identity;
	if (!WorldIdentity::TryBuild(config, worldName, identity)
		|| identity.relativeWorldPath != worldName
		|| !WorldIdentity::FindStorageConflicts({{0, config}}).empty()) {
		RESTORE_ERROR(L("RESTORE_WORLD_IDENTITY_INVALID"));
		return false;
	}
	filesystem::path destinationFolder = identity.sourcePath;
	if (IsWorldOccupied(destinationFolder)) {
		RESTORE_WARNING(L("LOG_RESTORE_ACTIVE_WORLD_BLOCKED"),
			wstring_to_utf8(worldName).c_str());
		BroadcastEvent("event=restore_failed;config_id=" + wstring_to_utf8(config.configId)
			+ ";world=" + wstring_to_utf8(worldName) + ";error=world_occupied");
		return false;
	}
	WorldOperationGuard opGuard(destinationFolder, FolderState::RESTORE);
	if (!opGuard.Acquired()) {
		RESTORE_WARNING(
			L("LOG_OP_REJECTED_BUSY"),
			wstring_to_utf8(worldName).c_str(),
			L(FolderStateToI18nKey(opGuard.Existing())),
			L(FolderStateToI18nKey(opGuard.Requested()))
		);
		return false;
	}

	auto failRestore = [&](const string& reason) {
		BroadcastEvent("event=restore_failed;config_id=" + wstring_to_utf8(config.configId) + ";world=" + wstring_to_utf8(worldName) + ";error=" + reason);
		return false;
	};

	RESTORE_INFO(L("LOG_RESTORE_START_HEADER"));
	RESTORE_INFO(L("LOG_RESTORE_PREPARE"), wstring_to_utf8(worldName).c_str());
	RESTORE_INFO(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(fullBackupPath.wstring()).c_str());

	const ArchiveRunner archiveRunner = ArchiveRunner::Resolve(
		config.zipPath,
		GetAppPaths(),
		TaskCoordinator::CurrentStopToken());
	if (!archiveRunner.IsAvailable()) {
		RESTORE_ERROR(
			L("LOG_ERROR_7Z_NOT_FOUND"),
			wstring_to_utf8(archiveRunner.Resolution().diagnostic).c_str());
		RESTORE_ERROR(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return failRestore("seven_zip_not_found");
	}

	vector<filesystem::path> backupsToApply = { fullBackupPath };
	if (!ValidateRestoreArchives(backupsToApply, config)) {
		return failRestore("archive_integrity_check_failed");
	}

	optional<RestoreSafetyBackup> safety;
	opGuard.Reset();
	if(!PrepareRestoreSafety(config,worldName,safety)) return failRestore("safety_backup_failed");
	if(IsWorldOccupied(destinationFolder)) return failRestore("world_occupied");
	opGuard=WorldOperationGuard(destinationFolder,FolderState::RESTORE);
	if(!opGuard.Acquired()) return failRestore("world_busy");
	RestoreWorkspace::State restoreWorkspace;
	string workspaceError;
	const auto workspaceMode = restoreMethod == 0
		? RestoreWorkspace::Mode::Clean : RestoreWorkspace::Mode::Overlay;
	if (!RestoreWorkspace::Prepare(
			destinationFolder, restoreWorkspace, workspaceError, workspaceMode)) {
		if (restoreWorkspace.CanRollback()) {
			string rollbackError;
			if (!RestoreWorkspace::Rollback(restoreWorkspace, rollbackError)) {
				RESTORE_ERROR("Failed to rollback after workspace prepare failure: %s", rollbackError.c_str());
			}
		}
		RESTORE_ERROR("Failed to prepare safe restore workspace: %s", workspaceError.c_str());
		return failRestore("snapshot_prepare_failed");
	}

	bool restoreSucceeded = ApplyRestoreChain(backupsToApply, destinationFolder, config);
	if (TaskCoordinator::CurrentStopToken().stop_requested()) restoreSucceeded = false;
	if (restoreSucceeded) {
		CleanupInternalRestoreMarkers(destinationFolder);
		const vector<wstring> effectiveRestoreWhitelist = restoreMethod == 0
			? BuildEffectiveRestoreWhitelist(SettingsState().restoreWhitelist) : vector<wstring>{};
		const auto commit = RestoreWorkspace::Commit(
			restoreWorkspace, effectiveRestoreWhitelist, workspaceError,
			{.stopToken = TaskCoordinator::CurrentStopToken()});
		if (commit.status == RestoreWorkspace::CommitStatus::CleanupWarning)
			RESTORE_WARNING("Restore committed; retained snapshot: %s (%s)",
				commit.retainedSnapshot.string().c_str(), commit.error.c_str());
		if (!commit.WasCommitted()) {
			restoreSucceeded = false;
			RESTORE_ERROR("Failed to commit safe restore workspace: %s", workspaceError.c_str());
		}
	}

	if (!restoreSucceeded) {
		if (restoreWorkspace.CanRollback()) {
			if (!RestoreWorkspace::Rollback(restoreWorkspace, workspaceError)) {
				RESTORE_ERROR("Failed to rollback safe restore workspace: %s", workspaceError.c_str());
			}
		}
		return failRestore("command_failed");
	}

	FinishRestoreSafety(safety);
	RESTORE_INFO(L("LOG_RESTORE_END_HEADER"));
	BroadcastEvent("event=restore_success;config_id=" + wstring_to_utf8(config.configId) + ";world=" + wstring_to_utf8(worldName) + ";backup=" + wstring_to_utf8(fullBackupPath.filename().wstring()));
	return true;
}

bool DoRestore(
	const Config& config,
	const wstring& worldName,
	const wstring& backupFile,
	int restoreMethod,
	const string& customRestoreList,
	const vector<wstring>* restoreWhitelistOverride,
	const string& requestId,
	const RestoreSafetyBackup* safetyBackup,
    const RestorePlan* preparedPlan) {
	if (restoreMethod == 0 || restoreMethod == 1) {
		return RunSharedManagedRestore(config, worldName, backupFile,
			restoreMethod == 0 ? RestoreMode::Clean : RestoreMode::Overwrite,
			restoreWhitelistOverride, requestId, safetyBackup, preparedPlan);
	}
	const string operationId = requestId.empty()
		? wstring_to_utf8(FolderRewindFormat::GenerateGuidString()) : requestId;
	minebackup::logging::ScopedLogContext operationContext{{
		"operation_id", operationId},
		{"config_id", wstring_to_utf8(config.configId)},
		{"world", wstring_to_utf8(worldName)}};
	if (config.pendingLocalBinding) {
		RESTORE_WARNING("Restore is disabled until local paths are bound.");
		return false;
	}
	WorldIdentity::Value identity;
	if (!WorldIdentity::TryBuild(config, worldName, identity)
		|| identity.relativeWorldPath != worldName
		|| !WorldIdentity::FindStorageConflicts({{0, config}}).empty()) {
		RESTORE_ERROR(L("RESTORE_WORLD_IDENTITY_INVALID"));
		return false;
	}
	filesystem::path destinationFolder = identity.sourcePath;
	if (IsWorldOccupied(destinationFolder)) {
		RESTORE_WARNING(L("LOG_RESTORE_ACTIVE_WORLD_BLOCKED"),
			wstring_to_utf8(worldName).c_str());
		BroadcastEvent("event=restore_failed;config_id=" + wstring_to_utf8(config.configId)
			+ ";world=" + wstring_to_utf8(worldName) + ";error=world_occupied");
		return false;
	}
	WorldOperationGuard opGuard(destinationFolder, FolderState::RESTORE);
	if (!opGuard.Acquired()) {
		RESTORE_WARNING(
			L("LOG_OP_REJECTED_BUSY"),
			wstring_to_utf8(worldName).c_str(),
			L(FolderStateToI18nKey(opGuard.Existing())),
			L(FolderStateToI18nKey(opGuard.Requested()))
		);
		return false;
	}

	auto failRestoreWithMessage = [&](const string& reason, const string& message) {
		if (!message.empty()) {
			RESTORE_ERROR("%s", message.c_str());
		}
		BroadcastEvent("event=restore_failed;config_id=" + wstring_to_utf8(config.configId)
			+ ";world=" + wstring_to_utf8(worldName) + ";error=" + reason
			+ (requestId.empty() ? "" : ";request_id=" + requestId));
		return false;
	};
	auto failRestore = [&](const string& reason) {
		return failRestoreWithMessage(reason, string{});
	};

	RESTORE_INFO(L("LOG_RESTORE_START_HEADER"));
	RESTORE_INFO(L("LOG_RESTORE_PREPARE"), wstring_to_utf8(worldName).c_str());
	RESTORE_INFO(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(backupFile).c_str());

	const ArchiveRunner archiveRunner = ArchiveRunner::Resolve(
		config.zipPath,
		GetAppPaths(),
		TaskCoordinator::CurrentStopToken());
	if (!archiveRunner.IsAvailable()) {
		RESTORE_ERROR(
			L("LOG_ERROR_7Z_NOT_FOUND"),
			wstring_to_utf8(archiveRunner.Resolution().diagnostic).c_str());
		RESTORE_ERROR(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return failRestore("seven_zip_not_found");
	}

    const auto plan = preparedPlan ? *preparedPlan
        : PreflightDesktopRestore(config, worldName, backupFile, restoreMethod, TaskCoordinator::CurrentStopToken());
    if (!IsSuccessful(plan.code)) {
        for (const auto& diagnostic : plan.diagnostics) RESTORE_ERROR("%s: %s", diagnostic.eventId.c_str(), diagnostic.detail.c_str());
        return failRestore("archive_verification_failed");
    }
    FolderRewindFormat::StoragePaths storage;
    if (!FolderRewindFormat::TryResolveStoragePaths(config.backupPath, worldName, destinationFolder.wstring(), storage)
        || !PathIdentity::PathsEqual(plan.targetWorld, destinationFolder)
        || !PathIdentity::PathsEqual(plan.selectedArchive, storage.backupSubDir / backupFile)
        || plan.archiveChain.empty()
        || any_of(plan.archiveChain.begin(), plan.archiveChain.end(), [&](const auto& archive) {
            return !PathIdentity::PathsEqual(archive.parent_path(), storage.backupSubDir);
        })) return failRestore("prepared_plan_invalid");
    const auto& backupsToApply = plan.archiveChain;
    if (!ValidateRestoreArchives(backupsToApply, config)) return failRestore("archive_integrity_check_failed");

	vector<wstring> filesToExtract;
	if (restoreMethod == 3 && !customRestoreList.empty()) {
		RESTORE_INFO(L("LOG_CUSTOM_RESTORE_START"));
		stringstream ss(customRestoreList);
		string item;
		while (getline(ss, item, ',')) {
			item.erase(0, item.find_first_not_of(" \t\n\r"));
			item.erase(item.find_last_not_of(" \t\n\r") + 1);
			if (!item.empty()) {
				filesToExtract.push_back(utf8_to_wstring(item));
			}
		}
	}


	optional<RestoreSafetyBackup> safety;
	if(safetyBackup) safety=*safetyBackup;
	opGuard.Reset();
	if(!PrepareRestoreSafety(config,worldName,safety)) return failRestore("safety_backup_failed");
	if(IsWorldOccupied(destinationFolder)) return failRestore("world_occupied");
	opGuard=WorldOperationGuard(destinationFolder,FolderState::RESTORE);
	if(!opGuard.Acquired()) return failRestore("world_busy");
	RestoreWorkspace::State restoreWorkspace;
	string workspaceError;
	const auto workspaceMode = restoreMethod == 0
		? RestoreWorkspace::Mode::Clean : RestoreWorkspace::Mode::Overlay;
	if (!RestoreWorkspace::Prepare(
			destinationFolder, restoreWorkspace, workspaceError, workspaceMode)) {
		if (restoreWorkspace.CanRollback()) {
			string rollbackError;
			if (!RestoreWorkspace::Rollback(restoreWorkspace, rollbackError)) {
				RESTORE_ERROR("Failed to rollback after workspace prepare failure: %s", rollbackError.c_str());
			}
		}
		RESTORE_ERROR("Failed to prepare safe restore workspace: %s", workspaceError.c_str());
		return failRestore("snapshot_prepare_failed");
	}

	bool restoreSucceeded = ApplyRestoreChain(backupsToApply, destinationFolder, config, filesToExtract);

	if (TaskCoordinator::CurrentStopToken().stop_requested()) restoreSucceeded = false;
	if (restoreSucceeded) {
		CleanupInternalRestoreMarkers(destinationFolder);
		const vector<wstring> effectiveRestoreWhitelist = restoreMethod == 0
			? BuildEffectiveRestoreWhitelist(
				restoreWhitelistOverride ? *restoreWhitelistOverride : SettingsState().restoreWhitelist)
			: vector<wstring>{};
		const auto commit = RestoreWorkspace::Commit(
			restoreWorkspace, effectiveRestoreWhitelist, workspaceError,
			{.stopToken = TaskCoordinator::CurrentStopToken()});
		if (commit.status == RestoreWorkspace::CommitStatus::CleanupWarning)
			RESTORE_WARNING("Restore committed; retained snapshot: %s (%s)",
				commit.retainedSnapshot.string().c_str(), commit.error.c_str());
		if (!commit.WasCommitted()) {
			restoreSucceeded = false;
			RESTORE_ERROR("Failed to commit safe restore workspace: %s", workspaceError.c_str());
		}
	}

	if (!restoreSucceeded) {
		if (restoreWorkspace.CanRollback()) {
			if (!RestoreWorkspace::Rollback(restoreWorkspace, workspaceError)) {
				RESTORE_ERROR("Failed to rollback safe restore workspace: %s", workspaceError.c_str());
			}
		}
		return failRestore("command_failed");
	}

	FinishRestoreSafety(safety);
	RESTORE_INFO(L("LOG_RESTORE_END_HEADER"));
	BroadcastEvent("event=restore_success;config_id=" + wstring_to_utf8(config.configId)
		+ ";world=" + wstring_to_utf8(worldName) + ";backup=" + wstring_to_utf8(backupFile)
		+ (requestId.empty() ? "" : ";request_id=" + requestId));
	return true;
}

bool DoHotRestore(
	const MyFolder& world,
	bool deleteBackup,
	const wstring& backupFile,
	int restoreMethod,
	const vector<wstring>* restoreWhitelistOverride,
	const string& customRestoreList,
	const string& requestId,
	const RestoreSafetyBackup* safetyBackup,
    const RestorePlan* preparedPlan) {
	(void)deleteBackup;
	auto& mod = g_appState.knotLinkMod;
	const string operationId = requestId.empty()
		? wstring_to_utf8(FolderRewindFormat::GenerateGuidString()) : requestId;
	minebackup::logging::ScopedLogContext operationContext{{
		"operation_id", operationId},
		{"config_id", wstring_to_utf8(world.config.configId)},
		{"world", wstring_to_utf8(world.name)}};
	RESTORE_INFO(L("KNOTLINK_HOT_RESTORE_START"), wstring_to_utf8(world.name).c_str());
	HotRestoreDependencies dependencies;
	dependencies.transport.reset = [&] { mod.resetForOperation(); };
	dependencies.transport.emit = [](string_view eventName,
		const vector<pair<string, string>>& fields) {
		BroadcastEvent(eventName, fields);
		return true;
	};
	dependencies.transport.waitHandshake = [&mod](chrono::milliseconds, stop_token) {
		return mod.versionCompatible.load()
			? HotRestoreHandshakeStatus::Compatible
			: HotRestoreHandshakeStatus::Incompatible;
	};
	dependencies.transport.waitSaveAndExit = [&mod](
		chrono::milliseconds timeout, stop_token) {
		return mod.waitForFlag(&KnotLinkModInfo::worldSaveAndExitComplete, timeout);
	};
	dependencies.transport.waitRejoin = [&mod](
		chrono::milliseconds timeout, stop_token) -> optional<bool> {
		if (!mod.waitForFlag(&KnotLinkModInfo::rejoinResponseReceived, timeout)) {
			return nullopt;
		}
		lock_guard<mutex> lock(mod.mtx);
		return mod.rejoinSuccess;
	};
	dependencies.isWorldOccupied = [](const filesystem::path& path) {
		return IsWorldOccupied(path.wstring());
	};
	dependencies.executeRestore = [&, pinnedBackup = backupFile](stop_token) {
		g_appState.hotkeyRestoreState = HotRestoreState::RESTORING;
		RESTORE_INFO(L("KNOTLINK_HOT_RESTORE_PROCEEDING"));
		RestoreResult result;
		wstring selected = pinnedBackup;
		if (selected.empty()) {
			FolderRewindFormat::StoragePaths storage;
			if (FolderRewindFormat::TryResolveStoragePaths(
					world.config.backupPath, world.name, world.path, storage)) {
				const auto history = GetHistoryEntriesForWorld(
					world.configIndex, world.name);
				for (auto current = history.rbegin(); current != history.rend(); ++current) {
					error_code error;
					if (filesystem::is_regular_file(
							storage.backupSubDir / current->backupFile, error) && !error) {
						selected = current->backupFile;
						break;
					}
				}
			}
		}
		if (selected.empty()) {
			result.code = OperationCode::TargetNotFound;
			return result;
		}
		result.code = DoRestore(
			world.config, world.name, selected, restoreMethod,
			customRestoreList, restoreWhitelistOverride, requestId, safetyBackup, preparedPlan)
			? OperationCode::Success : OperationCode::RestoreFailed;
		return result;
	};
	HotRestoreRequest request;
	request.configId = world.config.configId;
	request.worldPath = world.name;
	request.fullWorldPath = world.path;
	request.requestId = requestId;
	request.handshakeComplete = true;
	const auto result = HotRestoreCoordinator(std::move(dependencies)).Run(request);
	g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
	g_appState.isRespond = false;
	return IsSuccessful(result.code);
}
