#include "ProfileTransaction.h"
#include "PathIdentity.h"
#include "CompressionPolicy.h"
#include "ArchiveRunner.h"
#include "BackupChangeDetector.h"
#include "BackupService.h"
#include "HistoryRepository.h"
#include "RestoreService.h"
#include "RestoreWorkspace.h"
#include <array>
#include "BackupSelection.h"
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

int ResolveDesktopConfigIndex(int requestedConfigIndex, int currentConfigIndex) {
	return requestedConfigIndex == -1
		? currentConfigIndex
		: requestedConfigIndex;
}

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





static mutex g_worldOpMutex;
static unordered_map<wstring, FolderState> g_worldOpInProgress;

WorldOperationGuard::WorldOperationGuard(const filesystem::path& worldPath, FolderState requested)
	: key_(PathIdentity::BuildPathIdentityKey(worldPath)), requested_(requested) {
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
		L"__FolderRewind_Internal",
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

	FolderRewindMetadataStore::SaveTransactionResult UpdateMetadataFiles(const filesystem::path& metadataDir, const wstring& currentBackupFile, const wstring& baseBackupFile, const wstring& previousLastBackupFile, const wstring& backupType, map<wstring, FolderRewindFormat::FileState> currentState, const BackupChangeSet& changeSet, bool independentPartial) {
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

		if (!FolderRewindFormat::IsSmartBackupType(backupType)) {
			record.fullFileList.reserve(currentState.size());
			for (const auto& pair : currentState) {
				record.fullFileList.push_back(FolderRewindFormat::NormalizeRelativePath(pair.first));
			}
			sort(record.fullFileList.begin(), record.fullFileList.end());
			record.previousBackupFileName.clear();
			record.basedOnFullBackup = currentBackupFile;
			record.addedFiles = record.fullFileList;
			record.modifiedFiles.clear();
			record.deletedFiles.clear();
		}
		if (independentPartial) {
			// An inclusion snapshot is not a checkpoint for the ordinary world chain.
			// Publish its record independently and leave state.json byte-for-byte alone.
			record.basedOnFullBackup.clear();
			const auto saved = FolderRewindMetadataStore::SaveRecordDetailed(metadataDir, record);
			using State = FolderRewindMetadataStore::SaveTransactionState;
			return {saved.IsDurable() ? State::CommittedDurably : saved.WasCommitted()
				? State::CommittedNotDurable : State::NotCommitted, saved.error};
		}

		FolderRewindFormat::MetadataState state;
		state.version = L"3.0";
		state.lastBackupTime = FolderRewindFormat::MakeLocalHistoryTimestampString();
		state.lastBackupFileName = currentBackupFile;
		state.basedOnFullBackup = record.basedOnFullBackup;
		state.fileStates = std::move(currentState);

		return FolderRewindMetadataStore::SaveDetailed(metadataDir, state, record);
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
		const auto rootStatus = filesystem::symlink_status(dir, ec);
		if (ec || !filesystem::exists(rootStatus) || filesystem::is_symlink(rootStatus)) return;
		filesystem::recursive_directory_iterator iterator(dir, filesystem::directory_options::skip_permission_denied, ec);
		const filesystem::recursive_directory_iterator end;
		while (!ec && iterator != end) {
			const auto status = iterator->symlink_status(ec);
			if (ec) break;
			if (!filesystem::is_symlink(status)
				&& (!filesystem::is_regular_file(status) || filesystem::hard_link_count(iterator->path(), ec) <= 1)) {
				filesystem::permissions(iterator->path(), filesystem::perms::owner_all,
					filesystem::perm_options::add | filesystem::perm_options::nofollow, ec);
			}
			ec.clear();
			iterator.increment(ec);
		}
		filesystem::permissions(dir, filesystem::perms::owner_all,
			filesystem::perm_options::add | filesystem::perm_options::nofollow, ec);
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

			const int normalizedZipLevel = CompressionPolicy::NormalizeLevel(config.zipMethod, config.zipLevel);
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
	if (!dependencies_.deleteMetadataRecord) {
		dependencies_.deleteMetadataRecord = FolderRewindMetadataStore::DeleteRecord;
	}
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


// A timestamp/size match is only a hint. Protected reuse verifies a complete
// materialized chain against every included source byte and checks the file set
// again afterward. Only engine lock/internal exclusions are permitted.
bool MatchesCompleteSource(const filesystem::path& source, const filesystem::path& staged,
    const map<wstring, FolderRewindFormat::FileState>& expected,
    const function<bool(const filesystem::path&)>& internalExclude,
    string& error, stop_token token) {
    if (!RestoreWorkspace::ValidateSafeTree(source, error, token)
        || !RestoreWorkspace::ValidateSafeTree(staged, error, token)) return false;
    const auto actual = BackupChangeDetector{}.Scan(staged, staged / L".absent-metadata",
        staged / L".absent-backups");
    if (actual.status == BackupScanStatus::ScanFailed || actual.currentState.size() != expected.size()) {
        error = "Archive coverage does not match the complete source."; return false;
    }
    array<char, 64 * 1024> left{}, right{};
    for (const auto& [path, state] : expected) {
        if (token.stop_requested()) { error = "Snapshot verification cancelled."; return false; }
        const auto found = actual.currentState.find(path);
        if (found == actual.currentState.end() || found->second.size != state.size) {
            error = "Archive is missing or has a different file: " + wstring_to_utf8(path); return false;
        }
        ifstream a(source / path, ios::binary), b(staged / path, ios::binary);
        if (!a || !b) { error = "Could not read snapshot files."; return false; }
        while (a || b) {
            if (token.stop_requested()) { error = "Snapshot verification cancelled."; return false; }
            a.read(left.data(), left.size()); b.read(right.data(), right.size());
            if (a.bad() || b.bad() || a.gcount() != b.gcount()
                || !equal(left.begin(), left.begin() + a.gcount(), right.begin())) {
                error = "Archive content differs from source: " + wstring_to_utf8(path); return false;
            }
        }
    }
    const auto after = BackupChangeDetector{}.Scan(source, staged / L".absent-metadata",
        staged / L".absent-backups", internalExclude);
    if (after.status == BackupScanStatus::ScanFailed || after.currentState.size() != expected.size()) {
        error = "Source changed during snapshot verification."; return false;
    }
    for (const auto& [path, state] : expected) {
        const auto found = after.currentState.find(path);
        if (found == after.currentState.end() || found->second.size != state.size
            || found->second.lastWriteTimeUtc != state.lastWriteTimeUtc) {
            error = "Source changed during snapshot verification: " + wstring_to_utf8(path); return false;
        }
    }
    return true;
}

} // namespace

BackupResult BackupService::Run(
	const BackupRequest& request,
	stop_token stopToken,
	BackupExecutionOptions options) const {
    unique_lock operation(HistoryRepository::ArchiveMutationMutex(), try_to_lock);
    if (!operation.owns_lock()) return MakeBackupFailure(OperationCode::ProfileBusy,
        BackupOutcome::Rejected, "backup.archive.busy");
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
		result = RunCore(request, stopToken, options);
	}
	catch (const exception& error) {
		if (!request.protect) publish("backup_failed", {
			{"error", "exception"}, {"message", error.what()}});
		throw;
	}
	catch (...) {
		if (!request.protect) publish("backup_failed", {
			{"error", "unknown_exception"}});
		throw;
	}

	if (result.outcome == BackupOutcome::NoChanges && !request.protect) {
		// The companion mod treats this command lifecycle event as the
		// terminal signal that also releases a coordinated auto-save freeze.
		publish("command_completed", {
			{"command", "BACKUP"}, {"result", "no_changes"}});
	}
	else if (result.outcome != BackupOutcome::Created && result.outcome != BackupOutcome::NoChanges) {
		if (!request.protect) publish("backup_failed", {
			{"error", ToString(result.code)},
			{"result", ToString(result.outcome)}});
	}
	return result;
}

BackupResult BackupService::RunCore(
	const BackupRequest& request,
	stop_token stopToken,
	BackupExecutionOptions options) const {
	minebackup::logging::ScopedLogContext operationContext{{
		"operation_id", wstring_to_utf8(FolderRewindFormat::GenerateGuidString())},
		{"config_id", wstring_to_utf8(request.config.configId)},
		{"world", wstring_to_utf8(request.world.relativePath)}};
    BackupResult unavailable;
    if (!ProfileTransaction::Inspect(dependencies_.paths.ConfigFile(), unavailable.diagnostics)) {
        unavailable.code = OperationCode::InvalidProfile;
        unavailable.outcome = BackupOutcome::Rejected;
        return unavailable;
    }
	Config config = request.config;
	if (request.auxiliarySource) { config.backupMode = 1; config.skipIfUnchanged = false; }
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
	BackupSelection selection;
	string selectionError;
	if (!BackupSelection::TryBuild(request, selection, selectionError)) {
		return MakeBackupFailure(OperationCode::InvalidArguments, BackupOutcome::Rejected,
			"backup.selection.invalid", selectionError);
	}
    auto historyWritable = [&] { return !dependencies_.canPersistHistory || dependencies_.canPersistHistory(); };
    if (request.protect && !historyWritable())
        return MakeBackupFailure(OperationCode::MigrationRequired, BackupOutcome::Rejected,
            "backup.protection.history_blocked", "History migration must finish before protected backup.");
	const bool independentPartial = selection.IsPartial();
    if (request.protect && (independentPartial || request.auxiliarySource || !config.blacklist.empty()
            || !dependencies_.history || dependencies_.historyConfigs.empty()
            || none_of(config.worlds.begin(), config.worlds.end(), [&](const auto& world) { return world.first == worldName; })
            || !PathIdentity::PathsEqual(request.sourcePath, filesystem::path(config.saveRoot) / worldName))) {
        return MakeBackupFailure(OperationCode::InvalidArguments, BackupOutcome::Rejected,
            "backup.protection.incomplete_source",
            "Protected backup requires a complete configured source without user filters and persistent history.");
    }
	if (independentPartial) {
		// One-shot snapshots always capture their complete selected set, including
		// unchanged files; they do not inherit Smart or Overwrite semantics.
		config.backupMode = 1;
		config.skipIfUnchanged = false;
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
	const vector<wstring> effectiveBlacklist = request.auxiliarySource && !independentPartial
		? vector<wstring>{} : BuildEffectiveBackupBlacklist(selection.HasWhitelist() ? vector<wstring>{} : config.blacklist);
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
	filesystem::path finalArchivePath;
	const auto stagingRoot = destinationFolder.parent_path() / (L"MineBackup_Create_" + FolderRewindFormat::GenerateGuidString());
	ScopedRuntimeArtifact stagingCleanup(stagingRoot);
	auto makeArchivePath = [&](const wstring& backupType) {
		do { finalArchivePath = destinationFolder / FolderRewindFormat::GenerateArchiveFileName(backupType, storageFolderName, comment, config.zipFormat); } while (filesystem::exists(finalArchivePath));
		filesystem::create_directories(stagingRoot);
		return (stagingRoot / finalArchivePath.filename()).wstring();
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
		if (preparation.status == HotBackupStatus::Rejected
            || (request.protect && preparation.status != HotBackupStatus::Coordinated)) {
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
	if (independentPartial) forceFullBackup = true;
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

	const PathRuleSet backupRules(effectiveBlacklist);
	BackupScanResult scanResult = BackupChangeDetector{}.Scan(sourcePath, metadataFolder, destinationFolder,
		[&](const filesystem::path& relativePath) {
			return backupRules.Matches(filesystem::path(sourcePath) / relativePath, sourcePath, originalSourcePath);
		});
	vector<filesystem::path> candidate_files = std::move(scanResult.changedFiles);
	auto currentState = std::move(scanResult.currentState);
	auto changeSet = std::move(scanResult.changes);
	const wstring previousLastBackupFile = std::move(scanResult.previousLastBackupFileName);
	const wstring previousBasedOnFullBackup = std::move(scanResult.previousBasedOnFullBackup);

    auto reuseProtected = [&]() -> BackupResult {
        auto reject = [&](string error) {
            return MakeBackupFailure(cancelled() ? OperationCode::Cancelled : OperationCode::VerificationFailed,
                BackupOutcome::Failed, "backup.protection.reuse_failed", std::move(error));
        };
        if (previousLastBackupFile.empty()) return reject("No exact unchanged backup is available.");
        const auto existing = dependencies_.history->QueryImportance(config, worldName, previousLastBackupFile);
        if (!existing.success || !existing.entry) return reject(existing.error);
        if (FolderRewindFormat::IsPartialBackupType(existing.entry->backupFile)
            || FolderRewindFormat::IsPartialBackupType(existing.entry->backupType))
            return reject("Partial backups cannot be reused as a protected source.");
        RestoreRequest restore;
        restore.config = config; restore.world = request.world; restore.archive = previousLastBackupFile;
        RestoreServiceDependencies restoreDependencies;
        restoreDependencies.paths = dependencies_.paths;
        restoreDependencies.archiveRunnerFactory = [&archiveRunner](const auto&, const auto&, auto) { return archiveRunner; };
        string error;
        const auto staged = stagingRoot / L"verify-reuse";
        if (!RestoreService(std::move(restoreDependencies)).StageVerifiedSnapshot(restore, staged, error, stopToken)
            || !MatchesCompleteSource(request.sourcePath, staged, currentState,
                [&](const filesystem::path& relative) { return backupRules.Matches(
                    request.sourcePath / relative, sourcePath, originalSourcePath); }, error, stopToken)) return reject(error);
        if (cancelled()) return reject("Cancelled before protection commit.");
        if (!historyWritable()) return MakeBackupFailure(OperationCode::MigrationRequired, BackupOutcome::Failed,
            "backup.protection.history_blocked", "History migration blocks the protection commit.");
        const auto pinned = dependencies_.history->SetImportance(config, worldName, previousLastBackupFile,
            true, dependencies_.paths.HistoryFile(), dependencies_.historyConfigs);
        if (!pinned.success || !pinned.entry) return reject(pinned.error);
        BackupResult result;
        result.code = OperationCode::NoChanges; result.outcome = BackupOutcome::NoChanges;
        result.archivePath = destinationFolder / pinned.file; result.historyEntry = pinned.entry;
        result.diagnostics.push_back(MakeDiagnostic("backup.protection.reused", DiagnosticSeverity::Info));
        if (options.onProtectedCommit) options.onProtectedCommit(result);
        return result;
    };

    if (scanResult.status == BackupScanStatus::NoChange && (config.skipIfUnchanged || request.protect)) {
        if (request.protect) return reuseProtected();
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
			"backup.scan.failed", scanResult.failureDetail.empty()
				? "Failed to scan source directory for backup state." : scanResult.failureDetail);
    }

    forceFullBackup = (scanResult.status == BackupScanStatus::MetadataInvalid ||
        scanResult.status == BackupScanStatus::BaseBackupMissing ||
        forceFullBackupDueToLimit) || forceFullBackup;

    auto is_relative_blacklisted = [&](const wstring& relativePath) {
		filesystem::path absolutePath = filesystem::path(sourcePath) / relativePath;
		return !selection.Includes(relativePath) || backupRules.Matches(absolutePath, sourcePath, originalSourcePath);
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
	if (config.backupMode == 2 && !forceFullBackup) {
		files_to_backup.reserve(candidate_files.size());
		for (const auto& file : candidate_files) {
			if (!backupRules.Matches(file, sourcePath, originalSourcePath)) {
				files_to_backup.push_back(file);
			}
		}
	}
	else {
		files_to_backup.reserve(currentState.size());
		for (const auto& [relativePath, ignored] : currentState) {
			files_to_backup.push_back(sourcePath / filesystem::path(relativePath));
		}
	}

	if (!forceFullBackup && !changeSet.HasChanges() && (config.skipIfUnchanged || config.backupMode == 2)) {
        if (request.protect) return reuseProtected();
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
	if (files_to_backup.empty() && !(config.backupMode == 3 || (config.backupMode == 2 && deletionOnlyChange && !forceFullBackup))) {
        if (request.protect) return MakeBackupFailure(OperationCode::VerificationFailed, BackupOutcome::Failed,
            "backup.protection.empty_source", "A protected source must contain at least one included file.");
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

	const int normalizedZipLevel = CompressionPolicy::NormalizeLevel(config.zipMethod, config.zipLevel);

    wstring backupTypeStr;
    wstring basedOnBackupFile;
    filesystem::path latestBackupPath;

	if ((config.backupMode == 1 || forceFullBackup) && config.backupMode != 3) {
		backupTypeStr = independentPartial ? L"Partial" : L"Full";
		archivePath = makeArchivePath(backupTypeStr);
		auto arguments = SevenZipCreateArguments(config, normalizedZipLevel, archivePath);
		arguments.push_back(L"@" + filelist_path);
		command = makeCommand(std::move(arguments), sourcePath);
		basedOnBackupFile = filesystem::path(archivePath).filename().wstring();
    } else if (config.backupMode == 2) {
        backupTypeStr = L"Smart";

		BACKUP_INFO("Creating smart backup with %zu changed paths.", files_to_backup.size() + changeSet.deletedFiles.size());

        // 智能备份需要找到它所基于的文件；扫描器已验证这些归档仍存在。
		try {
			basedOnBackupFile = previousBasedOnFullBackup.empty()
				? previousLastBackupFile
				: previousBasedOnFullBackup;
			if (basedOnBackupFile.empty()) {
				throw runtime_error("Metadata does not contain a base backup");
			}
		} catch (const exception& e) {
			BACKUP_WARNING("Failed to read metadata for smart backup, forcing full backup: %s", e.what());
            if (request.protect) return MakeBackupFailure(OperationCode::VerificationFailed,
                BackupOutcome::Failed, "backup.protection.chain_invalid", e.what());
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
  backupTypeStr=L"Overwrite"; archivePath=makeArchivePath(L"Overwrite");
  basedOnBackupFile=filesystem::path(archivePath).filename().wstring();
  auto arguments=SevenZipCreateArguments(config,normalizedZipLevel,archivePath);
  arguments.push_back(L"@"+filelist_path); command=makeCommand(std::move(arguments),sourcePath);
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
		if ((backupTypeStr == L"Smart" && deletionOnlyChange) || (config.backupMode == 3 && files_to_backup.empty())) {
			backupSucceeded = createDeletionOnlyArchive();
		}
		else {
			backupSucceeded = runArchiveCommand(command);
		}

        if (backupSucceeded)
        {
			BACKUP_INFO("Backup archive completed.");

			const filesystem::path createdArchivePath(archivePath);
			error_code archiveError;
			if (!filesystem::is_regular_file(createdArchivePath, archiveError) || archiveError) {
				const string detail = archiveError
					? archiveError.message()
					: "The archive was not created as a regular file.";
				BACKUP_ERROR("Backup archive is missing after a successful 7-Zip command: %s",
					wstring_to_utf8(createdArchivePath.filename().wstring()).c_str());
				publish("backup.failed", {{"error", "archive_missing"}});
				return MakeBackupFailure(
					OperationCode::BackupFailed, BackupOutcome::Failed,
					"backup.archive.missing", detail);
			}
			const uintmax_t archiveSize = filesystem::file_size(createdArchivePath, archiveError);
			if (archiveError) {
				BACKUP_ERROR("Could not read backup archive size: %s", archiveError.message().c_str());
				publish("backup.failed", {{"error", "archive_stat_failed"}});
				return MakeBackupFailure(
					OperationCode::BackupFailed, BackupOutcome::Failed,
					"backup.archive.stat_failed", archiveError.message());
			}
			if (archiveSize == 0) {
				BACKUP_ERROR("Backup archive is empty after a successful 7-Zip command: %s",
					wstring_to_utf8(createdArchivePath.filename().wstring()).c_str());
				publish("backup.failed", {{"error", "archive_empty"}});
				return MakeBackupFailure(
					OperationCode::BackupFailed, BackupOutcome::Failed,
					"backup.archive.empty",
					wstring_to_utf8(createdArchivePath.filename().wstring()));
			}


   {
    if (archiveRunner.Execute({L"t", archivePath}, {}, config.useLowPriority).status != ProcessStatus::Succeeded)
     return MakeBackupFailure(cancelled() ? OperationCode::Cancelled : OperationCode::BackupFailed,
         BackupOutcome::Failed, cancelled() ? "backup.cancelled" : "backup.archive.verify_failed");

    if (request.protect) {
        string error;
        auto reject = [&]() {
            return MakeBackupFailure(cancelled() ? OperationCode::Cancelled : OperationCode::VerificationFailed,
                BackupOutcome::Failed, "backup.protection.verify_failed", error);
        };
        const auto staged = stagingRoot / L"verify-created";
        if (backupTypeStr == L"Smart") {
            RestoreRequest previous;
            previous.config = config; previous.world = request.world; previous.archive = previousLastBackupFile;
            RestoreServiceDependencies restoreDependencies;
            restoreDependencies.paths = dependencies_.paths;
            restoreDependencies.archiveRunnerFactory = [&archiveRunner](const auto&, const auto&, auto) { return archiveRunner; };
            if (!RestoreService(std::move(restoreDependencies)).StageVerifiedSnapshot(previous, staged, error, stopToken))
                return reject();
            for (const auto& deleted : changeSet.deletedFiles) {
                error_code ec;
                filesystem::remove(staged / deleted, ec);
                if (ec) { error = ec.message(); return reject(); }
            }
        }
        if (!archiveRunner.ValidateMembers(archivePath, error, config.useLowPriority)
            || archiveRunner.Execute({L"x", archivePath, L"-o" + staged.wstring(), L"-y"},
                {}, config.useLowPriority).status != ProcessStatus::Succeeded) return reject();
        if (!RestoreWorkspace::ValidateSafeTree(staged, error, stopToken)) return reject();
        error_code ec;
        filesystem::remove_all(staged / FolderRewindFormat::kInternalRestoreMarkerDirectoryName, ec);
        if (ec) { error = ec.message(); return reject(); }
        if (!MatchesCompleteSource(request.sourcePath, staged, currentState,
                [&](const filesystem::path& relative) { return backupRules.Matches(
                    request.sourcePath / relative, sourcePath, originalSourcePath); }, error, stopToken)) return reject();
        if (cancelled()) return MakeBackupFailure(OperationCode::Cancelled, BackupOutcome::Failed, "backup.cancelled");
    }

    while (filesystem::exists(finalArchivePath)) {
     finalArchivePath = destinationFolder / FolderRewindFormat::GenerateArchiveFileName(
      backupTypeStr, storageFolderName, comment, config.zipFormat);
    }
    if (backupTypeStr != L"Smart") basedOnBackupFile = finalArchivePath.filename().wstring();
    filesystem::rename(archivePath, finalArchivePath);
    archivePath = finalArchivePath.wstring();
   }
   if (backupTypeStr == L"Smart" && previousLastBackupFile == filesystem::path(archivePath).filename().wstring())
    return MakeBackupFailure(OperationCode::BackupFailed, BackupOutcome::Failed, "backup.chain.self_reference");
		wstring completedBackupFile = filesystem::path(archivePath).filename().wstring();


  if (request.protect && !historyWritable()) {
      error_code cleanupError;
      filesystem::remove(filesystem::path(archivePath), cleanupError);
      return MakeBackupFailure(OperationCode::MigrationRequired, BackupOutcome::Failed,
          "backup.protection.history_blocked", "History migration blocks the protection commit.");
  }
  const auto metadataRecovery=stagingRoot/L"metadata-recovery";
  filesystem::create_directories(metadataRecovery);
  const auto statePath=FolderRewindMetadataStore::GetStatePath(metadataFolder);
  const bool stateExisted=filesystem::exists(statePath);
  error_code snapshotError;
  if(stateExisted && !independentPartial) filesystem::copy_file(statePath,metadataRecovery/L"state.json",filesystem::copy_options::overwrite_existing,snapshotError);
  if(snapshotError) return MakeBackupFailure(OperationCode::BackupFailed,BackupOutcome::Failed,"backup.metadata.snapshot_failed",snapshotError.message());

		const auto metadataUpdate = UpdateMetadataFiles(
			metadataFolder,
			completedBackupFile,
			basedOnBackupFile,
			previousLastBackupFile,
			backupTypeStr,
			std::move(currentState),
			changeSet,
			independentPartial);
		if (!metadataUpdate.IsCommitted()) {
			BACKUP_ERROR("Failed to write FolderRewind metadata for backup: %s", wstring_to_utf8(completedBackupFile).c_str());
			const bool archiveWasNewlyCreated = config.backupMode != 3 || latestBackupPath.empty();
			error_code cleanupError;
			if (archiveWasNewlyCreated) filesystem::remove(filesystem::path(archivePath), cleanupError);
			publish("backup.failed", {{"error", "metadata_write_failed"}});
			BackupResult failure = MakeBackupFailure(
				OperationCode::BackupFailed, BackupOutcome::Failed,
				"backup.metadata.write_failed",
				metadataUpdate.error.empty()
					? wstring_to_utf8(completedBackupFile)
					: wstring_to_utf8(metadataUpdate.error));
			if (cleanupError) {
				failure.diagnostics.push_back(MakeDiagnostic(
					"backup.archive.cleanup_failed",
					DiagnosticSeverity::Warning,
					cleanupError.message()));
			}
			return failure;
		}
		const bool metadataDurabilityWarning = !metadataUpdate.IsDurable();
		if (metadataDurabilityWarning) {
			BACKUP_WARNING("FolderRewind metadata was committed but directory durability could not be confirmed: %s",
				wstring_to_utf8(metadataUpdate.error).c_str());
			publish("backup.metadata.committed_not_durable");
		}

		HistoryEntry historyEntry;
		historyEntry.configId = config.configId;
		historyEntry.timestamp_str = FolderRewindFormat::MakeLocalHistoryTimestampString();
		historyEntry.worldPath = request.sourcePath.wstring();
		historyEntry.worldName = storageFolderName;
		historyEntry.backupFile = completedBackupFile;
		historyEntry.backupType = backupTypeStr;
		historyEntry.isPartialBackup = independentPartial || FolderRewindFormat::IsSmartBackupType(backupTypeStr);
		historyEntry.comment = comment;
        historyEntry.isImportant = request.protect;
        bool historyCommitted = false;
        if (request.protect && !metadataDurabilityWarning && historyWritable() && !cancelled()) {
            const auto mutation = dependencies_.history->Mutate(config.configId, dependencies_.paths.HistoryFile(),
                dependencies_.historyConfigs, true, [&](vector<HistoryEntry>& entries) {
                    entries.push_back(historyEntry); return true;
                });
            if (mutation.committed && !mutation.persisted) {
                auto uncertain = MakeBackupFailure(OperationCode::BackupFailed, BackupOutcome::Failed,
                    "backup.history.committed_not_durable", "Pinned history committed, but directory durability is unconfirmed; archive and metadata retained.");
                uncertain.archivePath = archivePath; uncertain.historyEntry = historyEntry;
                return uncertain;
            }
            historyCommitted = mutation.changed && mutation.persisted;
        } else if (!request.protect) {
            historyCommitted = dependencies_.addHistory && dependencies_.addHistory(historyEntry);
        }
		if (!historyCommitted) {
			error_code recoveryError;
			if (!independentPartial && stateExisted) {
				const auto preparedState = metadataRecovery / L"restore-state.json";
				filesystem::copy_file(metadataRecovery / L"state.json", preparedState,
					filesystem::copy_options::overwrite_existing, recoveryError);
				if (!recoveryError && !AtomicFileWriter::ReplacePreparedFile(preparedState, statePath).WasReplaced()) {
					recoveryError = make_error_code(errc::io_error);
				}
			} else if (!independentPartial) {
				filesystem::remove(statePath, recoveryError);
			}
			if (!recoveryError && !dependencies_.deleteMetadataRecord(metadataFolder, completedBackupFile)) {
				recoveryError = make_error_code(errc::io_error);
			}
			if (!recoveryError) filesystem::remove(filesystem::path(archivePath), recoveryError);
			BackupResult failure = MakeBackupFailure(cancelled() ? OperationCode::Cancelled : OperationCode::BackupFailed,
                BackupOutcome::Failed, cancelled() ? "backup.cancelled" : "backup.history.commit_failed",
                wstring_to_utf8(completedBackupFile));
			if (recoveryError) {
				stagingCleanup.Release();
				failure.archivePath = archivePath;
				const auto detail = wstring_to_utf8(metadataRecovery.wstring()) + "; archive: " + wstring_to_utf8(archivePath);
				failure.diagnostics.push_back(MakeDiagnostic("backup.rollback.incomplete", DiagnosticSeverity::Error, detail));
				BACKUP_ERROR("Backup rollback is incomplete; recovery materials retained: %s", detail.c_str());
			}
			publish("backup.failed", {{"error", "history_commit_failed"}});
			return failure;
		}
		if (dependencies_.enforceRetention && !options.deferRetention && !independentPartial) {
			dependencies_.enforceRetention(request, historyEntry, stopToken);
		}

		publish("backup.completed", {
			{"config_id", wstring_to_utf8(config.configId)},
			{"world", wstring_to_utf8(storageFolderName)},
			{"file", wstring_to_utf8(completedBackupFile)}});
		// This is the companion-mod terminal event. Publish it immediately
		// after the local archive and history commit so auto-save resumes before
		// potentially slow rclone post-processing begins.
		if (!request.protect) publish("backup_success", {
			{"config", wstring_to_utf8(config.configId)},
			{"config_id", wstring_to_utf8(config.configId)},
			{"folder", wstring_to_utf8(storageFolderName)},
			{"world", wstring_to_utf8(storageFolderName)},
			{"file", wstring_to_utf8(completedBackupFile)},
			{"result", "created"}});

		BackupResult result;
		result.code = metadataDurabilityWarning
			? OperationCode::PartialSuccess
			: OperationCode::Success;
		result.outcome = BackupOutcome::Created;
		result.archivePath = filesystem::path(archivePath);
		result.historyEntry = historyEntry;
		result.diagnostics.push_back(MakeDiagnostic("backup.completed", DiagnosticSeverity::Info));
		if (metadataDurabilityWarning) {
			result.diagnostics.push_back(MakeDiagnostic(
				"backup.metadata.committed_not_durable",
				DiagnosticSeverity::Warning,
				wstring_to_utf8(metadataUpdate.error)));
		}
		result.diagnostics.insert(result.diagnostics.end(),
			deferredDiagnostics.begin(), deferredDiagnostics.end());
        if (request.protect && options.onProtectedCommit) options.onProtectedCommit(result);
		if (dependencies_.cloudPost && !cancelled()) {
            try {
                result.cloud = dependencies_.cloudPost->Run(request, historyEntry, stopToken);
            } catch (const exception& error) {
                if (!request.protect) throw;
                result.cloud.status = CloudPostStatus::Failed;
                result.cloud.diagnostics.push_back(MakeDiagnostic("backup.cloud.post_failed", DiagnosticSeverity::Warning, error.what()));
            } catch (...) {
                if (!request.protect) throw;
                result.cloud.status = CloudPostStatus::Failed;
                result.cloud.diagnostics.push_back(MakeDiagnostic("backup.cloud.post_failed", DiagnosticSeverity::Warning, "Unknown cloud post-processing failure."));
            }
            if (request.protect && result.cloud.status == CloudPostStatus::Failed)
                publish("backup.cloud.warning", {{"file", wstring_to_utf8(completedBackupFile)}});
			result.diagnostics.insert(
				result.diagnostics.end(),
				result.cloud.diagnostics.begin(),
				result.cloud.diagnostics.end());
			if (result.cloud.status == CloudPostStatus::Failed && !request.protect) {
				result.code = OperationCode::PartialSuccess;
			}
		}
		if (cancelled() && result.cloud.status != CloudPostStatus::Failed && !request.protect) {
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
