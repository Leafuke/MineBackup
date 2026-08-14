#include "BackupManager.h"
#include "BackupManagerInternal.h"

#include "AppPaths.h"
#include "ChainSafeRetention.h"
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

void BackupManagerInternal::LimitBackupFiles(
	const Config& config,
	const int& configIndex,
	const wstring& folderPath,
	int limit) {
	if (limit <= 0) return;
	vector<filesystem::directory_entry> files;
	try {
		if (!filesystem::is_directory(folderPath)) return;
		for (const auto& entry : filesystem::directory_iterator(folderPath)) {
			if (entry.is_regular_file()) files.push_back(entry);
		}
	}
	catch (const filesystem::filesystem_error& error) {
		BACKUP_ERROR(L("LOG_ERROR_SCAN_BACKUP_DIR"), error.what());
		return;
	}
	if (static_cast<int>(files.size()) <= limit) return;

	const vector<HistoryEntry> history = GetHistoryEntriesForConfig(configIndex);
	const bool historyAvailable = !history.empty();
	sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
		return left.last_write_time() < right.last_write_time();
	});

	vector<filesystem::directory_entry> deletable;
	for (const auto& file : files) {
		bool important = false;
		if (historyAvailable) {
			for (const auto& entry : history) {
				if (entry.worldName == file.path().parent_path().filename().wstring()
					&& entry.backupFile == file.path().filename().wstring()) {
					important = entry.isImportant;
					if (important) {
						BACKUP_INFO(
							L("LOG_INFO_BACKUP_MARKED_IMPORTANT"),
							wstring_to_utf8(file.path().filename().wstring()).c_str());
					}
					break;
				}
			}
		}
		if (!important) deletable.push_back(file);
	}
	if (static_cast<int>(files.size() - deletable.size()) >= limit) {
		BACKUP_INFO("Cannot delete more files; remaining backups are marked as important.");
		return;
	}

	const size_t deleteCount = static_cast<size_t>(max(
		0,
		static_cast<int>(files.size()) - limit));
	for (size_t index = 0; index < deleteCount && index < deletable.size(); ++index) {
		const auto& file = deletable[index];
		try {
			if (FolderRewindFormat::IsSmartBackupType(file.path().filename().wstring())) {
				// 记录实际选择的可删除文件，而不是原始文件列表中的同下标元素。
				BACKUP_WARNING(
					L("LOG_WARNING_DELETE_SMART_BACKUP"),
					wstring_to_utf8(file.path().filename().wstring()).c_str());
			}

			bool handledThroughHistory = false;
			if (historyAvailable) {
				for (const auto& entry : history) {
					if (entry.worldName == file.path().parent_path().filename().wstring()
						&& entry.backupFile == file.path().filename().wstring()) {
						if (isSafeDelete) {
							DoSafeDeleteBackupShared(config, entry, configIndex);
						}
						else {
							int mutableConfigIndex = configIndex;
							DoDeleteBackup(config, entry, mutableConfigIndex);
						}
						handledThroughHistory = true;
						break;
					}
				}
			}
			if (!isSafeDelete && !handledThroughHistory) {
				filesystem::remove(file);
				InvalidateBackupMetadata(
					config,
					file.path().parent_path().filename().wstring(),
					file.path().filename().wstring());
				RemoveHistoryEntry(configIndex, file.path().filename().wstring());
			}
			BACKUP_INFO(
				L("LOG_DELETE_OLD_BACKUP"),
				wstring_to_utf8(file.path().filename().wstring()).c_str());
		}
		catch (const filesystem::filesystem_error& error) {
			BACKUP_ERROR(L("LOG_ERROR_DELETE_BACKUP"), error.what());
		}
	}
}

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
	request.commitHistory = [configIndex](vector<HistoryEntry> updated) {
		return ReplaceHistoryEntriesForConfig(configIndex, std::move(updated));
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
