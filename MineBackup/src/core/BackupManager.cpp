#include "Broadcast.h"
#include "ArchiveRunner.h"
#include "BackupChangeDetector.h"
#include "BackupManager.h"
#include "BackupManagerInternal.h"
#include "AppState.h"
#include "AppPaths.h"
#include "Globals.h"
#include "text_to_text.h"
#include "i18n.h"
#include "Logging.h"
#include "HistoryManager.h"
#include "CloudSyncService.h"
#include "ConfigManager.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "MigrationCoordinator.h"
#include "ProcessRunner.h"
#include "TaskCoordinator.h"
#include "ExternalToolManager.h"
#include "GameSessionManager.h"
#include "PathRuleSet.h"
#include "FileName.h"
#include "json.hpp"
#include "PlatformCompat.h"
#include "DesktopServices.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <cwctype>
#include <set>
#include <regex>
using namespace std;

#define BACKUP_DEBUG(...) MB_LOG_PRINTF_DEBUG(minebackup::logging::LogCategory::Backup, "backup.debug", __VA_ARGS__)
#define BACKUP_INFO(...) MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Backup, "backup.progress", __VA_ARGS__)
#define BACKUP_WARNING(...) MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Backup, "backup.warning", __VA_ARGS__)
#define BACKUP_ERROR(...) MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Backup, "backup.error", __VA_ARGS__)
#define RESTORE_INFO(...) MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Restore, "restore.progress", __VA_ARGS__)
#define RESTORE_WARNING(...) MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Restore, "restore.warning", __VA_ARGS__)
#define RESTORE_ERROR(...) MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Restore, "restore.error", __VA_ARGS__)

namespace BackupManagerInternal {

ScopedRuntimeArtifact::ScopedRuntimeArtifact(filesystem::path path)
	: path_(std::move(path)) {
}

ScopedRuntimeArtifact::~ScopedRuntimeArtifact() {
	error_code ignored;
	filesystem::remove_all(path_, ignored);
}

ProcessSpec MakeInternalProcess(
	const filesystem::path& executable,
	vector<wstring> arguments,
	const filesystem::path& workingDirectory,
	bool useLowPriority) {
	ProcessSpec spec;
	const auto runner = ArchiveRunner::Resolve(
		executable,
		GetAppPaths(),
		TaskCoordinator::CurrentStopToken());
	spec.executable = runner.IsAvailable()
		? runner.Resolution().executable
		: executable;
	spec.arguments = std::move(arguments);
	spec.workingDirectory = workingDirectory;
	spec.useLowPriority = useLowPriority;
	return spec;
}

bool RunInternalProcess(const ProcessSpec& spec) {
	minebackup::logging::ScopedLogContext processContext{{
		"executable", wstring_to_utf8(spec.executable.filename().wstring())},
		{"working_directory", spec.workingDirectory.empty() ? "default" : "custom"}};
	MB_LOG_DEBUG(minebackup::logging::LogCategory::Process,
		"process.started", "External process started.");
	const auto result = ProcessRunner::Run(spec);
	if (!result.standardOutput.empty()) {
		minebackup::logging::LogRaw(minebackup::logging::LogCategory::Process,
			"process.stdout", result.standardOutput, minebackup::logging::LogLevel::Debug, MB_LOG_SOURCE);
	}
	if (!result.standardError.empty()) {
		minebackup::logging::LogRaw(minebackup::logging::LogCategory::Process,
			"process.stderr", result.standardError, minebackup::logging::LogLevel::Debug, MB_LOG_SOURCE);
	}
	if (result.status == ProcessStatus::Succeeded) {
		MB_LOG_INFO(minebackup::logging::LogCategory::Process,
			"process.completed", "External process completed successfully.");
		return true;
	}
	MB_LOG_ERROR(minebackup::logging::LogCategory::Process,
		"process.failed", "External process failed with exit code {}: {}",
		result.exitCode, wstring_to_utf8(result.error));
	if (result.exitCode == 2) BACKUP_WARNING(L("LOG_7Z_ERROR_SUGGESTION"));
	return false;
}

vector<wstring> SevenZipCreateArguments(const Config& config, int level, const filesystem::path& archive) {
	return ArchiveRunner::BuildCreateArguments(config, level, archive);
}

const char* FolderStateToI18nKey(FolderState state) {
	switch (state) {
	case FolderState::BACKUP: return "OP_BACKUP";
	case FolderState::RESTORE: return "OP_RESTORE";
	default: return "OP_BACKUP";
	}
}

static void ToLowerInPlace(wstring& s) {
#ifdef _WIN32
	for (wchar_t& ch : s) ch = (wchar_t)towlower(ch);
#else
	(void)s;
#endif
}

bool IsAsciiOnlyPath(const wstring& value) {
	for (wchar_t ch : value) {
		if (static_cast<unsigned int>(ch) > 127u) {
			return false;
		}
	}
	return true;
}

int NormalizeCompressionLevel(const wstring& method, int level) {
	int minLevel = 1;
	int maxLevel = 9;
	if (_wcsicmp(method.c_str(), L"zstd") == 0) {
		maxLevel = 22;
	}
	if (level < minLevel) return minLevel;
	if (level > maxLevel) return maxLevel;
	return level;
}

static inline wstring MakeWorldOperationKey(const filesystem::path& worldPath) {
	error_code ec;
	filesystem::path p = worldPath;

	// Normalize to an absolute, lexically-normal path so the same folder maps to one key.
	auto abs = filesystem::absolute(p, ec);
	if (!ec) p = abs;
	p = p.lexically_normal();

	wstring key = p.wstring();

#ifdef _WIN32
	// Windows paths are case-insensitive; unify casing and separators.
	for (wchar_t& ch : key) {
		if (ch == L'/') ch = L'\\';
	}
	ToLowerInPlace(key);
#else
	for (wchar_t& ch : key) {
		if (ch == L'\\') ch = L'/';
	}
#endif

	return key;
}

static mutex g_worldOpMutex;
static unordered_map<wstring, FolderState> g_worldOpInProgress;

WorldOperationGuard::WorldOperationGuard(const filesystem::path& worldPath, FolderState requested)
	: key_(MakeWorldOperationKey(worldPath)), requested_(requested) {
	lock_guard<mutex> lock(g_worldOpMutex);
	const auto existing = g_worldOpInProgress.find(key_);
	if (existing == g_worldOpInProgress.end()) {
		g_worldOpInProgress.emplace(key_, requested_);
		acquired_ = true;
	}
	else {
		existing_ = existing->second;
	}
}

WorldOperationGuard::WorldOperationGuard(WorldOperationGuard&& other) noexcept {
	*this = std::move(other);
}

WorldOperationGuard& WorldOperationGuard::operator=(WorldOperationGuard&& other) noexcept {
	if (this == &other) return *this;
	Release();
	key_ = std::move(other.key_);
	requested_ = other.requested_;
	existing_ = other.existing_;
	acquired_ = other.acquired_;
	other.acquired_ = false;
	return *this;
}

WorldOperationGuard::~WorldOperationGuard() {
	Release();
}

bool WorldOperationGuard::Acquired() const {
	return acquired_;
}

FolderState WorldOperationGuard::Requested() const {
	return requested_;
}

FolderState WorldOperationGuard::Existing() const {
	return existing_;
}

void WorldOperationGuard::Release() {
	if (!acquired_) return;
	lock_guard<mutex> lock(g_worldOpMutex);
	g_worldOpInProgress.erase(key_);
	acquired_ = false;
}

	constexpr const wchar_t* kDeletedOnlyMarkerDir = FolderRewindFormat::kInternalRestoreMarkerDirectoryName;
	constexpr const wchar_t* kDeletedOnlyMarkerFile = FolderRewindFormat::kInternalRestoreMarkerFileName;
	const vector<wstring> kForcedBackupBlacklistRules = {
		L"regex:(^|[\\\\/])session\\.lock$",
		L"regex:(^|[\\\\/])lock$",
		L"regex:(^|[\\\\/]).*\\.lock$"
	};

	vector<wstring> BuildEffectiveBackupBlacklist(const vector<wstring>& userBlacklist) {
		vector<wstring> effective = userBlacklist;
		for (const auto& forcedRule : kForcedBackupBlacklistRules) {
			const bool exists = any_of(effective.begin(), effective.end(), [&](const wstring& item) {
				return _wcsicmp(item.c_str(), forcedRule.c_str()) == 0;
				});
			if (!exists) {
				effective.push_back(forcedRule);
			}
		}
		return effective;
	}

	filesystem::path GetMetadataDirectory(const Config& config, const wstring& worldName) {
		FolderRewindFormat::StoragePaths storagePaths;
		if (FolderRewindFormat::TryResolveStoragePaths(config.backupPath, worldName, L"", storagePaths)) {
			return storagePaths.metadataDir;
		}
		return filesystem::path(config.backupPath) / FolderRewindFormat::kMetadataRootDirName / FolderRewindFormat::SanitizePathSegment(worldName);
	}

	bool UpdateMetadataFiles(const filesystem::path& metadataDir, const wstring& currentBackupFile, const wstring& baseBackupFile, const wstring& backupType, const map<wstring, FolderRewindFormat::FileState>& currentState, const BackupChangeSet& changeSet) {
		FolderRewindFormat::MetadataState previousState;
		FolderRewindMetadataStore::LoadState(metadataDir, previousState);

		const wstring previousLastBackupFile = previousState.lastBackupFileName;
		const wstring normalizedBase = FolderRewindFormat::IsSmartBackupType(backupType)
			? (baseBackupFile.empty() ? currentBackupFile : baseBackupFile)
			: currentBackupFile;

		FolderRewindFormat::ChangeRecord record;
		record.archiveFileName = currentBackupFile;
		record.backupType = backupType;
		record.basedOnFullBackup = normalizedBase;
		record.previousBackupFileName = FolderRewindFormat::IsSmartBackupType(backupType) ? previousLastBackupFile : L"";
		record.createdAtUtc = FolderRewindFormat::MakeUtcTimestampString();
		record.addedFiles = changeSet.addedFiles;
		record.modifiedFiles = changeSet.modifiedFiles;
		record.deletedFiles = changeSet.deletedFiles;
		for (const auto& pair : currentState) {
			record.fullFileList.push_back(FolderRewindFormat::NormalizeRelativePath(pair.first));
		}
		sort(record.fullFileList.begin(), record.fullFileList.end());

		if (!FolderRewindFormat::IsSmartBackupType(backupType)) {
			record.previousBackupFileName.clear();
			record.basedOnFullBackup = currentBackupFile;
			record.addedFiles = record.fullFileList;
			record.modifiedFiles.clear();
			record.deletedFiles.clear();
		}

		FolderRewindFormat::MetadataState state;
		state.version = L"3.0";
		state.lastBackupTime = FolderRewindFormat::MakeLocalHistoryTimestampString();
		state.lastBackupFileName = currentBackupFile;
		state.basedOnFullBackup = record.basedOnFullBackup;
		for (const auto& pair : currentState) {
			state.fileStates[FolderRewindFormat::NormalizeRelativePath(pair.first)] = pair.second;
		}

		if (!FolderRewindMetadataStore::Save(metadataDir, state, record)) {
			error_code ec;
			filesystem::remove(FolderRewindMetadataStore::GetStatePath(metadataDir), ec);
			FolderRewindMetadataStore::DeleteRecord(metadataDir, currentBackupFile);
			return false;
		}
		return true;
	}

	void InvalidateBackupMetadata(const Config& config, const wstring& worldName, const wstring& deletedBackupFile, const wstring& renamedOldFile, const wstring& renamedNewFile) {
		filesystem::path metadataDir = GetMetadataDirectory(config, worldName);
		error_code ec;
		const bool hasRename = !renamedOldFile.empty() && !renamedNewFile.empty();
		if (hasRename) {
			FolderRewindMetadataStore::RewriteRecordArchiveName(metadataDir, renamedOldFile, renamedNewFile);
		}
		else if (!deletedBackupFile.empty()) {
			FolderRewindMetadataStore::DeleteRecord(metadataDir, deletedBackupFile);
		}

		FolderRewindFormat::MetadataState state;
		if (FolderRewindMetadataStore::LoadState(metadataDir, state)) {
			if (hasRename) {
				if (_wcsicmp(state.lastBackupFileName.c_str(), renamedOldFile.c_str()) == 0) state.lastBackupFileName = renamedNewFile;
				if (_wcsicmp(state.basedOnFullBackup.c_str(), renamedOldFile.c_str()) == 0) state.basedOnFullBackup = renamedNewFile;
				FolderRewindMetadataStore::SaveState(metadataDir, state);
			}
			else if (!deletedBackupFile.empty()) {
				filesystem::remove(FolderRewindMetadataStore::GetStatePath(metadataDir), ec);
			}
		}
		filesystem::remove(metadataDir / L"metadata.json", ec);
	}

	void ClearReadonlyAttributesRecursively(const filesystem::path& dir) {
		error_code ec;
		if (!filesystem::exists(dir, ec) || ec) return;
		for (const auto& entry : filesystem::recursive_directory_iterator(dir, filesystem::directory_options::skip_permission_denied, ec)) {
			if (ec) break;
			filesystem::permissions(entry.path(), filesystem::perms::owner_all, filesystem::perm_options::add, ec);
		}
		filesystem::permissions(dir, filesystem::perms::owner_all, filesystem::perm_options::add, ec);
	}

	bool CreateDeletionOnlyArchive(const Config& config, const filesystem::path& archivePath) {
		wstringstream nameBuilder;
		nameBuilder << L"MineBackup_DeleteOnly_" << chrono::steady_clock::now().time_since_epoch().count();
		filesystem::path tempDir = GetAppPaths().runtimeRoot / nameBuilder.str();
		bool success = false;
		try {
			filesystem::path internalDir = tempDir / kDeletedOnlyMarkerDir;
			filesystem::create_directories(internalDir);
			ofstream marker(internalDir / kDeletedOnlyMarkerFile, ios::binary | ios::trunc);
			marker << wstring_to_utf8(FolderRewindFormat::MakeUtcTimestampString());
			marker.close();

			const int normalizedZipLevel = NormalizeCompressionLevel(config.zipMethod, config.zipLevel);
			auto arguments = SevenZipCreateArguments(config, normalizedZipLevel, archivePath);
			arguments.push_back(L"*");
			success = RunInternalProcess(MakeInternalProcess(config.zipPath, std::move(arguments), tempDir, config.useLowPriority));
		}
		catch (const exception& ex) {
			BACKUP_ERROR("Failed to create deletion-only archive: %s", ex.what());
		}

		error_code ec;
		if (filesystem::exists(tempDir, ec) && !ec) {
			ClearReadonlyAttributesRecursively(tempDir);
			filesystem::remove_all(tempDir, ec);
		}
		return success;
	}
} // namespace BackupManagerInternal

using namespace BackupManagerInternal;

// 执行单个世界的备份操作。
BackupOutcome DoBackup(const MyFolder& folder, const wstring& comment) {
	minebackup::logging::ScopedLogContext operationContext{{
		"operation_id", wstring_to_utf8(FolderRewindFormat::GenerateGuidString())},
		{"config_id", wstring_to_utf8(folder.config.configId)},
		{"world", wstring_to_utf8(folder.name)}};
    const Config& config = folder.config;
	if (config.pendingLocalBinding) {
		BACKUP_WARNING("This imported configuration is waiting for local path binding.");
		return BackupOutcome::Rejected;
	}

	WorldOperationGuard opGuard(filesystem::path(folder.path), FolderState::BACKUP);
	if (!opGuard.Acquired()) {
		BACKUP_WARNING(
			L("LOG_OP_REJECTED_BUSY"),
			wstring_to_utf8(folder.name).c_str(),
			L(FolderStateToI18nKey(opGuard.Existing())),
			L(FolderStateToI18nKey(opGuard.Requested()))
		);
		return BackupOutcome::Rejected;
	}
	const MigrationUnitResult migration = MigrationCoordinator::EnsureWorldMigrated(config, folder.configIndex, folder.name, folder.path);
	const bool forceFullForMigration = migration.status == MigrationStatus::Failed || migration.status == MigrationStatus::Degraded;
	if (migration.status == MigrationStatus::Failed) {
		BACKUP_WARNING("Legacy metadata migration failed; this backup will establish a new Full chain: %s", wstring_to_utf8(migration.message).c_str());
	}
	else if (migration.status == MigrationStatus::Degraded) {
		BACKUP_WARNING("Legacy metadata was only partially migrated; forcing a safe Full backup.");
	}

	BACKUP_INFO(L("LOG_BACKUP_START_HEADER"));
	BACKUP_INFO(L("LOG_BACKUP_PREPARE"), wstring_to_utf8(folder.name).c_str());

	const ArchiveRunner archiveRunner = ArchiveRunner::Resolve(
		config.zipPath,
		GetAppPaths(),
		TaskCoordinator::CurrentStopToken());
    if (!archiveRunner.IsAvailable()) {
        BACKUP_ERROR(
			L("LOG_ERROR_7Z_NOT_FOUND"),
			wstring_to_utf8(archiveRunner.Resolution().diagnostic).c_str());
        BACKUP_ERROR(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
        return BackupOutcome::Failed;
    }

	wstring originalSourcePath = folder.path;
	wstring sourcePath = NormalizeSeparators(originalSourcePath);
	const vector<wstring> effectiveBlacklist = BuildEffectiveBackupBlacklist(config.blacklist);
	FolderRewindFormat::StoragePaths storagePaths;
	if (!FolderRewindFormat::TryResolveStoragePaths(config.backupPath, folder.name, folder.path, storagePaths)) {
		BACKUP_ERROR("Invalid FolderRewind storage folder name for world: %s", wstring_to_utf8(folder.name).c_str());
		return BackupOutcome::Failed;
	}
	filesystem::path destinationFolder = storagePaths.backupSubDir;
	filesystem::path metadataFolder = storagePaths.metadataDir;
	const wstring storageFolderName = storagePaths.folderName;
	ProcessSpec command;
	wstring archivePath;
	auto makeArchivePath = [&](const wstring& backupType) {
		return (destinationFolder / FolderRewindFormat::GenerateArchiveFileName(backupType, storageFolderName, comment, config.zipFormat)).wstring();
	};

	try {
		filesystem::create_directories(destinationFolder);
		filesystem::create_directories(metadataFolder);
		BACKUP_INFO(L("LOG_BACKUP_DIR_IS"), wstring_to_utf8(destinationFolder.wstring()).c_str());
    } catch (const filesystem::filesystem_error& e) {
        BACKUP_ERROR(L("LOG_ERROR_CREATE_BACKUP_DIR"), e.what());
        return BackupOutcome::Failed;
    }

	// 检测到 level.dat 被锁定，启用热备份握手并依赖 7z -ssw 直接从原世界路径压缩

    if (IsFileLocked(sourcePath + L"/level.dat") || IsFileLocked(sourcePath + L"/session.lock")) {
        // 在热备份前，先检查联动模组是否存在
        bool modAvailable = PerformModHandshake("backup", wstring_to_utf8(folder.name));

        if (modAvailable) {
            BACKUP_INFO(L("KNOTLINK_MOD_DETECTED_BACKUP"),
                g_appState.knotLinkMod.modVersion.c_str());
        } else {
            if (g_appState.knotLinkMod.modDetected.load() && !g_appState.knotLinkMod.versionCompatible.load()) {
                BACKUP_WARNING(L("KNOTLINK_MOD_VERSION_TOO_OLD"),
                    g_appState.knotLinkMod.modVersion.c_str(),
                    KnotLinkModInfo::MIN_MOD_VERSION);
            } else {
                BACKUP_WARNING(L("KNOTLINK_MOD_NOT_DETECTED_BACKUP"));
            }
        }

        if (modAvailable) {
            // 联动模组存在且版本兼容: 等待模组完成世界保存
            BACKUP_INFO(L("KNOTLINK_WAITING_WORLD_SAVE"));
			std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 握手和下一个广播之间必须有短暂延时
			// 通知模组准备热备份
			BroadcastEvent("event=pre_hot_backup;config=" + to_string(folder.configIndex) +
				";world=" + wstring_to_utf8(folder.name));
			bool saved = g_appState.knotLinkMod.waitForFlag(
				&KnotLinkModInfo::worldSaveComplete,
                std::chrono::milliseconds(10000)); // 最多等待10秒

            if (saved) {
                BACKUP_INFO(L("KNOTLINK_WORLD_SAVE_CONFIRMED"));
            } else {
                // 超时：模组未在规定时间内完成保存，停止
                BACKUP_WARNING(L("KNOTLINK_WORLD_SAVE_TIMEOUT"));
				return BackupOutcome::Rejected;
            }
        }
		BACKUP_INFO("Snapshot copy is disabled. Using 7-Zip -ssw to back up from live world files.");
	}

    bool forceFullBackup = true;
    if (filesystem::exists(destinationFolder)) {
        for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
            if (entry.is_regular_file() && FolderRewindFormat::IsFullLikeBackupType(entry.path().filename().wstring())) {
                forceFullBackup = false;
                break;
            }
        }
    }
	if (forceFullForMigration) forceFullBackup = true;
    if (forceFullBackup)
        BACKUP_INFO(L("LOG_FORCE_FULL_BACKUP"));

    bool forceFullBackupDueToLimit = false;
    if (config.backupMode == 2 && config.maxSmartBackupsPerFull > 0 && !forceFullBackup) {
        vector<filesystem::path> worldBackups;
        try {
            for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
                if (entry.is_regular_file()) {
                    worldBackups.push_back(entry.path());
                }
            }
        } catch (const filesystem::filesystem_error& e) {
            BACKUP_ERROR(L("LOG_ERROR_SCAN_BACKUP_DIR"), e.what());
        }

        if (!worldBackups.empty()) {
            sort(worldBackups.begin(), worldBackups.end(), [](const auto& a, const auto& b) {
                return filesystem::last_write_time(a) < filesystem::last_write_time(b);
            });

            int smartCount = 0;
            bool fullFound = false;
            for (auto it = worldBackups.rbegin(); it != worldBackups.rend(); ++it) {
                wstring filename = it->filename().wstring();
                if (FolderRewindFormat::IsFullLikeBackupType(filename)) {
                    fullFound = true;
                    break;
                }
                if (FolderRewindFormat::IsSmartBackupType(filename)) {
                    ++smartCount;
                }
            }

            if (fullFound && smartCount >= config.maxSmartBackupsPerFull) {
                forceFullBackupDueToLimit = true;
                BACKUP_INFO(L("LOG_FORCE_FULL_BACKUP_LIMIT_REACHED"), config.maxSmartBackupsPerFull);
            }
        }
    }

	BackupScanResult scanResult = BackupChangeDetector{}.Scan(sourcePath, metadataFolder, destinationFolder);
	vector<filesystem::path> candidate_files = std::move(scanResult.changedFiles);
	auto currentState = std::move(scanResult.currentState);
	auto changeSet = std::move(scanResult.changes);
    if (scanResult.status == BackupScanStatus::NoChange && config.skipIfUnchanged) {
        BACKUP_INFO(L("LOG_NO_CHANGE_FOUND"));
        return BackupOutcome::NoChanges;
    } else if (scanResult.status == BackupScanStatus::MetadataInvalid) {
        BACKUP_WARNING(L("LOG_METADATA_INVALID"));
    } else if (scanResult.status == BackupScanStatus::BaseBackupMissing && config.backupMode == 2) {
        BACKUP_WARNING(L("LOG_BASE_BACKUP_NOT_FOUND"));
    } else if (scanResult.status == BackupScanStatus::ScanFailed) {
        BACKUP_ERROR("Failed to scan source directory for backup state.");
        return BackupOutcome::Failed;
    }

    forceFullBackup = (scanResult.status == BackupScanStatus::MetadataInvalid ||
        scanResult.status == BackupScanStatus::BaseBackupMissing ||
        forceFullBackupDueToLimit) || forceFullBackup;

	if (!(config.backupMode == 2 && !forceFullBackup)) {
        try {
            candidate_files.clear();
            for (const auto& entry : filesystem::recursive_directory_iterator(sourcePath)) {
                if (entry.is_regular_file()) {
                    candidate_files.push_back(entry.path());
                }
            }
        } catch (const filesystem::filesystem_error& e) {
            BACKUP_ERROR("Failed to scan source directory %s: %s", wstring_to_utf8(sourcePath).c_str(), e.what());
            return BackupOutcome::Failed;
        }
    }

	const PathRuleSet backupRules(effectiveBlacklist);
    auto is_relative_blacklisted = [&](const wstring& relativePath) {
		filesystem::path absolutePath = filesystem::path(sourcePath) / relativePath;
		return backupRules.Matches(absolutePath, sourcePath, originalSourcePath);
	};

	for (auto it = currentState.begin(); it != currentState.end(); ) {
		if (is_relative_blacklisted(it->first)) {
			it = currentState.erase(it);
		}
		else {
			++it;
		}
	}

	auto filter_relative_changes = [&](vector<wstring>& paths) {
		paths.erase(remove_if(paths.begin(), paths.end(), [&](const wstring& relativePath) {
			return is_relative_blacklisted(relativePath);
		}), paths.end());
	};
	filter_relative_changes(changeSet.addedFiles);
	filter_relative_changes(changeSet.modifiedFiles);
	filter_relative_changes(changeSet.deletedFiles);

    vector<filesystem::path> files_to_backup;
    for (const auto& file : candidate_files) {
		if (!backupRules.Matches(file, sourcePath, originalSourcePath)) {
            files_to_backup.push_back(file);
        }
    }

	if (!forceFullBackup && !changeSet.HasChanges() && (config.skipIfUnchanged || config.backupMode == 2)) {
        BACKUP_INFO(L("LOG_NO_CHANGE_FOUND"));
        return BackupOutcome::NoChanges;
    }

	const bool deletionOnlyChange = changeSet.deletedFiles.size() > 0 && files_to_backup.empty();
	if (files_to_backup.empty() && !(config.backupMode == 2 && deletionOnlyChange && !forceFullBackup)) {
		BACKUP_INFO(L("LOG_NO_CHANGE_FOUND"));
		return BackupOutcome::NoChanges;
	}

    filesystem::path tempDir = GetAppPaths().runtimeRoot /
		(L"MineBackup_Filelist_" + FolderRewindFormat::GenerateGuidString());
	ScopedRuntimeArtifact tempDirCleanup(tempDir);
	wstring filelist_path;
	if (!files_to_backup.empty()) {
		filesystem::create_directories(tempDir);
		filelist_path = (tempDir / (L"_filelist.txt")).wstring();

		ofstream ofs{std::filesystem::path(filelist_path), ios::binary};
		if (ofs.is_open()) {
			for (const auto& file : files_to_backup) {
				string utf8Path = wstring_to_utf8(filesystem::relative(file, sourcePath).wstring());
				ofs.write(utf8Path.data(), static_cast<std::streamsize>(utf8Path.size()));
				ofs.put('\n');
			}
			ofs.close();
#ifdef _WIN32
			{
				HANDLE h = CreateFileW(filelist_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				if (h != INVALID_HANDLE_VALUE) {
					FlushFileBuffers(h);
					CloseHandle(h);
				}
			}
#endif
		} else {
			BACKUP_ERROR("Failed to create temporary file list for 7-Zip.");
			return BackupOutcome::Failed;
		}
	}

	const int normalizedZipLevel = NormalizeCompressionLevel(config.zipMethod, config.zipLevel);

    wstring backupTypeStr;
    wstring basedOnBackupFile;
    filesystem::path latestBackupPath;

	if ((config.backupMode == 1 || forceFullBackup) && config.backupMode != 3) {
		backupTypeStr = L"Full";
		archivePath = makeArchivePath(L"Full");
		auto arguments = SevenZipCreateArguments(config, normalizedZipLevel, archivePath);
		arguments.push_back(L"@" + filelist_path);
		command = MakeInternalProcess(config.zipPath, std::move(arguments), sourcePath, config.useLowPriority);
		basedOnBackupFile = filesystem::path(archivePath).filename().wstring();
    } else if (config.backupMode == 2) {
        backupTypeStr = L"Smart";

		BACKUP_INFO(L("LOG_BACKUP_SMART_INFO"), files_to_backup.size() + changeSet.deletedFiles.size());

        // 智能备份需要找到它所基于的文件；扫描器已验证这些归档仍存在。
		try {
			FolderRewindFormat::MetadataState folderRewindState;
			if (FolderRewindMetadataStore::LoadState(metadataFolder, folderRewindState)) {
				basedOnBackupFile = folderRewindState.basedOnFullBackup.empty() ? folderRewindState.lastBackupFileName : folderRewindState.basedOnFullBackup;
			}
			else {
				throw runtime_error("Cannot load FolderRewind metadata state");
			}
			if (basedOnBackupFile.empty()) {
				throw runtime_error("Metadata does not contain a base backup");
			}
		} catch (const exception& e) {
			BACKUP_WARNING("Failed to read metadata for smart backup, forcing full backup: %s", e.what());
			// 回退到完整备份
			backupTypeStr = L"Full";
			archivePath = makeArchivePath(L"Full");
			auto arguments = SevenZipCreateArguments(config, normalizedZipLevel, archivePath);
			arguments.push_back(L"@" + filelist_path);
			command = MakeInternalProcess(config.zipPath, std::move(arguments), sourcePath, config.useLowPriority);
			basedOnBackupFile = filesystem::path(archivePath).filename().wstring();
			goto execute_backup;
		}

        // 7z 支持用 @文件名 的方式批量指定要压缩的文件。把所有要备份的文件路径写到一个文本文件避免超过cmd 8191限长
		archivePath = makeArchivePath(L"Smart");

		if (!deletionOnlyChange) {
			auto arguments = SevenZipCreateArguments(config, normalizedZipLevel, archivePath);
			arguments.push_back(L"@" + filelist_path);
			command = MakeInternalProcess(config.zipPath, std::move(arguments), sourcePath, config.useLowPriority);
		}
    } else if (config.backupMode == 3) {
        backupTypeStr = L"Overwrite";
        BACKUP_INFO(L("LOG_OVERWRITE"));
        auto latest_time = filesystem::file_time_type{}; // 默认构造就是最小时间点，不需要::min()
        bool found = false;

		for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
            if (entry.is_regular_file() && entry.path().extension().wstring() == L"." + config.zipFormat) {
                if (entry.last_write_time() > latest_time) {
                    latest_time = entry.last_write_time();
                    latestBackupPath = entry.path();
                    found = true;
                }
            }
        }
        if (found) {
            BACKUP_INFO(L("LOG_FOUND_LATEST"), wstring_to_utf8(latestBackupPath.filename().wstring()).c_str());
			command = MakeInternalProcess(config.zipPath,
				{L"u", L"-ssw", latestBackupPath.wstring(), NormalizeSeparators(sourcePath) + L"/*",
				 L"-mx=" + to_wstring(normalizedZipLevel)}, sourcePath, config.useLowPriority);
            archivePath = latestBackupPath.wstring(); // 记录被更新的文件
        }
        else {
            BACKUP_INFO(L("LOG_NO_BACKUP_FOUND"));
			archivePath = makeArchivePath(L"Overwrite");
			auto arguments = SevenZipCreateArguments(config, normalizedZipLevel, archivePath);
			arguments.push_back(L"-spf");
			arguments.push_back(NormalizeSeparators(sourcePath) + L"/*");
			command = MakeInternalProcess(config.zipPath, std::move(arguments), sourcePath, config.useLowPriority);
            // -spf 强制使用完整路径，-spf2 使用相对路径
        }
    }

execute_backup:
    {
        // 在后台线程中执行命令
        bool backupSucceeded = false;
		if (backupTypeStr == L"Smart" && deletionOnlyChange) {
			backupSucceeded = CreateDeletionOnlyArchive(config, archivePath);
		}
		else {
			backupSucceeded = RunInternalProcess(command);
		}

        if (backupSucceeded)
        {
            BACKUP_INFO(L("LOG_BACKUP_END_HEADER"));

        // 备份文件大小检查 - 根据备份类型调整阈值
        try {
            if (filesystem::exists(archivePath)) {
                uintmax_t fileSize = filesystem::file_size(archivePath);
                // Full备份至少应该有100KB，Smart备份可以很小
                uintmax_t minThreshold = (backupTypeStr == L"Full") ? 102400 : 10240;
                if (fileSize < minThreshold) {
                    BACKUP_WARNING(L("BACKUP_FILE_TOO_SMALL_WARNING"), wstring_to_utf8(filesystem::path(archivePath).filename().wstring()).c_str());
                    // 广播一个警告
                    BroadcastEvent("event=backup_warning;type=file_too_small;");
                }
            }
        }
        catch (const filesystem::filesystem_error& e) {
            BACKUP_ERROR("Could not check backup file size: %s", e.what());
        }

		wstring completedBackupFile = filesystem::path(archivePath).filename().wstring();

        g_appState.realConfigIndex = -1;

        if (config.backupMode == 3) {
            if (!latestBackupPath.empty()) {
                wstring oldName = latestBackupPath.filename().wstring();
                wstring newName = FolderRewindFormat::GenerateArchiveFileName(L"Overwrite", storageFolderName, comment, config.zipFormat);
                filesystem::path newPath = latestBackupPath.parent_path() / newName;
                if (latestBackupPath != newPath) {
                    filesystem::rename(latestBackupPath, newPath);
                    latestBackupPath = newPath;
                    archivePath = latestBackupPath.wstring();
                    completedBackupFile = latestBackupPath.filename().wstring();
                    RemoveHistoryEntry(folder.configIndex, storageFolderName, oldName);
                    InvalidateBackupMetadata(config, storageFolderName, oldName, oldName, completedBackupFile);
                }
            }
            else {
                completedBackupFile = filesystem::path(archivePath).filename().wstring();
            }
        }

		if (!UpdateMetadataFiles(metadataFolder, completedBackupFile, basedOnBackupFile, backupTypeStr, currentState, changeSet)) {
			BACKUP_ERROR("Failed to write FolderRewind metadata for backup: %s", wstring_to_utf8(completedBackupFile).c_str());
			BroadcastEvent("event=backup_failed;config=" + to_string(folder.configIndex) + ";config_id=" + wstring_to_utf8(config.configId) + ";world=" + wstring_to_utf8(storageFolderName) + ";error=metadata_write_failed");
			return BackupOutcome::Failed;
		}
		AddHistoryEntry(folder.configIndex, storageFolderName, completedBackupFile, backupTypeStr, comment, folder.path);

		if (folder.configIndex != -1)
			LimitBackupFiles(config, folder.configIndex, destinationFolder.wstring(), config.keepCount);
		else
			LimitBackupFiles(config, g_appState.currentConfigIndex, destinationFolder.wstring(), config.keepCount);

		// 广播一个成功事件
		string payload = "event=backup_success;config=" + to_string(folder.configIndex) + ";config_id=" + wstring_to_utf8(config.configId) + ";world=" + wstring_to_utf8(storageFolderName) + ";file=" + wstring_to_utf8(completedBackupFile);
		BroadcastEvent(payload);


		// 云存档统一交给 CloudSyncService 处理，避免 UI 和核心逻辑各自拼接 rclone 命令。
		MyFolder cloudFolder = folder;
		cloudFolder.name = storageFolderName;
		QueueUploadAfterBackup(config, folder.configIndex, cloudFolder, completedBackupFile, comment);
		return BackupOutcome::Created;
        }
        else {
            BroadcastEvent("event=backup_failed;config=" + to_string(folder.configIndex) + ";config_id=" + wstring_to_utf8(config.configId) + ";world=" + wstring_to_utf8(folder.name) + ";error=command_failed");
			return BackupOutcome::Failed;
        }
    }
}
