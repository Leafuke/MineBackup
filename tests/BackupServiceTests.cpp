#include "BackupServiceTests.h"

#include "BackupService.h"
#include "ChainSafeRetention.h"
#include "ExternalToolManager.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "FolderRewindMetadataStore.h"
#include "RuntimeIntegration.h"
#include "RuntimeCloudPostHook.h"
#include "RuntimeRetentionService.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <stop_token>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace std;

namespace {

void WriteFixture(const filesystem::path& path, const string& content) {
	filesystem::create_directories(path.parent_path());
	ofstream(path, ios::binary | ios::trunc) << content;
}

string ReadFixture(const filesystem::path& path) {
	ifstream input(path, ios::binary);
	return string((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
}

ArchiveRunner MakeFakeArchiveRunner(
	const shared_ptr<int>& processCount,
	stop_token stopToken,
	size_t archiveSize = 128 * 1024,
	bool createArchive = true) {
	ExternalToolResolution resolution;
	resolution.available = true;
	resolution.executable = filesystem::path(L"fake-7zz");
	resolution.source = ExternalToolSource::Managed;
	return ArchiveRunner(
		std::move(resolution),
		stopToken,
		[processCount, archiveSize, createArchive](const ProcessSpec& spec, stop_token token) {
			ProcessResult result;
			if (token.stop_requested()) {
				result.status = ProcessStatus::Cancelled;
				return result;
			}
			++*processCount;
			if (createArchive) {
				for (const auto& argument : spec.arguments) {
					const filesystem::path candidate(argument);
					if (candidate.extension() != L".7z") continue;
					filesystem::create_directories(candidate.parent_path());
					ofstream archive(candidate, ios::binary | ios::trunc);
					archive << string(archiveSize, 'a');
					break;
				}
			}
			result.status = ProcessStatus::Succeeded;
			result.exitCode = 0;
			return result;
		});
}

const BackupRuntimeEvent* FindLastEvent(
	const vector<BackupRuntimeEvent>& events,
	string_view eventId) {
	for (auto current = events.rbegin(); current != events.rend(); ++current) {
		if (current->eventId == eventId) return &*current;
	}
	return nullptr;
}

string EventField(const BackupRuntimeEvent& event, string_view name) {
	for (const auto& [key, value] : event.fields) {
		if (key == name) return value;
	}
	return {};
}

#ifdef _WIN32
class ScopedCleanupHandle {
public:
	~ScopedCleanupHandle() { Release(); }

	bool Open(const filesystem::path& path) {
		Release();
		handle_ = CreateFileW(
			path.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		return handle_ != INVALID_HANDLE_VALUE;
	}

	bool Release() {
		if (handle_ == INVALID_HANDLE_VALUE) return true;
		const bool closed = CloseHandle(handle_) != FALSE;
		handle_ = INVALID_HANDLE_VALUE;
		return closed;
	}

private:
	HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#else
class ScopedDirectoryPermissions {
public:
	~ScopedDirectoryPermissions() { Restore(); }

	bool Protect(const filesystem::path& path) {
		Restore();
		struct stat status {};
		if (::stat(path.c_str(), &status) != 0) return false;
		originalMode_ = status.st_mode;
		if (::chmod(path.c_str(), status.st_mode & ~S_IWUSR) != 0) return false;
		path_ = path;
		active_ = true;
		return true;
	}

	bool Restore() {
		if (!active_) return true;
		const bool restored = ::chmod(path_.c_str(), originalMode_) == 0;
		if (restored) {
			path_.clear();
			active_ = false;
		}
		return restored;
	}

private:
	filesystem::path path_;
	mode_t originalMode_ = 0;
	bool active_ = false;
};
#endif

void RunDirectRemoveTransactionTests(
	TestContext& test,
	const filesystem::path& temporaryRoot) {
	auto runCase = [&](const filesystem::path& caseRoot,
		bool commitSucceeds,
		bool injectCleanupFailure) {
		Config config;
		config.configId = L"direct-remove-" + caseRoot.filename().wstring();
		config.saveRoot = (caseRoot / "saves").wstring();
		config.worlds = {{L"world", L"World"}};
		config.backupPath = (caseRoot / "backups").wstring();
		config.zipFormat = L"7z";
		const filesystem::path world = caseRoot / "saves" / "world";
		FolderRewindFormat::StoragePaths storage;
		const bool storageResolved = FolderRewindFormat::TryResolveStoragePaths(
			config.backupPath, L"world", world.wstring(), storage);
		test.Expect(storageResolved, "DirectRemove fixture should resolve storage paths");
		if (!storageResolved) return;

		const wstring targetFile = L"[Full]-DirectRemove-A.7z";
		const wstring survivorFile = L"[Full]-DirectRemove-B.7z";
		WriteFixture(world / "level.dat", "direct-remove-world");
		WriteFixture(storage.backupSubDir / targetFile, "target archive");
		WriteFixture(storage.backupSubDir / survivorFile, "survivor archive");

		HistoryEntry target;
		target.configId = config.configId;
		target.worldName = storage.folderName;
		target.worldPath = world.wstring();
		target.backupFile = targetFile;
		target.backupType = L"Full";
		target.timestamp_str = L"2024-01-01T00:00:00";
		HistoryEntry survivor = target;
		survivor.backupFile = survivorFile;
		survivor.timestamp_str = L"2024-01-01T00:01:00";

		FolderRewindFormat::MetadataState metadataState;
		metadataState.lastBackupFileName = survivorFile;
		metadataState.basedOnFullBackup = survivorFile;
		FolderRewindFormat::ChangeRecord targetRecord;
		targetRecord.archiveFileName = targetFile;
		targetRecord.backupType = L"Full";
		targetRecord.basedOnFullBackup = targetFile;
		targetRecord.fullFileList = {L"level.dat"};
		FolderRewindFormat::ChangeRecord survivorRecord = targetRecord;
		survivorRecord.archiveFileName = survivorFile;
		survivorRecord.basedOnFullBackup = survivorFile;
		test.Expect(FolderRewindMetadataStore::SaveState(
				storage.metadataDir, metadataState)
				&& FolderRewindMetadataStore::SaveRecord(
					storage.metadataDir, targetRecord)
				&& FolderRewindMetadataStore::SaveRecord(
					storage.metadataDir, survivorRecord),
			"DirectRemove fixture should persist archive metadata");

		const filesystem::path runtimeRoot = caseRoot / "runtime";
		AppPaths paths;
		paths.runtimeRoot = runtimeRoot;
		vector<HistoryEntry> currentHistory{target, survivor};
		bool commitCalled = false;
		bool cleanupFailureInjected = false;
#ifdef _WIN32
		ScopedCleanupHandle cleanupHandle;
#else
		ScopedDirectoryPermissions protectedTempRoot;
#endif

		ChainSafeRetention::Request request;
		request.config = config;
		request.entry = target;
		request.history = currentHistory;
		request.backupDirectory = storage.backupSubDir;
		request.metadataDirectory = storage.metadataDir;
		request.paths = paths;
		request.commitHistory = [&](vector<HistoryEntry> updated) {
			commitCalled = true;
			if (!commitSucceeds) return false;
			currentHistory = std::move(updated);
			if (!injectCleanupFailure) return true;

			error_code scanError;
			for (filesystem::directory_iterator iterator(runtimeRoot, scanError), end;
				!scanError && iterator != end; iterator.increment(scanError)) {
				error_code entryError;
				if (!iterator->is_directory(entryError) || entryError) continue;
				const wstring name = iterator->path().filename().wstring();
				if (name.rfind(L"MineBackup_Retention_", 0) != 0) continue;
				const filesystem::path tempRoot = iterator->path();
#ifdef _WIN32
				cleanupFailureInjected = cleanupHandle.Open(tempRoot / targetFile);
#else
				cleanupFailureInjected = protectedTempRoot.Protect(tempRoot);
#endif
				break;
			}
			return true;
		};

		const auto result = ChainSafeRetention::Remove(request);
		// The exact temporary directory is generated by production code; find it
		// again so the assertion observes the injected residue without guessing its GUID.
		bool retentionResidue = false;
		if (injectCleanupFailure && cleanupFailureInjected) {
			error_code scanError;
			for (filesystem::directory_iterator iterator(runtimeRoot, scanError), end;
				!scanError && iterator != end; iterator.increment(scanError)) {
				if (iterator->path().filename().wstring().rfind(
						L"MineBackup_Retention_", 0) == 0) {
					retentionResidue = true;
					break;
				}
			}
		}
		FolderRewindFormat::ChangeRecord loadedTarget;
		const bool targetMetadataExists = FolderRewindMetadataStore::LoadRecord(
			storage.metadataDir, targetFile, loadedTarget);
		const bool targetHistoryExists = any_of(
			currentHistory.begin(), currentHistory.end(),
			[&](const HistoryEntry& entry) {
				return entry.backupFile == targetFile;
			});
		if (injectCleanupFailure) {
			test.Expect(commitCalled && cleanupFailureInjected
					&& result.changed && !result.warning
					&& !filesystem::exists(storage.backupSubDir / targetFile)
					&& !targetMetadataExists && !targetHistoryExists
					&& retentionResidue,
				"Committed DirectRemove must not rollback data when temp cleanup fails");
		}
		else {
			test.Expect(commitCalled && !result.changed && result.warning
					&& filesystem::exists(storage.backupSubDir / targetFile)
					&& targetMetadataExists && targetHistoryExists,
				"DirectRemove must rollback archive, metadata, and history on commit failure");
		}

#ifdef _WIN32
		test.Expect(cleanupHandle.Release(),
			"DirectRemove test should release the temporary archive handle");
#else
		test.Expect(protectedTempRoot.Restore(),
			"DirectRemove test should restore temporary-directory permissions");
#endif
		error_code cleanupError;
		filesystem::remove_all(caseRoot, cleanupError);
		test.Expect(!cleanupError, "DirectRemove fixture should clean up after the transaction test");
	};

#ifndef _WIN32
	if (::geteuid() == 0) {
		cout << "[SKIP] DirectRemove cleanup-failure injection requires a non-root POSIX test user\n";
	}
	else
#endif
	{
		runCase(temporaryRoot / "direct-remove-cleanup-failure", true, true);
	}
	runCase(temporaryRoot / "direct-remove-commit-failure", false, false);
}

} // namespace

void RunBackupServiceTests(
	TestContext& test,
	const filesystem::path& temporaryRoot) {
	const filesystem::path root = temporaryRoot / "backup-service";
	const filesystem::path world = root / "saves" / "world";
	WriteFixture(world / "level.dat", "world-data");

	Config config;
	config.configId = L"config-service-test";
	config.saveRoot = (root / "saves").wstring();
	config.backupPath = (root / "backups").wstring();
	config.zipPath = L"fake-7zz";
	config.zipFormat = L"7z";
	config.backupMode = 1;
	config.skipIfUnchanged = true;
	config.worlds = {{L"world", L"World"}};

	BackupRequest request;
	request.config = config;
	request.world = {config.configId, L"world"};
	request.sourcePath = world;
	request.displayName = L"World";
	request.comment = L"service-test";

	vector<HistoryEntry> history;
	vector<BackupRuntimeEvent> events;
	auto processCount = make_shared<int>(0);
	BackupServiceDependencies dependencies;
	dependencies.paths.runtimeRoot = root / "runtime";
	dependencies.archiveRunnerFactory = [processCount](
		const filesystem::path&,
		const AppPaths&,
		stop_token token) {
		return MakeFakeArchiveRunner(processCount, token);
	};
	dependencies.addHistory = [&](const HistoryEntry& entry) {
		history.push_back(entry);
		return true;
	};
	dependencies.removeHistory = [](const wstring&, const wstring&) { return true; };
	dependencies.eventSink = make_shared<CallbackRuntimeEventSink>([&](const BackupRuntimeEvent& event) {
		events.push_back(event);
	});

	BackupService service(dependencies);
	const BackupResult created = service.Run(request);
	test.Expect(created.code == OperationCode::Success
			&& created.outcome == BackupOutcome::Created,
		"BackupService should create a real archive through its injected process port");
	test.Expect(filesystem::is_regular_file(created.archivePath),
		"BackupService should return the committed archive path");
	test.Expect(created.historyEntry.has_value() && history.size() == 1,
		"BackupService should commit and return the same history entry");
	const auto* backupStarted = FindLastEvent(events, "backup_started");
	const auto* backupSucceeded = FindLastEvent(events, "backup_success");
	test.Expect(backupStarted && backupSucceeded
			&& EventField(*backupSucceeded, "world") == "world"
			&& !EventField(*backupSucceeded, "file").empty(),
		"BackupService should emit the companion-mod backup lifecycle");

	const BackupResult unchanged = service.Run(request);
	test.Expect(unchanged.code == OperationCode::NoChanges
			&& unchanged.outcome == BackupOutcome::NoChanges,
		"BackupService should preserve the no-change outcome");
	test.Expect(*processCount == 1 && history.size() == 1,
		"No-change backup should not create a new archive or history entry");
	const auto* noChanges = FindLastEvent(events, "command_completed");
	test.Expect(noChanges
			&& EventField(*noChanges, "command") == "BACKUP"
			&& EventField(*noChanges, "result") == "no_changes",
		"No-change backup should emit a terminal event that releases hot-backup state");

	auto runArchiveHealthCase = [&](const char* name, size_t archiveSize, bool createArchive) {
		const filesystem::path caseRoot = temporaryRoot / (string("backup-archive-health-") + name);
		const filesystem::path caseWorld = caseRoot / "saves" / "world";
		WriteFixture(caseWorld / "level.dat", "archive-health-world-data");
		Config caseConfig = config;
		caseConfig.configId = L"config-archive-health";
		caseConfig.saveRoot = (caseRoot / "saves").wstring();
		caseConfig.backupPath = (caseRoot / "backups").wstring();
		caseConfig.skipIfUnchanged = false;
		BackupRequest caseRequest = request;
		caseRequest.config = caseConfig;
		caseRequest.world = {caseConfig.configId, L"world"};
		caseRequest.sourcePath = caseWorld;

		BackupServiceDependencies caseDependencies = dependencies;
		caseDependencies.paths.runtimeRoot = caseRoot / "runtime";
		caseDependencies.archiveRunnerFactory = [processCount, archiveSize, createArchive](
			const filesystem::path&,
			const AppPaths&,
			stop_token token) {
			return MakeFakeArchiveRunner(processCount, token, archiveSize, createArchive);
		};
		return BackupService(std::move(caseDependencies)).Run(caseRequest);
	};

	events.clear();
	const BackupResult smallArchive = runArchiveHealthCase("small", 1, true);
	test.Expect(smallArchive.code == OperationCode::Success
			&& smallArchive.outcome == BackupOutcome::Created
			&& filesystem::file_size(smallArchive.archivePath) == 1
			&& FindLastEvent(events, "backup_warning") == nullptr,
		"A valid non-empty small archive must not fail or warn the backup operation");
	const BackupResult missingArchive = runArchiveHealthCase("missing", 0, false);
	test.Expect(missingArchive.code == OperationCode::BackupFailed
			&& missingArchive.outcome == BackupOutcome::Failed
			&& any_of(missingArchive.diagnostics.begin(), missingArchive.diagnostics.end(),
				[](const Diagnostic& diagnostic) {
					return diagnostic.eventId == "backup.archive.missing";
				}),
		"A successful archive command without an archive must be reported as missing");
	const BackupResult emptyArchive = runArchiveHealthCase("empty", 0, true);
	test.Expect(emptyArchive.code == OperationCode::BackupFailed
			&& emptyArchive.outcome == BackupOutcome::Failed
			&& any_of(emptyArchive.diagnostics.begin(), emptyArchive.diagnostics.end(),
				[](const Diagnostic& diagnostic) {
					return diagnostic.eventId == "backup.archive.empty";
				}),
		"A zero-byte archive must be reported as empty");

	WriteFixture(world / "level.dat", "changed-world-data");
	BackupServiceDependencies unavailableDependencies = dependencies;
	unavailableDependencies.archiveRunnerFactory = [](
		const filesystem::path&,
		const AppPaths&,
		stop_token token) {
		ExternalToolResolution resolution;
		resolution.diagnostic = L"No supported 7zz executable was found.";
		return ArchiveRunner(std::move(resolution), token);
	};
	const BackupResult unavailable = BackupService(unavailableDependencies).Run(request);
	test.Expect(unavailable.code == OperationCode::ToolUnavailable
			&& unavailable.outcome == BackupOutcome::Failed
			&& any_of(unavailable.diagnostics.begin(), unavailable.diagnostics.end(),
				[](const Diagnostic& diagnostic) {
					return diagnostic.eventId == "backup.tool.unavailable";
				}),
		"BackupService should report a deterministic tool-unavailable contract through its injected archive port");

	events.clear();
	bool successPublishedBeforeCloud = false;
	dependencies.cloudPost = make_shared<CallbackCloudPostHook>([&](const BackupRequest&, const HistoryEntry&, stop_token) {
		successPublishedBeforeCloud = FindLastEvent(events, "backup_success") != nullptr;
		CloudPostResult result;
		result.status = CloudPostStatus::Failed;
		result.diagnostics.push_back({
			"cloud.upload.failed", DiagnosticSeverity::Error, "fixture"});
		return result;
	});
	BackupService cloudFailureService(dependencies);
	const BackupResult partial = cloudFailureService.Run(request);
	test.Expect(partial.code == OperationCode::PartialSuccess
			&& partial.outcome == BackupOutcome::Created
			&& filesystem::exists(partial.archivePath),
		"Cloud failure should preserve the local backup and return partial success");
	test.Expect(successPublishedBeforeCloud,
		"Hot-backup success should release companion auto-save before cloud post-processing");

	stop_source cancelled;
	cancelled.request_stop();
	const BackupResult cancelledResult = service.Run(request, cancelled.get_token());
	test.Expect(cancelledResult.code == OperationCode::Cancelled
			&& cancelledResult.outcome == BackupOutcome::Rejected,
		"BackupService should honor the explicit stop token before doing work");
	test.Expect(FindLastEvent(events, "backup_failed") != nullptr,
		"Rejected backup should emit the companion-mod failure terminal event");

	request.config.cloudSyncEnabled = true;
	NetworkDisabledCloudPostHook networkDisabledCloud;
	const CloudPostResult skippedCloud = networkDisabledCloud.Run(
		request, history.front(), {});
	test.Expect(skippedCloud.status == CloudPostStatus::Skipped
			&& !skippedCloud.diagnostics.empty()
			&& skippedCloud.diagnostics.front().eventId == "cloud.network_disabled",
		"NetworkDisabled cloud adapter should skip with a stable diagnostic");

	NetworkDisabledKnotLinkBridge networkDisabledKnotLink;
	const HotBackupPreparation degraded = networkDisabledKnotLink.Prepare(request, {});
	test.Expect(degraded.status == HotBackupStatus::Degraded
			&& !degraded.diagnostics.empty()
			&& degraded.diagnostics.front().eventId == "knotlink.network_disabled",
		"NetworkDisabled KnotLink adapter should preserve the live-file fallback");

	WriteFixture(world / "level.dat", "locked-world-fallback-data");
	request.config.cloudSyncEnabled = false;
	bool hotBackupAttempted = false;
	dependencies.isFileLocked = [](const filesystem::path&) { return true; };
	dependencies.hotBackup = make_shared<CallbackHotBackupBridge>(
		[&](const BackupRequest&, stop_token) {
			hotBackupAttempted = true;
			HotBackupPreparation preparation;
			preparation.status = HotBackupStatus::Degraded;
			preparation.diagnostics.push_back({
				"knotlink.hot_backup.timeout", DiagnosticSeverity::Warning,
				"fixture"});
			return preparation;
		});
	dependencies.cloudPost = make_shared<NoopCloudPostHook>();
	const BackupResult fallback = BackupService(dependencies).Run(request);
	test.Expect(hotBackupAttempted
			&& fallback.code == OperationCode::Success
			&& fallback.outcome == BackupOutcome::Created
			&& any_of(fallback.diagnostics.begin(), fallback.diagnostics.end(),
				[](const Diagnostic& diagnostic) {
					return diagnostic.eventId == "knotlink.hot_backup.timeout"
						&& diagnostic.severity == DiagnosticSeverity::Warning;
				}),
		"A timed-out hot-backup handshake should warn and continue with the live-file fallback");

	const filesystem::path cloudRoot = temporaryRoot / "runtime-cloud-post";
	Config cloudConfig = config;
	cloudConfig.cloudSyncEnabled = true;
	cloudConfig.cloudSyncMode = static_cast<int>(CloudSyncMode::HistoryAndBackups);
	cloudConfig.rcloneRemotePath = L"fixture:minebackup";
	cloudConfig.backupPath = (cloudRoot / "backups").wstring();
	map<int, Config> cloudConfigs{{1, cloudConfig}};
	FolderRewindFormat::StoragePaths cloudStorage;
	test.Expect(FolderRewindFormat::TryResolveStoragePaths(
		cloudConfig.backupPath, L"world", world.wstring(), cloudStorage),
		"Cloud post fixture should resolve FolderRewind paths");
	HistoryEntry cloudEntry = history.front();
	cloudEntry.configId = cloudConfig.configId;
	cloudEntry.worldName = cloudStorage.folderName;
	cloudEntry.worldPath = world.wstring();
	cloudEntry.backupFile = L"fixture.7z";
	WriteFixture(cloudStorage.backupSubDir / cloudEntry.backupFile, "archive");
	WriteFixture(cloudStorage.statePath, "{}");
	WriteFixture(cloudStorage.recordsDir / (cloudEntry.backupFile + L".json"), "{}");
	HistoryRepository cloudHistory;
	FolderRewindHistoryStore::HistoryByConfigId initialHistory;
	initialHistory[cloudConfig.configId].push_back(cloudEntry);
	test.Expect(cloudHistory.ReplaceAll(
		std::move(initialHistory), cloudRoot / "history.json", cloudConfigs, true),
		"Cloud post fixture history should persist");

	auto copyCount = make_shared<int>(0);
	vector<pair<wstring, string>> cloudUploads;
	SynchronousRcloneCloudPostHook cloudHook(
		AppPaths{
			.configRoot = cloudRoot,
			.dataRoot = cloudRoot,
			.runtimeRoot = cloudRoot / "runtime"},
		cloudHistory,
		[cloudConfigs] { return cloudConfigs; },
		[copyCount, &cloudUploads](const ProcessSpec& spec, stop_token) {
			++*copyCount;
			if (spec.arguments.size() >= 3) {
				cloudUploads.emplace_back(
					spec.arguments[2], ReadFixture(filesystem::path(spec.arguments[1])));
			}
			ProcessResult process;
			process.status = ProcessStatus::Succeeded;
			process.exitCode = 0;
			return process;
		},
		[](const filesystem::path&, const AppPaths&, stop_token) {
			ExternalToolResolution resolution;
			resolution.available = true;
			resolution.executable = L"fake-rclone";
			resolution.source = ExternalToolSource::Managed;
			return resolution;
		});
	BackupRequest cloudRequest = request;
	cloudRequest.config = cloudConfig;
	cloudRequest.world.configId = cloudConfig.configId;
	const CloudPostResult uploaded = cloudHook.Run(cloudRequest, cloudEntry, {});
	test.Expect(uploaded.status == CloudPostStatus::Succeeded && *copyCount == 5,
		"Synchronous cloud hook should wait for archive, metadata, history, and manifest uploads");
	const auto cloudEntries = cloudHistory.EntriesForConfig(cloudConfig.configId);
	test.Expect(cloudEntries->size() == 1
			&& cloudEntries->front().isCloudArchived
			&& !cloudEntries->front().cloudArchiveRemotePath.empty(),
		"Synchronous cloud hook should atomically commit cloud history state");
	auto uploadedHistory = find_if(cloudUploads.begin(), cloudUploads.end(),
		[](const auto& upload) {
			return upload.first.find(L"history.json") != wstring::npos
				&& upload.first.find(L"active-history.json") == wstring::npos;
		});
	test.Expect(uploadedHistory != cloudUploads.end()
			&& uploadedHistory->second.find("\"IsCloudArchived\": true") != string::npos,
		"Cloud history upload should include the local cloud state committed by the same backup");

	cloudUploads.clear();
	Config noRemoteHistoryConfig = cloudConfig;
	noRemoteHistoryConfig.cloudSyncHistoryAfterUpload = false;
	const filesystem::path noRemoteHistoryRoot = cloudRoot / "no-history";
	HistoryEntry noRemoteHistoryEntry = cloudEntry;
	noRemoteHistoryEntry.isCloudArchived = false;
	HistoryRepository noRemoteHistory;
	FolderRewindHistoryStore::HistoryByConfigId noRemoteInitialHistory;
	noRemoteInitialHistory[noRemoteHistoryConfig.configId].push_back(noRemoteHistoryEntry);
	test.Expect(noRemoteHistory.ReplaceAll(
		std::move(noRemoteInitialHistory),
		noRemoteHistoryRoot / "history.json",
		map<int, Config>{{1, noRemoteHistoryConfig}},
		true),
		"Cloud history switch-off fixture should persist local history");
	SynchronousRcloneCloudPostHook noRemoteHistoryHook(
		AppPaths{
			.configRoot = noRemoteHistoryRoot,
			.dataRoot = noRemoteHistoryRoot,
			.runtimeRoot = noRemoteHistoryRoot / "runtime"},
		noRemoteHistory,
		[noRemoteHistoryConfig] {
			return map<int, Config>{{1, noRemoteHistoryConfig}};
		},
		[&cloudUploads](const ProcessSpec& spec, stop_token) {
			if (spec.arguments.size() >= 3) {
				cloudUploads.emplace_back(
					spec.arguments[2], ReadFixture(filesystem::path(spec.arguments[1])));
			}
			ProcessResult process;
			process.status = ProcessStatus::Succeeded;
			return process;
		},
		[](const filesystem::path&, const AppPaths&, stop_token) {
			ExternalToolResolution resolution;
			resolution.available = true;
			resolution.executable = L"fake-rclone";
			return resolution;
		});
	BackupRequest noRemoteHistoryRequest = cloudRequest;
	noRemoteHistoryRequest.config = noRemoteHistoryConfig;
	noRemoteHistoryRequest.world.configId = noRemoteHistoryConfig.configId;
	const CloudPostResult noRemoteHistoryResult = noRemoteHistoryHook.Run(
		noRemoteHistoryRequest, noRemoteHistoryEntry, {});
	const bool uploadedGlobalHistory = any_of(
		cloudUploads.begin(), cloudUploads.end(), [](const auto& upload) {
			return upload.first.find(L"history.json") != wstring::npos;
		});
	const auto noRemoteEntries = noRemoteHistory.EntriesForConfig(
		noRemoteHistoryConfig.configId);
	test.Expect(noRemoteHistoryResult.status == CloudPostStatus::Succeeded
			&& cloudUploads.size() == 3
			&& !uploadedGlobalHistory
			&& noRemoteEntries->size() == 1
			&& noRemoteEntries->front().isCloudArchived,
		"cloudSyncHistoryAfterUpload=false should skip remote history while committing local state");

	const filesystem::path cloudFailureRoot = cloudRoot / "history-upload-failure";
	HistoryRepository cloudFailureHistory;
	FolderRewindHistoryStore::HistoryByConfigId failureInitialHistory;
	failureInitialHistory[cloudConfig.configId].push_back(cloudEntry);
	test.Expect(cloudFailureHistory.ReplaceAll(
		std::move(failureInitialHistory),
		cloudFailureRoot / "history.json",
		cloudConfigs,
		true),
		"Cloud history failure fixture should persist local history");
	bool historyUploadAttempted = false;
	SynchronousRcloneCloudPostHook cloudFailureHook(
		AppPaths{
			.configRoot = cloudFailureRoot,
			.dataRoot = cloudFailureRoot,
			.runtimeRoot = cloudFailureRoot / "runtime"},
		cloudFailureHistory,
		[cloudConfigs] { return cloudConfigs; },
		[&historyUploadAttempted](const ProcessSpec& spec, stop_token) {
			ProcessResult process;
			if (spec.arguments.size() >= 3
				&& spec.arguments[2].find(L"history.json") != wstring::npos
				&& spec.arguments[2].find(L"active-history.json") == wstring::npos) {
				historyUploadAttempted = true;
				process.status = ProcessStatus::ExitedWithError;
				process.exitCode = 17;
				process.error = L"fixture history upload failure";
				return process;
			}
			process.status = ProcessStatus::Succeeded;
			process.exitCode = 0;
			return process;
		},
		[](const filesystem::path&, const AppPaths&, stop_token) {
			ExternalToolResolution resolution;
			resolution.available = true;
			resolution.executable = L"fake-rclone";
			return resolution;
		});
	const CloudPostResult cloudFailure = cloudFailureHook.Run(
		cloudRequest, cloudEntry, {});
	const auto failureEntries = cloudFailureHistory.EntriesForConfig(cloudConfig.configId);
	test.Expect(cloudFailure.status == CloudPostStatus::Failed
			&& historyUploadAttempted
			&& failureEntries->size() == 1
			&& failureEntries->front().isCloudArchived,
		"Remote history failure should preserve committed local cloud state and report failure");

	const filesystem::path retentionRoot = temporaryRoot / "runtime-retention";
	Config retentionConfig = config;
	retentionConfig.backupPath = (retentionRoot / "backups").wstring();
	retentionConfig.keepCount = 1;
	retentionConfig.backupMode = 1;
	map<int, Config> retentionConfigs{{1, retentionConfig}};
	FolderRewindFormat::StoragePaths retentionStorage;
	test.Expect(FolderRewindFormat::TryResolveStoragePaths(
		retentionConfig.backupPath, L"world", world.wstring(), retentionStorage),
		"Retention fixture should resolve FolderRewind paths");
	const filesystem::path oldArchive = retentionStorage.backupSubDir / L"old.7z";
	const filesystem::path newArchive = retentionStorage.backupSubDir / L"new.7z";
	WriteFixture(oldArchive, "old");
	WriteFixture(newArchive, "new");
	filesystem::last_write_time(
		oldArchive, filesystem::file_time_type::clock::now() - chrono::hours(1));
	HistoryEntry oldEntry = cloudEntry;
	oldEntry.configId = retentionConfig.configId;
	oldEntry.worldName = retentionStorage.folderName;
	oldEntry.worldPath = world.wstring();
	oldEntry.backupFile = L"old.7z";
	HistoryEntry newEntry = oldEntry;
	newEntry.backupFile = L"new.7z";
	HistoryRepository retentionHistory;
	FolderRewindHistoryStore::HistoryByConfigId retentionItems;
	retentionItems[retentionConfig.configId] = {oldEntry, newEntry};
	const filesystem::path retentionHistoryFile = retentionRoot / "history.json";
	test.Expect(retentionHistory.ReplaceAll(
		std::move(retentionItems), retentionHistoryFile, retentionConfigs, true),
		"Retention fixture history should persist");
	RuntimeRetentionService retention(
		retentionHistory, retentionHistoryFile, retentionConfigs);
	BackupRequest retentionRequest = request;
	retentionRequest.config = retentionConfig;
	retentionRequest.world.configId = retentionConfig.configId;
	retention.Enforce(retentionRequest, newEntry);
	test.Expect(!filesystem::exists(oldArchive) && filesystem::exists(newArchive)
			&& retentionHistory.EntriesForConfig(retentionConfig.configId)->size() == 1,
		"Runtime retention should atomically remove the oldest ordinary archive and history entry");
	RunDirectRemoveTransactionTests(test, temporaryRoot / "direct-remove-transactions");

	auto runSmartRetention = [&](const filesystem::path& chainRoot,
		bool importantTail, bool failMerge, bool cancelMerge, const string& message) {
		Config chainConfig = retentionConfig;
		chainConfig.configId = L"chain-" + chainRoot.filename().wstring();
		chainConfig.backupPath = (chainRoot / "backups").wstring();
		chainConfig.backupMode = 2;
		chainConfig.keepCount = 1;
		map<int, Config> chainConfigs{{1, chainConfig}};
		FolderRewindFormat::StoragePaths chainStorage;
		FolderRewindFormat::TryResolveStoragePaths(
			chainConfig.backupPath, L"world", world.wstring(), chainStorage);
		const wstring fullFile = L"[Full]-Chain.7z";
		const wstring smartOneFile = L"[Smart]-Chain-1.7z";
		const wstring smartTwoFile = L"[Smart]-Chain-2.7z";
		WriteFixture(chainStorage.backupSubDir / fullFile, "full");
		WriteFixture(chainStorage.backupSubDir / smartOneFile, "smart-one");
		WriteFixture(chainStorage.backupSubDir / smartTwoFile, "smart-two");
		HistoryEntry full = oldEntry;
		full.configId = chainConfig.configId;
		full.worldName = chainStorage.folderName;
		full.backupFile = fullFile;
		full.backupType = L"Full";
		full.timestamp_str = L"2024-01-01T00:00:00";
		HistoryEntry smartOne = full;
		smartOne.backupFile = smartOneFile;
		smartOne.backupType = L"Smart";
		smartOne.timestamp_str = L"2024-01-01T00:01:00";
		HistoryEntry smartTwo = smartOne;
		smartTwo.backupFile = smartTwoFile;
		smartTwo.timestamp_str = L"2024-01-01T00:02:00";
		smartTwo.isImportant = importantTail;
		FolderRewindFormat::MetadataState chainState;
		chainState.lastBackupFileName = smartTwoFile;
		chainState.basedOnFullBackup = fullFile;
		test.Expect(FolderRewindMetadataStore::SaveState(
			chainStorage.metadataDir, chainState),
			"Smart retention fixture should write metadata state");
		FolderRewindFormat::ChangeRecord fullRecord;
		fullRecord.archiveFileName = fullFile;
		fullRecord.backupType = L"Full";
		fullRecord.basedOnFullBackup = fullFile;
		fullRecord.fullFileList = {L"level.dat"};
		FolderRewindFormat::ChangeRecord oneRecord;
		oneRecord.archiveFileName = smartOneFile;
		oneRecord.backupType = L"Smart";
		oneRecord.previousBackupFileName = fullFile;
		oneRecord.basedOnFullBackup = fullFile;
		oneRecord.addedFiles = {L"one.dat"};
		oneRecord.fullFileList = {L"level.dat", L"one.dat"};
		FolderRewindFormat::ChangeRecord twoRecord;
		twoRecord.archiveFileName = smartTwoFile;
		twoRecord.backupType = L"Smart";
		twoRecord.previousBackupFileName = smartOneFile;
		twoRecord.basedOnFullBackup = fullFile;
		twoRecord.addedFiles = {L"two.dat"};
		twoRecord.fullFileList = {L"level.dat", L"one.dat", L"two.dat"};
		test.Expect(FolderRewindMetadataStore::SaveRecord(chainStorage.metadataDir, fullRecord)
			&& FolderRewindMetadataStore::SaveRecord(chainStorage.metadataDir, oneRecord)
			&& FolderRewindMetadataStore::SaveRecord(chainStorage.metadataDir, twoRecord),
			"Smart retention fixture should write a complete metadata chain");
		HistoryRepository chainHistory;
		FolderRewindHistoryStore::HistoryByConfigId chainItems;
		chainItems[chainConfig.configId] = {full, smartOne, smartTwo};
		const auto chainHistoryFile = chainRoot / "history.json";
		test.Expect(chainHistory.ReplaceAll(
			std::move(chainItems), chainHistoryFile, chainConfigs, true),
			"Smart retention fixture history should persist");
		AppPaths chainPaths;
		chainPaths.runtimeRoot = chainRoot / "runtime";
		const auto cancelSource = cancelMerge ? make_shared<stop_source>()
			: shared_ptr<stop_source>{};
		const auto process = [failMerge, cancelMerge, cancelSource](
			const ProcessSpec& spec, stop_token) {
			ProcessResult result;
			if (cancelMerge && !spec.arguments.empty()
				&& spec.arguments.front() == L"x") {
				cancelSource->request_stop();
				result.status = ProcessStatus::Cancelled;
				return result;
			}
			if (!spec.arguments.empty() && spec.arguments.front() == L"a") {
				if (failMerge) {
					result.status = ProcessStatus::ExitedWithError;
					result.exitCode = 7;
					return result;
				}
				for (const auto& argument : spec.arguments) {
					if (argument.size() > 3 && argument.rfind(L".7z") == argument.size() - 3) {
						WriteFixture(argument, "rebuilt");
						break;
					}
				}
				result.status = ProcessStatus::Succeeded;
				return result;
			}
			if (!spec.arguments.empty() && spec.arguments.front() == L"x") {
				filesystem::path destination;
				for (const auto& argument : spec.arguments) {
					if (argument.rfind(L"-o", 0) == 0) destination = argument.substr(2);
				}
				WriteFixture(destination / "chain-state.txt", "extracted");
				result.status = ProcessStatus::Succeeded;
				return result;
			}
			result.status = ProcessStatus::FailedToStart;
			return result;
		};
		const auto resolver = [](const filesystem::path&, const AppPaths&, stop_token) {
			ExternalToolResolution resolution;
			resolution.available = true;
			resolution.executable = L"fake-7zz";
			resolution.source = ExternalToolSource::Managed;
			return resolution;
		};
		RuntimeRetentionService chainRetention(
			chainHistory, chainHistoryFile, chainConfigs, chainPaths, process, resolver);
		BackupRequest chainRequest = request;
		chainRequest.config = chainConfig;
		chainRequest.world.configId = chainConfig.configId;
		chainRetention.Enforce(
			chainRequest,
			smartTwo,
			cancelSource ? cancelSource->get_token() : stop_token{});
		const auto remainingEntries = chainHistory.EntriesForConfig(chainConfig.configId);
		const int archiveCount = static_cast<int>(distance(
			filesystem::directory_iterator(chainStorage.backupSubDir),
			filesystem::directory_iterator{}));
		const bool expected = cancelMerge
			? archiveCount == 3 && remainingEntries->size() == 3
			: importantTail
			? archiveCount == 2 && remainingEntries->size() == 2
			: failMerge
				? archiveCount == 3 && remainingEntries->size() == 3
				: archiveCount == 1 && remainingEntries->size() == 1
					&& FolderRewindFormat::IsFullLikeBackupType(
						remainingEntries->front().backupType);
		test.Expect(expected, message.c_str());
		if (!importantTail && !failMerge && !cancelMerge) {
			const auto finalEntry = remainingEntries->front();
			FolderRewindFormat::ChangeRecord finalRecord;
			FolderRewindFormat::MetadataState finalState;
			test.Expect(FolderRewindMetadataStore::LoadRecord(
				chainStorage.metadataDir, finalEntry.backupFile, finalRecord)
				&& FolderRewindMetadataStore::LoadState(chainStorage.metadataDir, finalState)
				&& finalRecord.backupType == L"Full"
				&& finalRecord.fullFileList.size() == 3
				&& finalState.lastBackupFileName == finalEntry.backupFile,
				"Smart retention should repair the surviving archive metadata and history references");
		}
	};

	runSmartRetention(
		temporaryRoot / "runtime-retention-smart",
		false,
		false,
		false,
		"Runtime retention should merge a Full -> Smart -> Smart chain in Smart mode");
	runSmartRetention(
		temporaryRoot / "runtime-retention-important",
		true,
		false,
		false,
		"Runtime retention should retain an Important Smart tail and its required predecessor");
	runSmartRetention(
		temporaryRoot / "runtime-retention-failed-merge",
		false,
		true,
		false,
		"Failed Smart retention merge should leave archives and history unchanged");
	runSmartRetention(
		temporaryRoot / "runtime-retention-cancelled-merge",
		false,
		false,
		true,
		"Cancelled Smart retention merge should leave archives and history unchanged");
}
