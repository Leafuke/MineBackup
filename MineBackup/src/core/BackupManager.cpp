#include "ArchiveRunner.h"
#include "BackupChangeDetector.h"
#include "BackupService.h"
#include "BackupManagerInternal.h"
#include "AppPaths.h"
#include "text_to_text.h"
#include "Logging.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "ProcessRunner.h"
#include "RuntimeIntegration.h"
#include "TaskCoordinator.h"
#include "ExternalToolManager.h"
#include "PathRuleSet.h"
#include "FileName.h"
#include "json.hpp"
#include "PlatformCompat.h"
#include <filesystem>
#include <fstream>
#include <iterator>
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
	const auto result = ProcessRunner::Run(
		spec, TaskCoordinator::CurrentStopToken());
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
	if (result.exitCode == 2) BACKUP_WARNING("7-Zip rejected the generated command; verify the archive settings.");
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
	for (wchar_t& ch : s) ch = (wchar_t)towlower(ch);
}

static bool EqualsIgnoreCase(const wstring& left, const wstring& right) {
	if (left.size() != right.size()) return false;
	for (size_t index = 0; index < left.size(); ++index) {
		if (towlower(left[index]) != towlower(right[index])) return false;
	}
	return true;
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
				return EqualsIgnoreCase(item, forcedRule);
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
				if (EqualsIgnoreCase(state.lastBackupFileName, renamedOldFile)) state.lastBackupFileName = renamedNewFile;
				if (EqualsIgnoreCase(state.basedOnFullBackup, renamedOldFile)) state.basedOnFullBackup = renamedNewFile;
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

BackupService::BackupService(BackupServiceDependencies dependencies)
	: dependencies_(std::move(dependencies)) {
}

namespace {

Diagnostic MakeDiagnostic(
	string eventId,
	DiagnosticSeverity severity,
	string detail = {}) {
	return {std::move(eventId), severity, std::move(detail)};
}

BackupResult MakeBackupFailure(
	OperationCode code,
	BackupOutcome outcome,
	string eventId,
	string detail = {}) {
	BackupResult result;
	result.code = code;
	result.outcome = outcome;
	result.diagnostics.push_back(MakeDiagnostic(
		std::move(eventId),
		code == OperationCode::Cancelled ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
		std::move(detail)));
	return result;
}

} // namespace

BackupResult BackupService::Run(
	const BackupRequest& request,
	stop_token stopToken) const {
	const vector<pair<string, string>> targetFields{
		{"config", wstring_to_utf8(request.config.configId)},
		{"config_id", wstring_to_utf8(request.config.configId)},
		{"folder", wstring_to_utf8(request.world.relativePath)},
		{"world", wstring_to_utf8(request.world.relativePath)}};
	auto publish = [&](string eventId,
		vector<pair<string, string>> fields = {}) {
		if (!dependencies_.eventSink) return;
		auto merged = targetFields;
		merged.insert(merged.end(),
			make_move_iterator(fields.begin()), make_move_iterator(fields.end()));
		dependencies_.eventSink->Publish({std::move(eventId), std::move(merged)});
	};

	publish("backup_started");
	BackupResult result;
	try {
		result = RunCore(request, stopToken);
	}
	catch (const exception& error) {
		publish("backup_failed", {
			{"error", "exception"}, {"message", error.what()}});
		throw;
	}
	catch (...) {
		publish("backup_failed", {
			{"error", "unknown_exception"}});
		throw;
	}

	if (result.outcome == BackupOutcome::NoChanges) {
		// The companion mod treats this command lifecycle event as the
		// terminal signal that also releases a coordinated auto-save freeze.
		publish("command_completed", {
			{"command", "BACKUP"}, {"result", "no_changes"}});
	}
	else if (result.outcome != BackupOutcome::Created) {
		publish("backup_failed", {
			{"error", ToString(result.code)},
			{"result", ToString(result.outcome)}});
	}
	return result;
}

BackupResult BackupService::RunCore(
	const BackupRequest& request,
	stop_token stopToken) const {
	minebackup::logging::ScopedLogContext operationContext{{
		"operation_id", wstring_to_utf8(FolderRewindFormat::GenerateGuidString())},
		{"config_id", wstring_to_utf8(request.config.configId)},
		{"world", wstring_to_utf8(request.world.relativePath)}};
	const Config& config = request.config;
	const wstring worldName = request.world.relativePath;
	const wstring displayName = request.displayName.empty() ? worldName : request.displayName;
	const wstring comment = request.comment;
	auto publish = [&](string eventId, vector<pair<string, string>> fields = {}) {
		if (!dependencies_.eventSink) return;
		dependencies_.eventSink->Publish({std::move(eventId), std::move(fields)});
	};
	auto cancelled = [&]() {
		return stopToken.stop_requested();
	};
	vector<Diagnostic> deferredDiagnostics;
	if (cancelled()) {
		return MakeBackupFailure(
			OperationCode::Cancelled, BackupOutcome::Rejected,
			"backup.cancelled", "Cancellation was requested before backup started.");
	}
	if (request.world.configId.empty() || config.configId.empty()
		|| request.world.configId != config.configId
		|| worldName.empty() || request.sourcePath.empty()) {
		return MakeBackupFailure(
			OperationCode::InvalidArguments, BackupOutcome::Rejected,
			"backup.request.invalid", "The backup request does not identify a stable configuration and world.");
	}
	if (config.pendingLocalBinding) {
		BACKUP_WARNING("This imported configuration is waiting for local path binding.");
		return MakeBackupFailure(
			OperationCode::MigrationRequired, BackupOutcome::Rejected,
			"backup.profile.binding_required", "The imported profile still needs local path binding.");
	}

	WorldOperationGuard opGuard(request.sourcePath, FolderState::BACKUP);
	if (!opGuard.Acquired()) {
		BACKUP_WARNING("World operation rejected because another operation is active: %s",
			wstring_to_utf8(displayName).c_str());
		return MakeBackupFailure(
			OperationCode::ProfileBusy, BackupOutcome::Rejected,
			"backup.world.busy", wstring_to_utf8(displayName));
	}
	if (cancelled()) {
		return MakeBackupFailure(
			OperationCode::Cancelled, BackupOutcome::Rejected,
			"backup.cancelled", "Cancellation was requested before migration.");
	}
	MigrationUnitResult migration;
	if (dependencies_.ensureMigration) {
		migration = dependencies_.ensureMigration(request);
	}
	const bool forceFullForMigration = migration.status == MigrationStatus::Failed || migration.status == MigrationStatus::Degraded;
	if (migration.status == MigrationStatus::Failed) {
		BACKUP_WARNING("Legacy metadata migration failed; this backup will establish a new Full chain: %s", wstring_to_utf8(migration.message).c_str());
	}
	else if (migration.status == MigrationStatus::Degraded) {
		BACKUP_WARNING("Legacy metadata was only partially migrated; forcing a safe Full backup.");
	}

	BACKUP_INFO("Starting backup preparation for %s", wstring_to_utf8(displayName).c_str());

	const ArchiveRunner archiveRunner = dependencies_.archiveRunnerFactory
		? dependencies_.archiveRunnerFactory(config.zipPath, dependencies_.paths, stopToken)
		: ArchiveRunner::Resolve(
			config.zipPath,
			dependencies_.paths,
			stopToken,
			dependencies_.processExecutor);
	if (!archiveRunner.IsAvailable()) {
		const string detail = wstring_to_utf8(archiveRunner.Resolution().diagnostic);
		BACKUP_ERROR("No supported 7-Zip executable is available: %s", detail.c_str());
		return MakeBackupFailure(
			OperationCode::ToolUnavailable, BackupOutcome::Failed,
			"backup.tool.unavailable", detail);
    }
	struct PendingArchiveCommand {
		vector<wstring> arguments;
		filesystem::path workingDirectory;
	};
	auto makeCommand = [&](vector<wstring> arguments, filesystem::path workingDirectory) {
		return PendingArchiveCommand{std::move(arguments), std::move(workingDirectory)};
	};
	auto runCommand = [&](const PendingArchiveCommand& command) {
		return archiveRunner.Execute(
			command.arguments,
			command.workingDirectory,
			config.useLowPriority);
	};
	auto runArchiveCommand = [&](const PendingArchiveCommand& command) {
		const ProcessResult process = runCommand(command);
		if (process.status == ProcessStatus::Succeeded) return true;
		if (process.status == ProcessStatus::Cancelled) {
			BACKUP_WARNING("Backup archive process was cancelled.");
		}
		else {
			BACKUP_ERROR("Backup archive process failed with exit code %d: %s",
				process.exitCode, wstring_to_utf8(process.error).c_str());
		}
		return false;
	};

	wstring originalSourcePath = request.sourcePath.wstring();
	wstring sourcePath = NormalizeSeparators(originalSourcePath);
	const vector<wstring> effectiveBlacklist = BuildEffectiveBackupBlacklist(config.blacklist);
	FolderRewindFormat::StoragePaths storagePaths;
	if (!FolderRewindFormat::TryResolveStoragePaths(config.backupPath, worldName, request.sourcePath.wstring(), storagePaths)) {
		BACKUP_ERROR("Invalid FolderRewind storage folder name for world: %s", wstring_to_utf8(worldName).c_str());
		return MakeBackupFailure(
			OperationCode::InvalidArguments, BackupOutcome::Failed,
			"backup.target.invalid", wstring_to_utf8(worldName));
	}
	filesystem::path destinationFolder = storagePaths.backupSubDir;
	filesystem::path metadataFolder = storagePaths.metadataDir;
	const wstring storageFolderName = storagePaths.folderName;
	PendingArchiveCommand command;
	wstring archivePath;
	auto makeArchivePath = [&](const wstring& backupType) {
		return (destinationFolder / FolderRewindFormat::GenerateArchiveFileName(backupType, storageFolderName, comment, config.zipFormat)).wstring();
	};

	try {
		filesystem::create_directories(destinationFolder);
		filesystem::create_directories(metadataFolder);
		BACKUP_INFO("Backup directory: %s", wstring_to_utf8(destinationFolder.wstring()).c_str());
    } catch (const filesystem::filesystem_error& e) {
		BACKUP_ERROR("Cannot create backup directory: %s", e.what());
		return MakeBackupFailure(
			OperationCode::BackupFailed, BackupOutcome::Failed,
			"backup.directory.create_failed", e.what());
    }

	// 检测到 level.dat 被锁定，启用热备份握手并依赖 7z -ssw 直接从原世界路径压缩
	const bool sourceLocked = dependencies_.isFileLocked
		&& (dependencies_.isFileLocked(filesystem::path(sourcePath) / L"level.dat")
			|| dependencies_.isFileLocked(filesystem::path(sourcePath) / L"session.lock"));
	if (sourceLocked) {
		HotBackupPreparation preparation;
		if (dependencies_.hotBackup) {
			preparation = dependencies_.hotBackup->Prepare(request, stopToken);
		}
		if (preparation.status == HotBackupStatus::Rejected) {
			BackupResult rejected = MakeBackupFailure(
				cancelled() ? OperationCode::Cancelled : OperationCode::BackupFailed,
				BackupOutcome::Rejected,
				cancelled() ? "backup.cancelled" : "backup.hot_backup.rejected",
				"The live-world save handshake did not complete.");
			rejected.diagnostics.insert(
				rejected.diagnostics.end(), preparation.diagnostics.begin(), preparation.diagnostics.end());
			return rejected;
		}
		deferredDiagnostics.insert(
			deferredDiagnostics.end(),
			preparation.diagnostics.begin(), preparation.diagnostics.end());
		BACKUP_INFO("Using 7-Zip -ssw to back up live world files.");
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
		BACKUP_INFO("A full backup is required.");

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
			BACKUP_ERROR("Cannot scan backup directory: %s", e.what());
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
				BACKUP_INFO("Smart backup limit reached (%d); creating a full backup.", config.maxSmartBackupsPerFull);
            }
        }
    }

	BackupScanResult scanResult = BackupChangeDetector{}.Scan(sourcePath, metadataFolder, destinationFolder);
	vector<filesystem::path> candidate_files = std::move(scanResult.changedFiles);
	auto currentState = std::move(scanResult.currentState);
	auto changeSet = std::move(scanResult.changes);
    if (scanResult.status == BackupScanStatus::NoChange && config.skipIfUnchanged) {
		BACKUP_INFO("No world changes were found.");
		publish("backup.no_changes", {{"config_id", wstring_to_utf8(config.configId)}, {"world", wstring_to_utf8(worldName)}});
		BackupResult result;
		result.code = OperationCode::NoChanges;
		result.outcome = BackupOutcome::NoChanges;
		result.diagnostics.push_back(MakeDiagnostic("backup.no_changes", DiagnosticSeverity::Info));
		result.diagnostics.insert(result.diagnostics.end(),
			deferredDiagnostics.begin(), deferredDiagnostics.end());
		return result;
    } else if (scanResult.status == BackupScanStatus::MetadataInvalid) {
		publish("backup.metadata.invalid");
    } else if (scanResult.status == BackupScanStatus::BaseBackupMissing && config.backupMode == 2) {
		BACKUP_WARNING("The smart-backup base archive is missing; creating a full backup.");
    } else if (scanResult.status == BackupScanStatus::ScanFailed) {
        BACKUP_ERROR("Failed to scan source directory for backup state.");
		return MakeBackupFailure(
			OperationCode::BackupFailed, BackupOutcome::Failed,
			"backup.scan.failed", "Failed to scan source directory for backup state.");
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
			return MakeBackupFailure(
				OperationCode::BackupFailed, BackupOutcome::Failed,
				"backup.scan.failed", e.what());
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
		BACKUP_INFO("No world changes were found.");
		publish("backup.no_changes", {{"config_id", wstring_to_utf8(config.configId)}, {"world", wstring_to_utf8(worldName)}});
		BackupResult result;
		result.code = OperationCode::NoChanges;
		result.outcome = BackupOutcome::NoChanges;
		result.diagnostics.push_back(MakeDiagnostic("backup.no_changes", DiagnosticSeverity::Info));
		result.diagnostics.insert(result.diagnostics.end(),
			deferredDiagnostics.begin(), deferredDiagnostics.end());
		return result;
	}

	const bool deletionOnlyChange = changeSet.deletedFiles.size() > 0 && files_to_backup.empty();
	if (files_to_backup.empty() && !(config.backupMode == 2 && deletionOnlyChange && !forceFullBackup)) {
		BACKUP_INFO("No world changes were found.");
		publish("backup.no_changes", {{"config_id", wstring_to_utf8(config.configId)}, {"world", wstring_to_utf8(worldName)}});
		BackupResult result;
		result.code = OperationCode::NoChanges;
		result.outcome = BackupOutcome::NoChanges;
		result.diagnostics.push_back(MakeDiagnostic("backup.no_changes", DiagnosticSeverity::Info));
		result.diagnostics.insert(result.diagnostics.end(),
			deferredDiagnostics.begin(), deferredDiagnostics.end());
		return result;
	}

    filesystem::path tempDir = dependencies_.paths.runtimeRoot /
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
		} else {
			BACKUP_ERROR("Failed to create temporary file list for 7-Zip.");
			return MakeBackupFailure(
				OperationCode::BackupFailed, BackupOutcome::Failed,
				"backup.file_list.create_failed", "Failed to create the temporary 7-Zip file list.");
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
		command = makeCommand(std::move(arguments), sourcePath);
		basedOnBackupFile = filesystem::path(archivePath).filename().wstring();
    } else if (config.backupMode == 2) {
        backupTypeStr = L"Smart";

		BACKUP_INFO("Creating smart backup with %zu changed paths.", files_to_backup.size() + changeSet.deletedFiles.size());

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
			command = makeCommand(std::move(arguments), sourcePath);
			basedOnBackupFile = filesystem::path(archivePath).filename().wstring();
			goto execute_backup;
		}

        // 7z 支持用 @文件名 的方式批量指定要压缩的文件。把所有要备份的文件路径写到一个文本文件避免超过cmd 8191限长
		archivePath = makeArchivePath(L"Smart");

		if (!deletionOnlyChange) {
			auto arguments = SevenZipCreateArguments(config, normalizedZipLevel, archivePath);
			arguments.push_back(L"@" + filelist_path);
			command = makeCommand(std::move(arguments), sourcePath);
		}
    } else if (config.backupMode == 3) {
        backupTypeStr = L"Overwrite";
		BACKUP_INFO("Updating the most recent overwrite backup.");
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
			BACKUP_INFO("Found latest overwrite archive: %s", wstring_to_utf8(latestBackupPath.filename().wstring()).c_str());
			command = makeCommand(
				{L"u", L"-ssw", latestBackupPath.wstring(), NormalizeSeparators(sourcePath) + L"/*",
				 L"-mx=" + to_wstring(normalizedZipLevel)}, sourcePath);
            archivePath = latestBackupPath.wstring(); // 记录被更新的文件
        }
        else {
			BACKUP_INFO("No overwrite archive exists; creating one.");
			archivePath = makeArchivePath(L"Overwrite");
			auto arguments = SevenZipCreateArguments(config, normalizedZipLevel, archivePath);
			arguments.push_back(L"-spf");
			arguments.push_back(NormalizeSeparators(sourcePath) + L"/*");
			command = makeCommand(std::move(arguments), sourcePath);
            // -spf 强制使用完整路径，-spf2 使用相对路径
        }
    }

execute_backup:
    {
		auto createDeletionOnlyArchive = [&]() {
			const filesystem::path tempDir = dependencies_.paths.runtimeRoot /
				(L"MineBackup_DeleteOnly_" + FolderRewindFormat::GenerateGuidString());
			ScopedRuntimeArtifact cleanup(tempDir);
			try {
				const filesystem::path internalDir = tempDir / FolderRewindFormat::kInternalRestoreMarkerDirectoryName;
				filesystem::create_directories(internalDir);
				ofstream marker(
					internalDir / FolderRewindFormat::kInternalRestoreMarkerFileName,
					ios::binary | ios::trunc);
				marker << wstring_to_utf8(FolderRewindFormat::MakeUtcTimestampString());
				marker.close();
				auto arguments = SevenZipCreateArguments(
					config, normalizedZipLevel, filesystem::path(archivePath));
				arguments.push_back(L"*");
				return runArchiveCommand(makeCommand(std::move(arguments), tempDir));
			}
			catch (const exception& error) {
				BACKUP_ERROR("Failed to create deletion-only archive: %s", error.what());
				return false;
			}
		};

        bool backupSucceeded = false;
		if (backupTypeStr == L"Smart" && deletionOnlyChange) {
			backupSucceeded = createDeletionOnlyArchive();
		}
		else {
			backupSucceeded = runArchiveCommand(command);
		}

        if (backupSucceeded)
        {
			BACKUP_INFO("Backup archive completed.");

        // 备份文件大小检查 - 根据备份类型调整阈值
        try {
            if (filesystem::exists(archivePath)) {
                uintmax_t fileSize = filesystem::file_size(archivePath);
                // Full备份至少应该有100KB，Smart备份可以很小
                uintmax_t minThreshold = (backupTypeStr == L"Full") ? 102400 : 10240;
                if (fileSize < minThreshold) {
					BACKUP_WARNING("The backup archive is unexpectedly small: %s",
						wstring_to_utf8(filesystem::path(archivePath).filename().wstring()).c_str());
					publish("backup_warning", {
						{"config", wstring_to_utf8(config.configId)},
						{"config_id", wstring_to_utf8(config.configId)},
						{"folder", wstring_to_utf8(worldName)},
						{"world", wstring_to_utf8(worldName)},
						{"file", wstring_to_utf8(filesystem::path(archivePath).filename().wstring())},
						{"type", "file_too_small"}});
                }
            }
        }
        catch (const filesystem::filesystem_error& e) {
            BACKUP_ERROR("Could not check backup file size: %s", e.what());
        }

		wstring completedBackupFile = filesystem::path(archivePath).filename().wstring();

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
					if (dependencies_.removeHistory) {
						(void)dependencies_.removeHistory(storageFolderName, oldName);
					}
                    InvalidateBackupMetadata(config, storageFolderName, oldName, oldName, completedBackupFile);
                }
            }
            else {
                completedBackupFile = filesystem::path(archivePath).filename().wstring();
            }
        }

		if (!UpdateMetadataFiles(metadataFolder, completedBackupFile, basedOnBackupFile, backupTypeStr, currentState, changeSet)) {
			BACKUP_ERROR("Failed to write FolderRewind metadata for backup: %s", wstring_to_utf8(completedBackupFile).c_str());
			publish("backup.failed", {{"error", "metadata_write_failed"}});
			return MakeBackupFailure(
				OperationCode::BackupFailed, BackupOutcome::Failed,
				"backup.metadata.write_failed", wstring_to_utf8(completedBackupFile));
		}

		HistoryEntry historyEntry;
		historyEntry.configId = config.configId;
		historyEntry.timestamp_str = FolderRewindFormat::MakeLocalHistoryTimestampString();
		historyEntry.worldPath = request.sourcePath.wstring();
		historyEntry.worldName = storageFolderName;
		historyEntry.backupFile = completedBackupFile;
		historyEntry.backupType = backupTypeStr;
		historyEntry.isPartialBackup = FolderRewindFormat::IsSmartBackupType(backupTypeStr);
		historyEntry.comment = comment;
		if (!dependencies_.addHistory || !dependencies_.addHistory(historyEntry)) {
			publish("backup.failed", {{"error", "history_commit_failed"}});
			return MakeBackupFailure(
				OperationCode::BackupFailed, BackupOutcome::Failed,
				"backup.history.commit_failed", wstring_to_utf8(completedBackupFile));
		}
		if (dependencies_.enforceRetention) {
			dependencies_.enforceRetention(request, historyEntry);
		}

		publish("backup.completed", {
			{"config_id", wstring_to_utf8(config.configId)},
			{"world", wstring_to_utf8(storageFolderName)},
			{"file", wstring_to_utf8(completedBackupFile)}});
		// This is the companion-mod terminal event. Publish it immediately
		// after the local archive and history commit so auto-save resumes before
		// potentially slow rclone post-processing begins.
		publish("backup_success", {
			{"config", wstring_to_utf8(config.configId)},
			{"config_id", wstring_to_utf8(config.configId)},
			{"folder", wstring_to_utf8(storageFolderName)},
			{"world", wstring_to_utf8(storageFolderName)},
			{"file", wstring_to_utf8(completedBackupFile)},
			{"result", "created"}});

		BackupResult result;
		result.code = OperationCode::Success;
		result.outcome = BackupOutcome::Created;
		result.archivePath = filesystem::path(archivePath);
		result.historyEntry = historyEntry;
		result.diagnostics.push_back(MakeDiagnostic("backup.completed", DiagnosticSeverity::Info));
		result.diagnostics.insert(result.diagnostics.end(),
			deferredDiagnostics.begin(), deferredDiagnostics.end());
		if (dependencies_.cloudPost && !cancelled()) {
			result.cloud = dependencies_.cloudPost->Run(request, historyEntry, stopToken);
			result.diagnostics.insert(
				result.diagnostics.end(),
				result.cloud.diagnostics.begin(),
				result.cloud.diagnostics.end());
			if (result.cloud.status == CloudPostStatus::Failed) {
				result.code = OperationCode::PartialSuccess;
			}
		}
		if (cancelled() && result.cloud.status != CloudPostStatus::Failed) {
			result.code = OperationCode::Cancelled;
			result.diagnostics.push_back(MakeDiagnostic(
				"backup.cancelled", DiagnosticSeverity::Warning,
				"Cancellation was requested after the local backup committed."));
		}
		return result;
        }
        else {
			publish("backup.failed", {
				{"config_id", wstring_to_utf8(config.configId)},
				{"world", wstring_to_utf8(worldName)},
				{"error", cancelled() ? "cancelled" : "command_failed"}});
			return MakeBackupFailure(
				cancelled() ? OperationCode::Cancelled : OperationCode::BackupFailed,
				BackupOutcome::Failed,
				cancelled() ? "backup.cancelled" : "backup.command.failed");
        }
    }
}
