#include "BackupManager.h"
#include "BackupManagerInternal.h"

#include "AppPaths.h"
#include "ChainSafeRetention.h"
#include "AppState.h"
#include "RuntimeRetentionService.h"
#include "WorldIdentity.h"
#include "TaskCoordinator.h"
#include "CloudSyncService.h"
#include "ConfigManager.h"
#include "FolderRewindFormat.h"
#include "Globals.h"
#include "HistoryManager.h"
#include "Logging.h"
#include "MigrationCoordinator.h"
#include "PlatformCompat.h"
#include "text_to_text.h"
#include "i18n.h"

#include <algorithm>
#include <filesystem>

using namespace std;
using namespace BackupManagerInternal;

#define BACKUP_INFO(...) MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Backup, "backup.progress", __VA_ARGS__)
#define BACKUP_WARNING(...) MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Backup, "backup.warning", __VA_ARGS__)
#define BACKUP_ERROR(...) MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Backup, "backup.error", __VA_ARGS__)

namespace {

bool DeleteLocalArchiveOnly(const Config& config, const HistoryEntry& entry) {
	const filesystem::path archive =
		JoinPath(config.backupPath, entry.worldName) / entry.backupFile;
	try {
		if (!filesystem::exists(archive)) {
			BACKUP_ERROR(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(entry.backupFile).c_str());
			return false;
		}
		filesystem::remove(archive);
		BACKUP_INFO("  - %s OK", wstring_to_utf8(archive.filename().wstring()).c_str());
		return true;
	}
	catch (const filesystem::filesystem_error& error) {
		BACKUP_ERROR(
			L("LOG_ERROR_DELETE_BACKUP"),
			wstring_to_utf8(archive.filename().wstring()).c_str(),
			error.what());
		return false;
	}
}

} // namespace

void DoSafeDeleteBackupShared(
	const Config& config,
	const HistoryEntry& entry,
	int configIndex);

void DeleteBackupWithMode(
	const Config& config,
	const HistoryEntry& entry,
	int configIndex,
	BackupDeleteMode mode,
	bool useSafeDelete) {
	minebackup::logging::ScopedLogContext context{{
		"operation_id", wstring_to_utf8(FolderRewindFormat::GenerateGuidString())},
		{"config_id", wstring_to_utf8(config.configId)},
		{"world", wstring_to_utf8(entry.worldName)}};
	if (config.pendingLocalBinding) {
		BACKUP_WARNING("This imported configuration is waiting for local path binding.");
		return;
	}
	if (mode == BackupDeleteMode::HistoryOnly) {
		RemoveHistoryEntry(configIndex, entry.worldName, entry.backupFile);
		QueueConfigurationHistorySyncAfterLocalChange(config, configIndex, "history deletion");
		return;
	}

	const auto migration = MigrationCoordinator::EnsureWorldMigrated(
		config,
		configIndex,
		entry.worldName,
		entry.worldPath);
	if (migration.status == MigrationStatus::Failed
		|| migration.status == MigrationStatus::Degraded) {
		BACKUP_ERROR(
			"Local archive deletion is blocked until metadata migration succeeds: %s",
			wstring_to_utf8(migration.message).c_str());
		return;
	}
	if (mode == BackupDeleteMode::LocalArchiveOnly) {
		if (DeleteLocalArchiveOnly(config, entry)) {
			InvalidateBackupMetadata(config, entry.worldName, entry.backupFile);
		}
		return;
	}
	if (useSafeDelete
		&& (FolderRewindFormat::IsSmartBackupType(entry.backupType)
			|| FolderRewindFormat::IsSmartBackupType(entry.backupFile))) {
		DoSafeDeleteBackupShared(config, entry, configIndex);
	}
	else {
		DoDeleteBackup(config, entry, configIndex);
	}
}

void DoDeleteBackup(const Config& config, const HistoryEntry& entry, int& configIndex) {
	BACKUP_INFO(L("LOG_PRE_TO_DELETE"), wstring_to_utf8(entry.backupFile).c_str());
	const filesystem::path archive =
		JoinPath(config.backupPath, entry.worldName) / entry.backupFile;
	try {
		if (filesystem::exists(archive)) {
			filesystem::remove(archive);
			BACKUP_INFO("  - %s OK", wstring_to_utf8(archive.filename().wstring()).c_str());
		}
		else {
			BACKUP_ERROR(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(entry.backupFile).c_str());
		}
		InvalidateBackupMetadata(config, entry.worldName, archive.filename().wstring());
		RemoveHistoryEntry(configIndex, entry.worldName, archive.filename().wstring());
	}
	catch (const filesystem::filesystem_error& error) {
		BACKUP_ERROR(
			L("LOG_ERROR_DELETE_BACKUP"),
			wstring_to_utf8(archive.filename().wstring()).c_str(),
			error.what());
	}
	QueueConfigurationHistorySyncAfterLocalChange(config, configIndex, "backup deletion");
}

void DoSafeDeleteBackupShared(
	const Config& config,
	const HistoryEntry& entry,
	int configIndex) {
	const auto migration = MigrationCoordinator::EnsureWorldMigrated(
		config, configIndex, entry.worldName, entry.worldPath);
	if (migration.status == MigrationStatus::Failed
		|| migration.status == MigrationStatus::Degraded) {
		BACKUP_WARNING(
			"Safe retention requires complete metadata migration: %s",
			wstring_to_utf8(migration.message).c_str());
		return;
	}
	FolderRewindFormat::StoragePaths storage;
	if (!FolderRewindFormat::TryResolveStoragePaths(
			config.backupPath, entry.worldName, entry.worldPath, storage)) {
		BACKUP_WARNING("Safe retention could not resolve the world storage path.");
		return;
	}
	ArchiveRunner archiveRunner = ArchiveRunner::Resolve(
		config.zipPath, GetAppPaths());
	ChainSafeRetention::Request request;
	request.config = config;
	request.entry = entry;
	request.history = GetHistoryEntriesForConfig(configIndex);
	request.backupDirectory = storage.backupSubDir;
	request.metadataDirectory = storage.metadataDir;
	request.paths = GetAppPaths();
	request.archiveRunner = &archiveRunner;
	request.commitHistory = [config](const ChainSafeRetention::HistoryChanges& changes) {
        const auto mutation = GetHistoryRepository().Mutate(config.configId, GetAppPaths().HistoryFile(),
            SnapshotConfigState().configs, true, [&](vector<HistoryEntry>& latest) {
                return ChainSafeRetention::ApplyHistoryChanges(config, latest, changes);
            });
        return mutation.changed && mutation.persisted;
    };
	const auto result = ChainSafeRetention::Remove(std::move(request));
	if (result.warning) {
		BACKUP_WARNING("Safe retention kept %s: %s",
			wstring_to_utf8(entry.backupFile).c_str(), result.detail.c_str());
	}
	if (result.changed) {
		QueueConfigurationHistorySyncAfterLocalChange(config, configIndex, "safe retention");
	}
}

// 保留旧的验证入口，但实际实现统一走桌面端与 runtime 共用的链安全删除器。
void DoSafeDeleteBackup(
	const Config& config,
	const HistoryEntry& entry,
	int configIndex) {
	DoSafeDeleteBackupShared(config, entry, configIndex);
}
