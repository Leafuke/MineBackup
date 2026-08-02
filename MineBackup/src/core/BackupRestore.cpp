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
#include "Logging.h"
#include "MigrationCoordinator.h"
#include "PathRuleSet.h"
#include "PlatformCompat.h"
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

enum class RestoreChainStatus {
	OK,
	METADATA_UNAVAILABLE,
	MISSING_BASE_FULL,
	INVALID
};

struct RestoreChainResult {
	RestoreChainStatus status = RestoreChainStatus::INVALID;
	vector<filesystem::path> chain;
	bool usedMetadata = false;
};

struct SmartRestoreArchiveGroup {
	filesystem::path archive;
	vector<wstring> files;
};

struct SmartRestorePlan {
	vector<filesystem::path> chain;
	vector<SmartRestoreArchiveGroup> archiveGroups;
};

filesystem::path CreateSafeRestoreTempDirectoryPath(const filesystem::path& targetDirectory) {
	const filesystem::path normalized = targetDirectory.lexically_normal();
	const filesystem::path parent = normalized.parent_path();
	if (parent.empty()) throw runtime_error("Restore target has no parent directory.");

	const filesystem::path base = parent / (normalized.filename().wstring() + L"-Temp");
	filesystem::path candidate = base;
	int suffix = 1;
	error_code ec;
	while (filesystem::exists(candidate, ec)) {
		candidate = filesystem::path(base.wstring() + L"-" + to_wstring(suffix++));
	}
	return candidate;
}

bool TryPrepareSafeRestoreWorkspace(
	const filesystem::path& targetDirectory,
	filesystem::path& snapshotDirectory,
	string& errorMessage) {
	snapshotDirectory.clear();
	errorMessage.clear();
	error_code ec;
	const bool targetExists = filesystem::exists(targetDirectory, ec);
	if (ec) {
		errorMessage = "Failed to inspect restore target: " + ec.message();
		return false;
	}
	if (!targetExists) {
		filesystem::create_directories(targetDirectory, ec);
		if (ec) errorMessage = "Failed to create restore target: " + ec.message();
		return !ec;
	}

	try {
		snapshotDirectory = CreateSafeRestoreTempDirectoryPath(targetDirectory);
	}
	catch (const exception& exception) {
		errorMessage = exception.what();
		return false;
	}

	filesystem::rename(targetDirectory, snapshotDirectory, ec);
	if (ec) {
		errorMessage = "Failed to move restore target to snapshot: " + ec.message();
		snapshotDirectory.clear();
		return false;
	}
	filesystem::create_directories(targetDirectory, ec);
	if (!ec) return true;

	const string createError = ec.message();
	error_code cleanupError;
	if (filesystem::exists(targetDirectory, cleanupError) && !cleanupError) {
		ClearReadonlyAttributesRecursively(targetDirectory);
		filesystem::remove_all(targetDirectory, cleanupError);
	}

	error_code rollbackError;
	filesystem::rename(snapshotDirectory, targetDirectory, rollbackError);
	if (rollbackError) {
		errorMessage = "Failed to create clean workspace (" + createError
			+ "), rollback also failed: " + rollbackError.message();
		return false;
	}
	snapshotDirectory.clear();
	errorMessage = "Failed to create clean workspace: " + createError;
	return false;
}

void CleanupInternalRestoreMarkers(const filesystem::path& targetDirectory) {
	for (const wchar_t* marker : {kDeletedOnlyMarkerDirectory, L"__MineBackup_Internal"}) {
		error_code ec;
		const filesystem::path internalDirectory = targetDirectory / marker;
		if (!filesystem::exists(internalDirectory, ec) || ec) continue;
		ClearReadonlyAttributesRecursively(internalDirectory);
		filesystem::remove_all(internalDirectory, ec);
	}
}

void CopyRestoreWhitelistEntries(
	const filesystem::path& sourceDirectory,
	const filesystem::path& targetDirectory,
	const vector<wstring>& whitelist) {
	if (whitelist.empty()) return;
	error_code ec;
	if (!filesystem::exists(sourceDirectory, ec) || ec) return;
	const PathRuleSet rules(whitelist);

	for (const auto& entry : filesystem::recursive_directory_iterator(
		sourceDirectory,
		filesystem::directory_options::skip_permission_denied,
		ec)) {
		if (ec) break;
		if (!entry.is_directory()
			|| !rules.MatchesSelfOrAncestor(entry.path(), sourceDirectory)) {
			continue;
		}
		const filesystem::path relative = filesystem::relative(entry.path(), sourceDirectory, ec);
		if (!ec) filesystem::create_directories(targetDirectory / relative, ec);
	}

	for (const auto& entry : filesystem::recursive_directory_iterator(
		sourceDirectory,
		filesystem::directory_options::skip_permission_denied,
		ec)) {
		if (ec) break;
		if (!entry.is_regular_file()
			|| !rules.MatchesSelfOrAncestor(entry.path(), sourceDirectory)) {
			continue;
		}
		const filesystem::path relative = filesystem::relative(entry.path(), sourceDirectory, ec);
		if (ec) continue;
		const filesystem::path destination = targetDirectory / relative;
		filesystem::create_directories(destination.parent_path(), ec);
		if (!filesystem::exists(destination, ec) || ec) {
			ec.clear();
			filesystem::copy_file(
				entry.path(),
				destination,
				filesystem::copy_options::overwrite_existing,
				ec);
		}
	}
}

bool TryCommitSafeRestoreWorkspace(
	const filesystem::path& targetDirectory,
	const filesystem::path& snapshotDirectory,
	const vector<wstring>& whitelist,
	string& errorMessage) {
	errorMessage.clear();
	try {
		CleanupInternalRestoreMarkers(targetDirectory);
		CopyRestoreWhitelistEntries(snapshotDirectory, targetDirectory, whitelist);
		if (!snapshotDirectory.empty() && filesystem::exists(snapshotDirectory)) {
			ClearReadonlyAttributesRecursively(snapshotDirectory);
			filesystem::remove_all(snapshotDirectory);
		}
		return true;
	}
	catch (const exception& exception) {
		errorMessage = exception.what();
		return false;
	}
}

bool TryRollbackSafeRestoreWorkspace(
	const filesystem::path& targetDirectory,
	const filesystem::path& snapshotDirectory,
	string& errorMessage) {
	errorMessage.clear();
	if (snapshotDirectory.empty()) {
		errorMessage = "Snapshot directory path is empty.";
		return false;
	}

	error_code ec;
	if (!filesystem::exists(snapshotDirectory, ec) || ec) {
		errorMessage = "Snapshot directory is missing.";
		return false;
	}
	if (filesystem::exists(targetDirectory, ec) && !ec) {
		ClearReadonlyAttributesRecursively(targetDirectory);
		filesystem::remove_all(targetDirectory, ec);
		if (ec) {
			errorMessage = "Failed to clean restore target before rollback: " + ec.message();
			return false;
		}
	}
	filesystem::rename(snapshotDirectory, targetDirectory, ec);
	if (ec) {
		errorMessage = "Failed to restore snapshot: " + ec.message();
		return false;
	}
	return true;
}

static bool ValidateRestoreArchives(const vector<filesystem::path>& archives, const Config& config) {
	RESTORE_INFO(L("LOG_VERIFYING_BACKUPS"));
	for (const auto& backup : archives) {
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

static RestoreChainResult BuildMetadataRestoreChain(const filesystem::path& metadataDir, const filesystem::path& backupDir, const filesystem::path& targetBackupPath) {
	RestoreChainResult result;
	set<wstring> visited;
	vector<FolderRewindFormat::ChangeRecord> recordChain;
	wstring current = targetBackupPath.filename().wstring();

	while (!current.empty()) {
		if (!visited.insert(current).second) {
			result.status = RestoreChainStatus::INVALID;
			result.chain.clear();
			return result;
		}

		FolderRewindFormat::ChangeRecord record;
		if (!FolderRewindMetadataStore::LoadRecord(metadataDir, current, record)) {
			result.status = RestoreChainStatus::METADATA_UNAVAILABLE;
			result.chain.clear();
			return result;
		}

		filesystem::path currentArchive = backupDir / current;
		if (!filesystem::exists(currentArchive)) {
			result.status = RestoreChainStatus::INVALID;
			result.chain.clear();
			return result;
		}

		result.chain.push_back(currentArchive);
		recordChain.push_back(record);
		const wstring recordType = record.backupType.empty() ? current : record.backupType;
		if (!FolderRewindFormat::IsSmartBackupType(recordType)) {
			break;
		}

		if (record.previousBackupFileName.empty()) {
			result.status = RestoreChainStatus::MISSING_BASE_FULL;
			result.chain.clear();
			return result;
		}
		current = record.previousBackupFileName;
	}

	reverse(result.chain.begin(), result.chain.end());
	reverse(recordChain.begin(), recordChain.end());
	if (result.chain.empty() || recordChain.empty()) {
		result.status = RestoreChainStatus::INVALID;
		return result;
	}

	const wstring firstName = result.chain.front().filename().wstring();
	const wstring firstType = recordChain.front().backupType.empty() ? firstName : recordChain.front().backupType;
	if (!FolderRewindFormat::IsFullLikeBackupType(firstType)) {
		result.chain.clear();
		result.status = RestoreChainStatus::MISSING_BASE_FULL;
		return result;
	}

	result.status = RestoreChainStatus::OK;
	result.usedMetadata = true;
	return result;
}

static vector<filesystem::path> BuildLegacyForwardRestoreChain(const filesystem::path& backupDir, const filesystem::path& targetBackupPath) {
	vector<filesystem::path> backupsToApply;
	const auto targetTime = filesystem::last_write_time(targetBackupPath);

	if (FolderRewindFormat::IsSmartBackupType(targetBackupPath.filename().wstring())) {
		filesystem::path baseFullBackup;
		auto baseFullTime = filesystem::file_time_type{};
		for (const auto& entry : filesystem::directory_iterator(backupDir)) {
			if (!entry.is_regular_file()) continue;
			if (!FolderRewindFormat::IsFullLikeBackupType(entry.path().filename().wstring())) continue;
			auto entryTime = entry.last_write_time();
			if (entryTime < targetTime && entryTime > baseFullTime) {
				baseFullTime = entryTime;
				baseFullBackup = entry.path();
			}
		}
		if (baseFullBackup.empty()) {
			return {};
		}
		backupsToApply.push_back(baseFullBackup);
		for (const auto& entry : filesystem::directory_iterator(backupDir)) {
			if (!entry.is_regular_file()) continue;
			if (!FolderRewindFormat::IsSmartBackupType(entry.path().filename().wstring())) continue;
			auto entryTime = entry.last_write_time();
			if (entryTime > baseFullTime && entryTime <= targetTime) {
				backupsToApply.push_back(entry.path());
			}
		}
		sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
			return filesystem::last_write_time(a) < filesystem::last_write_time(b);
		});
		return backupsToApply;
	}

	backupsToApply.push_back(targetBackupPath);
	return backupsToApply;
}

static vector<filesystem::path> BuildReverseRestoreChain(const filesystem::path& backupDir, const filesystem::path& targetBackupPath) {
	vector<filesystem::path> backupsToApply;
	const auto targetTime = filesystem::last_write_time(targetBackupPath);
	for (const auto& entry : filesystem::directory_iterator(backupDir)) {
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() != targetBackupPath.extension()) continue;
		if (entry.last_write_time() >= targetTime) {
			backupsToApply.push_back(entry.path());
		}
	}
	sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
		return filesystem::last_write_time(a) > filesystem::last_write_time(b);
	});
	return backupsToApply;
}

static bool TryBuildSmartRestorePlan(const filesystem::path& metadataDir, const vector<filesystem::path>& chain, SmartRestorePlan& outPlan) {
	outPlan = SmartRestorePlan{};
	if (chain.empty()) return false;

	FolderRewindFormat::ChangeRecord baseRecord;
	if (!FolderRewindMetadataStore::LoadRecord(metadataDir, chain.front().filename().wstring(), baseRecord) || baseRecord.fullFileList.empty()) {
		return false;
	}

	map<wstring, wstring> owners;
	for (const auto& file : baseRecord.fullFileList) {
		if (!file.empty()) owners[file] = chain.front().filename().wstring();
	}

	for (size_t i = 1; i < chain.size(); ++i) {
		FolderRewindFormat::ChangeRecord record;
		if (!FolderRewindMetadataStore::LoadRecord(metadataDir, chain[i].filename().wstring(), record)) {
			return false;
		}

		for (const auto& deleted : record.deletedFiles) {
			owners.erase(deleted);
		}
		for (const auto& added : record.addedFiles) {
			owners[added] = record.archiveFileName;
		}
		for (const auto& modified : record.modifiedFiles) {
			owners[modified] = record.archiveFileName;
		}

		set<wstring> expected(record.fullFileList.begin(), record.fullFileList.end());
		if (owners.size() != expected.size()) {
			return false;
		}
		for (const auto& pair : owners) {
			if (!expected.count(pair.first)) {
				return false;
			}
		}
	}

	map<wstring, filesystem::path> archiveLookup;
	map<wstring, size_t> archiveOrder;
	for (size_t i = 0; i < chain.size(); ++i) {
		archiveLookup[chain[i].filename().wstring()] = chain[i];
		archiveOrder[chain[i].filename().wstring()] = i;
	}

	map<wstring, vector<wstring>> groupedFiles;
	for (const auto& pair : owners) {
		groupedFiles[pair.second].push_back(pair.first);
	}

	vector<SmartRestoreArchiveGroup> groups;
	for (auto& pair : groupedFiles) {
		auto archiveIt = archiveLookup.find(pair.first);
		if (archiveIt == archiveLookup.end()) continue;
		sort(pair.second.begin(), pair.second.end());
		groups.push_back({ archiveIt->second, pair.second });
	}
	sort(groups.begin(), groups.end(), [&](const SmartRestoreArchiveGroup& a, const SmartRestoreArchiveGroup& b) {
		return archiveOrder[a.archive.filename().wstring()] < archiveOrder[b.archive.filename().wstring()];
	});

	outPlan.chain = chain;
	outPlan.archiveGroups = std::move(groups);
	return true;
}

static bool ApplySmartRestorePlan(const SmartRestorePlan& plan, const filesystem::path& destinationFolder, const Config& config) {
	vector<SmartRestoreArchiveGroup> groups;
	for (const auto& group : plan.archiveGroups) {
		if (!group.files.empty()) {
			groups.push_back(group);
		}
	}
	if (groups.empty()) {
		return true;
	}

	for (size_t i = 0; i < groups.size(); ++i) {
		const auto& group = groups[i];
		RESTORE_INFO(L("RESTORE_STEPS"), i + 1, groups.size(), wstring_to_utf8(group.archive.filename().wstring()).c_str());

		wstringstream fileNameBuilder;
		fileNameBuilder << L"MineBackup_Restore_" << chrono::steady_clock::now().time_since_epoch().count() << L"_" << i << L".txt";
		filesystem::path listFile = GetAppPaths().runtimeRoot / fileNameBuilder.str();
		try {
			ofstream out(listFile, ios::binary | ios::trunc);
			for (const auto& file : group.files) {
				string utf8Path = wstring_to_utf8(file);
				out.write(utf8Path.data(), static_cast<std::streamsize>(utf8Path.size()));
				out.put('\n');
			}
			out.close();

			if (!RunInternalProcess(MakeInternalProcess(config.zipPath,
				{L"x", group.archive.wstring(), L"@" + listFile.wstring(), L"-o" + destinationFolder.wstring(), L"-y"},
				{}, config.useLowPriority))) {
				filesystem::remove(listFile);
				return false;
			}
		}
		catch (...) {
			filesystem::remove(listFile);
			return false;
		}
		filesystem::remove(listFile);
	}

	return true;
}

} // namespace

bool DoRestore2(const Config& config, const wstring& worldName, const filesystem::path& fullBackupPath, int restoreMethod) {
	minebackup::logging::ScopedLogContext operationContext{{
		"operation_id", wstring_to_utf8(FolderRewindFormat::GenerateGuidString())},
		{"config_id", wstring_to_utf8(config.configId)},
		{"world", wstring_to_utf8(worldName)}};
	if (config.pendingLocalBinding) {
		RESTORE_WARNING("Restore is disabled until local paths are bound.");
		return false;
	}
	filesystem::path destinationFolder = JoinPath(config.saveRoot, worldName);
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

	filesystem::path safeRestoreTempDir;
	string workspaceError;
	bool safeWorkspacePrepared = false;
	if (restoreMethod == 0) {
		if (!TryPrepareSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, workspaceError)) {
			if (!safeRestoreTempDir.empty()) {
				string rollbackError;
				if (!TryRollbackSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, rollbackError)) {
					RESTORE_ERROR("Failed to rollback after workspace prepare failure: %s", rollbackError.c_str());
				}
			}
			RESTORE_ERROR("Failed to prepare safe restore workspace: %s", workspaceError.c_str());
			return failRestore("snapshot_prepare_failed");
		}
		safeWorkspacePrepared = !safeRestoreTempDir.empty();
	}
	else {
		error_code ec;
		filesystem::create_directories(destinationFolder, ec);
	}

	bool restoreSucceeded = ApplyRestoreChain(backupsToApply, destinationFolder, config);
	if (restoreSucceeded) {
		CleanupInternalRestoreMarkers(destinationFolder);
		if (safeWorkspacePrepared) {
			const vector<wstring> effectiveRestoreWhitelist = BuildEffectiveRestoreWhitelist(restoreWhitelist);
			if (!TryCommitSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, effectiveRestoreWhitelist, workspaceError)) {
				restoreSucceeded = false;
				RESTORE_ERROR("Failed to commit safe restore workspace: %s", workspaceError.c_str());
			}
		}
	}

	if (!restoreSucceeded) {
		if (safeWorkspacePrepared) {
			if (!TryRollbackSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, workspaceError)) {
				RESTORE_ERROR("Failed to rollback safe restore workspace: %s", workspaceError.c_str());
			}
		}
		return failRestore("command_failed");
	}

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
	const string& requestId) {
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
	filesystem::path destinationFolder = JoinPath(config.saveRoot, worldName);
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

	filesystem::path sourceDir = JoinPath(config.backupPath, worldName);
	filesystem::path targetBackupPath = sourceDir / backupFile;
	const int resolvedConfigIndex = ResolveConfigIndexForCloud(config);
	const MigrationUnitResult migration = MigrationCoordinator::EnsureWorldMigrated(config, resolvedConfigIndex, worldName, destinationFolder.wstring());
	if ((migration.status == MigrationStatus::Failed || migration.status == MigrationStatus::Degraded)
		&& FolderRewindFormat::IsSmartBackupType(backupFile) && restoreMethod == 0) {
		RESTORE_ERROR("Exact Smart restore is unavailable until metadata migration succeeds: %s", wstring_to_utf8(migration.message).c_str());
		return failRestore("legacy_metadata_migration_incomplete");
	}
	HistoryEntry targetHistoryEntry;
	const bool hasHistoryEntry = resolvedConfigIndex >= 0
		&& TryGetHistoryEntry(resolvedConfigIndex, worldName, backupFile, targetHistoryEntry);

	// 云存档补链发生在本地存在性校验之前：
	// 这样本地缺包、增量链缺失元数据时，都可以先尝试从云端补齐。
	if (hasHistoryEntry && config.cloudAutoDownloadBeforeRestore) {
		EnsureRestoreChainAvailable(config, resolvedConfigIndex, targetHistoryEntry);
	}

	if ((!FolderRewindFormat::IsSmartBackupType(backupFile) && !FolderRewindFormat::IsFullLikeBackupType(backupFile)) || !filesystem::exists(targetBackupPath)) {
		RESTORE_ERROR(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(backupFile).c_str());
		return failRestore("backup_not_found");
	}

	const bool targetIsIncremental = FolderRewindFormat::IsSmartBackupType(backupFile);
	const filesystem::path metadataDir = GetMetadataDirectory(config, worldName);
	RestoreChainResult chainResult;
	vector<filesystem::path> backupsToApply;

	if (restoreMethod == 2) {
		backupsToApply = BuildReverseRestoreChain(sourceDir, targetBackupPath);
		if (backupsToApply.empty()) {
			RESTORE_ERROR(L("LOG_BACKUP_SMART_NO_FOUND"));
			return failRestore("reverse_chain_not_found");
		}
	}
	else if (targetIsIncremental) {
		chainResult = BuildMetadataRestoreChain(metadataDir, sourceDir, targetBackupPath);
		if (chainResult.status == RestoreChainStatus::OK) {
			backupsToApply = chainResult.chain;
		}
		else {
			if (restoreMethod == 0) {
				RESTORE_INFO("Current MineBackup uses FolderRewind records/*.json for Smart clean restore.");
				RESTORE_ERROR("Exact Clean Restore for Smart backups requires valid metadata and an intact full base.");
				return failRestore("exact_clean_restore_unavailable");
			}

			backupsToApply = BuildLegacyForwardRestoreChain(sourceDir, targetBackupPath);
			if (backupsToApply.empty()) {
				RESTORE_ERROR(L("LOG_BACKUP_SMART_NO_FOUND"));
				return failRestore("restore_chain_not_found");
			}
		}
	}
	else {
		backupsToApply.push_back(targetBackupPath);
	}

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

	if (!ValidateRestoreArchives(backupsToApply, config)) {
		return failRestore("archive_integrity_check_failed");
	}

	SmartRestorePlan smartRestorePlan;
	const bool useExactSmartCleanRestore = restoreMethod == 0 && targetIsIncremental && chainResult.status == RestoreChainStatus::OK && chainResult.usedMetadata;
	if (useExactSmartCleanRestore) {
		if (!TryBuildSmartRestorePlan(metadataDir, backupsToApply, smartRestorePlan)) {
			RESTORE_ERROR("Smart restore metadata is incomplete or inconsistent. Clean restore aborted to protect data.");
			return failRestore("smart_restore_plan_invalid");
		}
	}

	filesystem::path safeRestoreTempDir;
	string workspaceError;
	bool safeWorkspacePrepared = false;
	if (restoreMethod == 0) {
		if (!TryPrepareSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, workspaceError)) {
			if (!safeRestoreTempDir.empty()) {
				string rollbackError;
				if (!TryRollbackSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, rollbackError)) {
					RESTORE_ERROR("Failed to rollback after workspace prepare failure: %s", rollbackError.c_str());
				}
			}
			RESTORE_ERROR("Failed to prepare safe restore workspace: %s", workspaceError.c_str());
			return failRestore("snapshot_prepare_failed");
		}
		safeWorkspacePrepared = !safeRestoreTempDir.empty();
	}
	else {
		error_code ec;
		filesystem::create_directories(destinationFolder, ec);
	}

	bool restoreSucceeded = false;
	if (useExactSmartCleanRestore) {
		restoreSucceeded = ApplySmartRestorePlan(smartRestorePlan, destinationFolder, config);
	}
	else {
		restoreSucceeded = ApplyRestoreChain(backupsToApply, destinationFolder, config, filesToExtract);
	}

	if (restoreSucceeded) {
		CleanupInternalRestoreMarkers(destinationFolder);
		if (safeWorkspacePrepared) {
			const vector<wstring> effectiveRestoreWhitelist = BuildEffectiveRestoreWhitelist(
				restoreWhitelistOverride ? *restoreWhitelistOverride : restoreWhitelist);
			if (!TryCommitSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, effectiveRestoreWhitelist, workspaceError)) {
				restoreSucceeded = false;
				RESTORE_ERROR("Failed to commit safe restore workspace: %s", workspaceError.c_str());
			}
		}
	}

	if (!restoreSucceeded) {
		if (safeWorkspacePrepared) {
			if (!TryRollbackSafeRestoreWorkspace(destinationFolder, safeRestoreTempDir, workspaceError)) {
				RESTORE_ERROR("Failed to rollback safe restore workspace: %s", workspaceError.c_str());
			}
		}
		return failRestore("command_failed");
	}

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
	const string& requestId) {
	(void)deleteBackup;
	Config config = world.config;
	auto& mod = g_appState.knotLinkMod;
	const string operationId = requestId.empty()
		? wstring_to_utf8(FolderRewindFormat::GenerateGuidString()) : requestId;
	minebackup::logging::ScopedLogContext operationContext{{
		"operation_id", operationId},
		{"config_id", wstring_to_utf8(world.config.configId)},
		{"world", wstring_to_utf8(world.name)}};
	auto broadcastLifecycle = [&](string_view eventName,
		minebackup::knotlink::KnotLinkProtocolFormatter::Fields fields = {}) {
		if (!requestId.empty()) fields.emplace_back("request_id", requestId);
		BroadcastEvent(eventName, fields);
	};
	RESTORE_INFO(L("KNOTLINK_HOT_RESTORE_START"), wstring_to_utf8(world.name).c_str());

	mod.resetForOperation();
	broadcastLifecycle("pre_hot_restore", {
		{"config", to_string(world.configIndex)}, {"world", wstring_to_utf8(world.name)}});
	RESTORE_INFO(L("KNOTLINK_WAITING_WORLD_SAVE_EXIT"));
	const bool exitComplete = mod.waitForFlag(
		&KnotLinkModInfo::worldSaveAndExitComplete,
		chrono::milliseconds(10000));
	if (!exitComplete) {
		RESTORE_WARNING(L("KNOTLINK_HOT_RESTORE_TIMEOUT"));
		broadcastLifecycle("restore_cancelled", {
			{"reason", "timeout"}, {"world", wstring_to_utf8(world.name)}});
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		g_appState.isRespond = false;
		return false;
	}

	RESTORE_INFO(L("KNOTLINK_MOD_EXIT_CONFIRMED"));
	const auto releaseDeadline = chrono::steady_clock::now() + chrono::seconds(15);
	bool worldReleased = false;
	while (chrono::steady_clock::now() < releaseDeadline) {
		if (!IsWorldOccupied(world.path)) {
			worldReleased = true;
			break;
		}
		this_thread::sleep_for(chrono::milliseconds(500));
	}
	if (!worldReleased) {
		RESTORE_WARNING(L("KNOTLINK_HOT_RESTORE_WORLD_OCCUPIED"));
		broadcastLifecycle("restore_cancelled", {
			{"reason", "world_occupied"}, {"world", wstring_to_utf8(world.name)}});
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		g_appState.isRespond = false;
		return false;
	}

	const filesystem::path levelDat = filesystem::path(world.path) / L"level.dat";
	const auto lockDeadline = chrono::steady_clock::now() + chrono::seconds(10);
	while (chrono::steady_clock::now() < lockDeadline && IsFileLocked(levelDat.wstring())) {
		this_thread::sleep_for(chrono::milliseconds(200));
	}
	this_thread::sleep_for(chrono::milliseconds(500));

	g_appState.hotkeyRestoreState = HotRestoreState::RESTORING;
	RESTORE_INFO(L("KNOTLINK_HOT_RESTORE_PROCEEDING"));

	const filesystem::path backupDirectory = JoinPath(config.backupPath, world.name);
	filesystem::path targetBackup;
	if (!backupFile.empty()) {
		targetBackup = backupDirectory / backupFile;
	}
	else if (filesystem::exists(backupDirectory)) {
		auto latestTime = filesystem::file_time_type{};
		for (const auto& entry : filesystem::directory_iterator(backupDirectory)) {
			const wstring fileName = entry.path().filename().wstring();
			if (entry.is_regular_file()
				&& (FolderRewindFormat::IsSmartBackupType(fileName)
					|| FolderRewindFormat::IsFullLikeBackupType(fileName))
				&& entry.last_write_time() > latestTime) {
				latestTime = entry.last_write_time();
				targetBackup = entry.path();
			}
		}
	}

	if (targetBackup.empty()) {
		RESTORE_WARNING(L("LOG_NO_BACKUP_FOUND"));
		broadcastLifecycle("restore_finished", {{"status", "failure"},
			{"reason", "no_backup_found"}, {"world", wstring_to_utf8(world.name)}});
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		g_appState.isRespond = false;
		return false;
	}

	RESTORE_INFO(
		L("LOG_RESTORE_USING_FILE"),
		wstring_to_utf8(targetBackup.filename().wstring()).c_str());
	if (!DoRestore(
		config,
		world.name,
		targetBackup.filename().wstring(),
		restoreMethod,
		customRestoreList,
		restoreWhitelistOverride,
		requestId)) {
		broadcastLifecycle("restore_finished", {{"status", "failure"},
			{"reason", "restore_failed"}, {"world", wstring_to_utf8(world.name)}});
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		g_appState.isRespond = false;
		return false;
	}

	this_thread::sleep_for(chrono::milliseconds(100));
	broadcastLifecycle("restore_finished", {{"status", "success"},
		{"config", to_string(world.configIndex)}, {"world", wstring_to_utf8(world.name)}});
	RESTORE_INFO(L("KNOTLINK_HOT_RESTORE_DONE"));
	this_thread::sleep_for(chrono::milliseconds(3000));

	broadcastLifecycle("rejoin_world", {{"world", wstring_to_utf8(world.name)}});
	RESTORE_INFO(L("KNOTLINK_REJOIN_SENT"));
	const bool responseReceived = mod.waitForFlag(
		&KnotLinkModInfo::rejoinResponseReceived,
		chrono::milliseconds(30000));
	if (responseReceived) {
		bool rejoinSucceeded = false;
		{
			lock_guard<mutex> lock(mod.mtx);
			rejoinSucceeded = mod.rejoinSuccess;
		}
		if (rejoinSucceeded) {
			RESTORE_INFO(L("KNOTLINK_REJOIN_OK"));
			broadcastLifecycle("hot_restore_complete", {{"status", "full_success"},
				{"world", wstring_to_utf8(world.name)}});
		}
		else {
			RESTORE_WARNING(L("KNOTLINK_REJOIN_FAIL"));
			broadcastLifecycle("hot_restore_complete", {{"status", "restore_ok_rejoin_failed"},
				{"world", wstring_to_utf8(world.name)}});
		}
	}
	else {
		RESTORE_WARNING(L("KNOTLINK_REJOIN_TIMEOUT"));
		broadcastLifecycle("hot_restore_complete", {{"status", "restore_ok_rejoin_timeout"},
			{"world", wstring_to_utf8(world.name)}});
	}

	g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
	g_appState.isRespond = false;
	return true;
}
