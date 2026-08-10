#include "BackupServiceTests.h"

#include "BackupService.h"
#include "ExternalToolManager.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "RuntimeIntegration.h"
#include "RuntimeCloudPostHook.h"
#include "RuntimeRetentionService.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <stop_token>
#include <string_view>

using namespace std;

namespace {

void WriteFixture(const filesystem::path& path, const string& content) {
	filesystem::create_directories(path.parent_path());
	ofstream(path, ios::binary | ios::trunc) << content;
}

ArchiveRunner MakeFakeArchiveRunner(
	const shared_ptr<int>& processCount,
	stop_token stopToken) {
	ExternalToolResolution resolution;
	resolution.available = true;
	resolution.executable = filesystem::path(L"fake-7zz");
	resolution.source = ExternalToolSource::Managed;
	return ArchiveRunner(
		std::move(resolution),
		stopToken,
		[processCount](const ProcessSpec& spec, stop_token token) {
			ProcessResult result;
			if (token.stop_requested()) {
				result.status = ProcessStatus::Cancelled;
				return result;
			}
			++*processCount;
			for (const auto& argument : spec.arguments) {
				const filesystem::path candidate(argument);
				if (candidate.extension() != L".7z") continue;
				filesystem::create_directories(candidate.parent_path());
				ofstream archive(candidate, ios::binary | ios::trunc);
				archive << string(128 * 1024, 'a');
				break;
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

	WriteFixture(world / "level.dat", "changed-world-data");
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
	SynchronousRcloneCloudPostHook cloudHook(
		AppPaths{
			.configRoot = cloudRoot,
			.dataRoot = cloudRoot,
			.runtimeRoot = cloudRoot / "runtime"},
		cloudHistory,
		[cloudConfigs] { return cloudConfigs; },
		[copyCount](const ProcessSpec&, stop_token) {
			++*copyCount;
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
}
