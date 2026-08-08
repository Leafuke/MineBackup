#include "BackupServiceTests.h"

#include "BackupService.h"
#include "ExternalToolManager.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "RuntimeIntegration.h"
#include "RuntimeCloudPostHook.h"

#include <fstream>
#include <map>
#include <stop_token>

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
	test.Expect(!events.empty() && events.back().eventId == "backup.completed",
		"BackupService should emit stable runtime event IDs");

	const BackupResult unchanged = service.Run(request);
	test.Expect(unchanged.code == OperationCode::NoChanges
			&& unchanged.outcome == BackupOutcome::NoChanges,
		"BackupService should preserve the no-change outcome");
	test.Expect(*processCount == 1 && history.size() == 1,
		"No-change backup should not create a new archive or history entry");

	WriteFixture(world / "level.dat", "changed-world-data");
	dependencies.cloudPost = make_shared<CallbackCloudPostHook>([](const BackupRequest&, const HistoryEntry&, stop_token) {
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

	stop_source cancelled;
	cancelled.request_stop();
	const BackupResult cancelledResult = service.Run(request, cancelled.get_token());
	test.Expect(cancelledResult.code == OperationCode::Cancelled
			&& cancelledResult.outcome == BackupOutcome::Rejected,
		"BackupService should honor the explicit stop token before doing work");

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
}
