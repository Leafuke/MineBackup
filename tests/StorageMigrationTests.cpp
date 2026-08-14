#include "AtomicFileWriter.h"
#include "AppPaths.h"
#include "LaunchOptions.h"
#include "DiagnosticLogExporter.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "FolderRewindMetadataStore.h"
#include "MigrationCoordinator.h"
#include "SingleInstanceService.h"
#include "LegacyLocationDiscovery.h"
#include "LegacyLocationMigration.h"
#include "Logging.h"
#include "KnotLinkPackageManager.h"
#include "ProcessRunner.h"
#include "TaskCoordinator.h"
#include "InterruptedTaskRecovery.h"
#include "NetworkService.h"
#include "RemoteContentService.h"
#include "Sha256.h"
#include "ExternalToolManager.h"
#include "PortableConfigDocument.h"
#include "DesktopServices.h"
#include "LegacyServicePolicy.h"
#include "text_to_text.h"
#include "BackupPipelineTest.h"
#include "TestSupport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

#include "StorageMigrationTests.h"

namespace {

void TestFormat(TestContext& test) {
    test.Expect(FolderRewindFormat::IsSafeSinglePathSegment(L"World One"), "normal world name should be safe");
    test.Expect(!FolderRewindFormat::IsSafeSinglePathSegment(L"../World"), "parent traversal must be rejected");
    const auto sanitized = FolderRewindFormat::SanitizePathSegment(L"../World");
    test.Expect(FolderRewindFormat::IsSafeSinglePathSegment(sanitized), "sanitized world name should be a safe segment");
    test.Expect(FolderRewindFormat::IsSmartBackupType(L"[Smart]-World.7z"), "smart archive should be recognized");
    const auto generatedConfigId = FolderRewindFormat::EnsureConfigId(L"");
    test.Expect(generatedConfigId.size() == 36 && generatedConfigId[8] == L'-' && generatedConfigId[13] == L'-'
        && generatedConfigId[18] == L'-' && generatedConfigId[23] == L'-',
        "empty ConfigId should produce a canonical UUID on every platform");
}

void TestUnicodeFilesystemPath(TestContext& test, const std::filesystem::path& root) {
    const std::wstring unicodeSegment = L"\u4E2D\u6587\u8DEF\u5F84";
    const std::filesystem::path unicodePath(unicodeSegment);
    test.Expect(unicodePath.wstring() == unicodeSegment,
        "the supported standard library should round-trip a Unicode filesystem path");

    std::error_code error;
    const auto directory = root / unicodePath;
    std::filesystem::create_directories(directory, error);
    test.Expect(!error && std::filesystem::is_directory(directory),
        "the supported standard library should create a Unicode directory");
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

std::filesystem::path NormalizeTestPath(const std::filesystem::path& path) {
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (!error) return normalized;
    error.clear();
    normalized = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : normalized.lexically_normal();
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto normalizedLeft = NormalizeTestPath(left);
    const auto normalizedRight = NormalizeTestPath(right);
#ifdef _WIN32
    auto leftText = normalizedLeft.wstring();
    auto rightText = normalizedRight.wstring();
    std::transform(leftText.begin(), leftText.end(), leftText.begin(), ::towlower);
    std::transform(rightText.begin(), rightText.end(), rightText.begin(), ::towlower);
    return leftText == rightText;
#else
    return normalizedLeft == normalizedRight;
#endif
}

void ExpectSamePath(TestContext& test, const std::filesystem::path& actual,
    const std::filesystem::path& expected, const char* message) {
    const bool same = SamePath(actual, expected);
    if (!same) {
        std::wcerr << L"[DETAIL] actual path: " << actual.wstring()
            << L"\n[DETAIL] expected path: " << expected.wstring() << L'\n';
    }
    test.Expect(same, message);
}

void TestAtomicWriter(TestContext& test, const std::filesystem::path& root) {
    const auto target = root / "atomic" / "value.txt";
    test.Expect(AtomicFileWriter::WriteText(target, "first").success, "first atomic write should succeed");
    test.Expect(AtomicFileWriter::WriteText(target, "second").success, "replacement atomic write should succeed");
    test.Expect(ReadFile(target) == "second", "atomic target should contain the replacement");
    test.Expect(ReadFile(target.wstring() + L".bak") == "first", "atomic backup should contain the previous value");

    int temporaryFiles = 0;
    std::error_code iterationError;
    for (const auto& entry : std::filesystem::directory_iterator(target.parent_path(), iterationError)) {
        if (entry.path().filename().wstring().find(L".tmp.") != std::wstring::npos) ++temporaryFiles;
    }
    test.Expect(!iterationError, "the atomic write directory should remain inspectable");
    test.Expect(temporaryFiles == 0, "successful atomic writes should not leave temporary files");

    const auto concurrentTarget = root / "atomic" / "concurrent.txt";
    test.Expect(AtomicFileWriter::WriteText(concurrentTarget, "seed").success,
        "the concurrent atomic target should be initialized");
    std::vector<int> successes(8, 0);
    std::vector<std::thread> writers;
    for (size_t index = 0; index < successes.size(); ++index) {
        writers.emplace_back([&, index] {
            successes[index] = AtomicFileWriter::WriteText(
                concurrentTarget, "writer-" + std::to_string(index)).success ? 1 : 0;
        });
    }
    for (auto& writer : writers) writer.join();
    test.Expect(std::all_of(successes.begin(), successes.end(), [](int value) { return value == 1; }),
        "parallel atomic writes should each use an isolated transaction");
    const auto finalValue = ReadFile(concurrentTarget);
    const auto backupValue = ReadFile(concurrentTarget.wstring() + L".bak");
    test.Expect(finalValue.rfind("writer-", 0) == 0, "the concurrent target should contain one complete transaction");
    test.Expect(backupValue == "seed" || backupValue.rfind("writer-", 0) == 0,
        "the concurrent backup should contain one complete previous transaction");
}

void TestMetadataRoundTrip(TestContext& test, const std::filesystem::path& root) {
    FolderRewindFormat::MetadataState state;
    state.lastBackupTime = L"2026-07-11T00:00:00Z";
    state.lastBackupFileName = L"Full-World.7z";
    state.fileStates[L"level.dat"] = {42, L"2026-07-11T00:00:00Z", L"abc"};

    FolderRewindFormat::ChangeRecord record;
    record.archiveFileName = L"Full-World.7z";
    record.backupType = L"Full";
    record.basedOnFullBackup = L"FULL-WORLD.7Z";
    record.previousBackupFileName = L"full-world.7z";
    record.createdAtUtc = state.lastBackupTime;
    record.fullFileList = {L"level.dat"};

    const auto metadata = root / "metadata";
    test.Expect(FolderRewindMetadataStore::Save(metadata, state, record), "metadata save should succeed");

    const auto loaded = FolderRewindMetadataStore::Load(metadata, {record.archiveFileName});
    test.Expect(loaded.stateLoaded, "metadata state should load");
    test.Expect(!loaded.recordLoadFailed, "metadata record should load");
    test.Expect(loaded.state.lastBackupFileName == state.lastBackupFileName, "state archive name should round-trip");
    test.Expect(loaded.records.count(record.archiveFileName) == 1, "record should be indexed by archive name");

    const auto renamedArchive = L"Renamed-World.7z";
    test.Expect(FolderRewindMetadataStore::RewriteRecordArchiveName(
        metadata, record.archiveFileName, renamedArchive),
        "metadata archive rename should succeed");

    FolderRewindFormat::ChangeRecord renamedRecord;
    test.Expect(FolderRewindMetadataStore::LoadRecord(metadata, renamedArchive, renamedRecord),
        "renamed metadata record should load");
    test.Expect(renamedRecord.archiveFileName == renamedArchive
        && renamedRecord.basedOnFullBackup == renamedArchive
        && renamedRecord.previousBackupFileName == renamedArchive,
        "metadata archive references should be rewritten case-insensitively");
}

void TestHistoryRoundTrip(TestContext& test, const std::filesystem::path& root) {
    Config config;
    config.name = "Default";
    config.configId = L"11111111-1111-4111-8111-111111111111";

    HistoryEntry entry;
    entry.configId = config.configId;
    entry.timestamp_str = L"2026-07-11 08:00:00";
    entry.worldPath = L"C:/World";
    entry.worldName = L"World";
    entry.backupFile = L"Full-World.7z";
    entry.backupType = L"Full";

    std::map<int, Config> configs{{1, config}};
    std::map<int, std::vector<HistoryEntry>> history{{1, {entry}}};
    const auto path = root / "history.json";
    test.Expect(FolderRewindHistoryStore::SaveHistoryFile(path, configs, history), "history save should succeed");

    std::map<int, std::vector<HistoryEntry>> loaded;
    test.Expect(FolderRewindHistoryStore::LoadHistoryFile(path, configs, loaded), "history load should succeed");
    const bool hasOneItem = loaded.count(1) == 1 && loaded[1].size() == 1;
    test.Expect(hasOneItem, "one history item should round-trip");
    if (hasOneItem) {
        test.Expect(loaded[1][0].configId == config.configId, "history ConfigId should round-trip");
        test.Expect(loaded[1][0].backupFile == entry.backupFile, "history archive name should round-trip");
    }
}

void TestMigrationCoordinator(TestContext& test, const std::filesystem::path& root) {
    MigrationCoordinator::MigrationPaths paths;
    paths.configFile = root / "profile" / "config.ini";
    paths.historyFile = root / "profile" / "history.json";
    paths.reportFile = root / "profile" / "state" / "migration.json";
    paths.snapshotRoot = root / "profile" / "snapshots";
    MigrationCoordinator::ConfigurePaths(paths);

    MigrationUnitResult first;
    first.unitId = L"startup:test";
    first.status = MigrationStatus::Failed;
    first.message = L"first attempt";
    first.snapshotPath = (paths.snapshotRoot / "transaction").wstring();
    MigrationCoordinator::RecordUnit(first);

    MigrationUnitResult retried = first;
    retried.status = MigrationStatus::Succeeded;
    retried.message = L"retry succeeded";
    retried.snapshotPath.clear();
    MigrationCoordinator::RecordUnit(retried);

    const auto report = MigrationCoordinator::GetMigrationReport();
    test.Expect(report.units.size() == 1, "recording the same migration unit should be idempotent");
    if (report.units.size() == 1) {
        test.Expect(report.units.front().status == MigrationStatus::Succeeded, "a retry should replace the unit status");
        test.Expect(report.units.front().snapshotPath == first.snapshotPath, "a retry should retain the recovery snapshot");
    }
    test.Expect(report.status == MigrationStatus::Succeeded, "the report should recompute its aggregate status");
    test.Expect(ReadFile(paths.reportFile).find("startup:test") != std::string::npos,
        "the coordinator should persist its report through the atomic writer");

    MigrationCoordinator::ConfigurePaths(paths);
    const auto reloaded = MigrationCoordinator::GetMigrationReport();
    test.Expect(reloaded.units.size() == 1 && reloaded.units.front().status == MigrationStatus::Succeeded,
        "the coordinator should reload a valid persisted report idempotently");

    MigrationCoordinator::SetHistoryPersistenceBlocked(true);
    test.Expect(MigrationCoordinator::IsHistoryPersistenceBlocked(), "the coordinator should expose the history write gate");
    MigrationCoordinator::SetHistoryPersistenceBlocked(false);
}

void TestLaunchOptionsAndAppPaths(TestContext& test, const std::filesystem::path& root) {
	LaunchOptions options;
	LaunchOptions rejected;
    std::wstring error;
    const auto profile = std::filesystem::absolute(root / "explicit-profile");
	test.Expect(ParseLaunchOptions(
		{L"MineBackup", L"--data-dir", profile.wstring(), L"--autostart",
			L"--select-config", L"config-id"},
		options, error), "known launch options should parse");
	test.Expect(options.autostart && options.selectConfigId == L"config-id",
		"launch option values should be retained");
	test.Expect(!ParseLaunchOptions(
		{L"MineBackup", L"--run-special", L"special-id"}, rejected, error),
		"removed special-run launch options should be rejected");
    test.Expect(!ParseLaunchOptions({L"MineBackup", L"--data-dir"}, rejected, error),
        "a launch option with a missing value should be rejected");
    test.Expect(!ParseLaunchOptions({L"MineBackup", L"--unknown"}, rejected, error),
        "an unknown launch option should be rejected explicitly");
    test.Expect(ParseLaunchOptions(
        {L"MineBackup", L"--cleanup-legacy-service", L"MineBackupService"},
        rejected, error) && rejected.legacyServiceCleanup == L"MineBackupService",
        "the internal legacy service cleanup option should parse by itself");
    test.Expect(!ParseLaunchOptions(
        {L"MineBackup", L"--cleanup-legacy-service", L"MineBackupService", L"--autostart"},
        rejected, error), "legacy service cleanup must reject profile launch options");
    test.Expect(ParseLaunchOptions({L"MineBackup", L"--service"}, rejected, error)
        && rejected.legacyServiceMode,
        "the deprecated service option should remain recognizable for a controlled refusal");
    test.Expect(!ParseLaunchOptions(
        {L"MineBackup", L"--service", L"--silent-startup"}, rejected, error),
        "the deprecated service option must reject mixed launch modes");

    const auto appDirectory = root / "read-only-app-layout";
    std::filesystem::create_directories(appDirectory);
    const auto executable = appDirectory / "MineBackup.exe";
    AppPaths paths;
    const AppPathRequest explicitPathRequest{options.dataDirectory};
    test.Expect(ResolveAppPaths(explicitPathRequest, executable, paths, error), "an absolute --data-dir should resolve");
    test.Expect(paths.mode == AppPathMode::Explicit, "--data-dir should take precedence over other modes");
    ExpectSamePath(test, paths.ConfigFile(), profile / "config" / "config.ini",
        "the explicit profile should own the config root");
    ExpectSamePath(test, paths.HistoryFile(), profile / "data" / "history.json",
        "the explicit profile should own the data root");
    test.Expect(std::filesystem::is_empty(appDirectory),
        "resolving an explicit profile should not write beside the application");

    const auto previousWorkingDirectory = std::filesystem::current_path();
    const auto unrelatedWorkingDirectory = root / "unrelated-working-directory";
    std::filesystem::create_directories(unrelatedWorkingDirectory);
    std::filesystem::current_path(unrelatedWorkingDirectory);
    AppPaths pathsFromAnotherDirectory;
    const bool resolvedFromAnotherDirectory = ResolveAppPaths(
		explicitPathRequest, executable, pathsFromAnotherDirectory, error);
    std::filesystem::current_path(previousWorkingDirectory);
    test.Expect(resolvedFromAnotherDirectory
        && SamePath(pathsFromAnotherDirectory.ConfigFile(), paths.ConfigFile()),
        "the profile should not depend on the process working directory");

	AppPathRequest invalid;
    invalid.dataDirectory = std::filesystem::path("relative-profile");
    test.Expect(!ResolveAppPaths(invalid, executable, paths, error),
        "a relative explicit profile should fail instead of falling back");
    const auto fileInsteadOfProfile = std::filesystem::absolute(root / "profile-is-a-file");
    test.Expect(AtomicFileWriter::WriteText(fileInsteadOfProfile, "not a directory").success,
        "the invalid explicit profile fixture should be created");
    invalid.dataDirectory = fileInsteadOfProfile;
    test.Expect(!ResolveAppPaths(invalid, executable, paths, error),
        "an unusable explicit profile should fail instead of falling back");

#ifdef _WIN32
    test.Expect(AtomicFileWriter::WriteText(appDirectory / "portable.flag", "").success,
        "the portable marker should be created for the test");
	AppPathRequest portable;
    test.Expect(ResolveAppPaths(portable, executable, paths, error), "the Windows portable profile should resolve");
    test.Expect(paths.mode == AppPathMode::Portable
        && SamePath(paths.configRoot, appDirectory / "MineBackupData" / "config"),
        "portable.flag should select the adjacent MineBackupData layout on Windows");
    std::error_code markerError;
    std::filesystem::remove(appDirectory / "portable.flag", markerError);
    const wchar_t* previousLocalAppDataValue = _wgetenv(L"LOCALAPPDATA");
    const std::wstring previousLocalAppData = previousLocalAppDataValue ? previousLocalAppDataValue : L"";
    const auto localAppData = std::filesystem::absolute(root / "local-app-data");
    _wputenv_s(L"LOCALAPPDATA", localAppData.wstring().c_str());
	AppPathRequest installed;
    test.Expect(ResolveAppPaths(installed, executable, paths, error), "the Windows installed profile should resolve");
    test.Expect(paths.mode == AppPathMode::Installed
        && SamePath(paths.configRoot, localAppData / "MineBackup" / "config"),
        "the Windows installed profile should use LOCALAPPDATA");
    _wputenv_s(L"LOCALAPPDATA", previousLocalAppData.c_str());
#elif !defined(__APPLE__)
    const auto fakeAppImage = appDirectory / "MineBackup.AppImage";
    setenv("APPIMAGE", fakeAppImage.c_str(), 1);
    test.Expect(AtomicFileWriter::WriteText(appDirectory / "portable.flag", "").success,
        "the AppImage portable marker should be created for the test");
	AppPathRequest portable;
    test.Expect(ResolveAppPaths(portable, executable, paths, error), "the AppImage portable profile should resolve");
    test.Expect(paths.mode == AppPathMode::Portable
        && SamePath(paths.configRoot, appDirectory / "MineBackupData" / "config"),
        "portable.flag should select the adjacent MineBackupData layout for AppImage");
    unsetenv("APPIMAGE");

    const auto fakeHome = root / "xdg-home";
    std::filesystem::create_directories(fakeHome);
    setenv("HOME", fakeHome.c_str(), 1);
    setenv("XDG_CONFIG_HOME", "relative-config", 1);
    setenv("XDG_DATA_HOME", "relative-data", 1);
    setenv("XDG_STATE_HOME", "relative-state", 1);
    setenv("XDG_CACHE_HOME", "relative-cache", 1);
    unsetenv("XDG_RUNTIME_DIR");
	AppPathRequest installed;
    test.Expect(ResolveAppPaths(installed, executable, paths, error), "invalid relative XDG roots should fall back safely");
    test.Expect(SamePath(paths.configRoot, fakeHome / ".config" / "MineBackup")
        && SamePath(paths.runtimeRoot, fakeHome / ".local" / "state" / "MineBackup" / "runtime"),
        "invalid XDG and absent runtime roots should use the documented fallbacks");
#endif

    test.Expect(ResolveAppPaths(explicitPathRequest, executable, paths, error) && paths.mode == AppPathMode::Explicit,
        "--data-dir should remain authoritative when a portable marker is present");
}

void TestLegacyServicePolicy(TestContext& test) {
#ifdef _WIN32
    const std::wstring executable = L"C:\\Program Files\\MineBackup\\MineBackup.exe";
#else
    const std::wstring executable = L"/opt/MineBackup/MineBackup.exe";
#endif
    const auto quoted = ParseLegacyServiceImagePath(L"\"" + executable + L"\" --service");
    test.Expect(quoted.valid && quoted.executable.filename() == L"MineBackup.exe",
        "a quoted legacy MineBackup service ImagePath should be accepted");
    test.Expect(ParseLegacyServiceImagePath(executable + L" --SERVICE").valid,
        "the unquoted legacy ImagePath and case-insensitive service flag should be accepted");
    test.Expect(!ParseLegacyServiceImagePath(executable).valid,
        "a service ImagePath without --service must be rejected");
    test.Expect(!ParseLegacyServiceImagePath(executable + L" --service --extra").valid,
        "a service ImagePath with extra arguments must be rejected");
    test.Expect(!ParseLegacyServiceImagePath(executable + L" --service --service").valid,
        "duplicate service arguments must be rejected");
    test.Expect(!ParseLegacyServiceImagePath(L"MineBackup.exe --service").valid,
        "a relative service executable must be rejected");
    test.Expect(!ParseLegacyServiceImagePath(L"\"" + executable + L" --service").valid,
        "an unbalanced quote in the service ImagePath must be rejected");
    std::wstring embeddedNull = executable;
    embeddedNull.push_back(L'\0');
    embeddedNull += L" --service";
    test.Expect(!ParseLegacyServiceImagePath(embeddedNull).valid,
        "an embedded null in the service ImagePath must be rejected");
    test.Expect(!ParseLegacyServiceImagePath(
        std::filesystem::path(executable).parent_path().wstring() + L"\\Other.exe --service").valid,
        "a service ImagePath pointing to another executable must be rejected");
}

void TestSingleInstance(TestContext& test, const std::filesystem::path& root) {
    const auto runtime = root / "single-instance";
#ifndef _WIN32
    const auto longRuntime = runtime / std::string(80, 'a') / std::string(80, 'b');
    test.Expect(longRuntime.string().size() > 160,
        "the Unix single-instance regression fixture should exceed socket pathname limits");
#else
    const auto& longRuntime = runtime;
#endif
    const auto profileOne = (root / "profile-one").wstring();
    const auto profileTwo = (root / "profile-two").wstring();
    std::wstring error;
    {
        SingleInstanceService primary;
        test.Expect(primary.Acquire(profileOne, longRuntime, error) == InstanceAcquireResult::Acquired,
            "the first instance should acquire its profile lock");

        SingleInstanceService secondary;
        test.Expect(secondary.Acquire(profileOne, longRuntime, error) == InstanceAcquireResult::AlreadyRunning,
            "a second instance of the same profile should be rejected");
        test.Expect(secondary.Send({InstanceRequestType::SelectConfig, L"stable-config-id"}, error),
            "the second instance should deliver a bounded IPC request");
        const auto requests = primary.PollRequests(error);
        if (requests.empty() && !error.empty()) {
            std::wcerr << L"[DETAIL] single-instance IPC: " << error << L'\n';
        }
        test.Expect(requests.size() == 1 && requests.front().type == InstanceRequestType::SelectConfig
            && requests.front().stableId == L"stable-config-id", "the primary instance should decode the IPC request");

		InstanceControlResponse clientResponse;
		std::wstring clientError;
		bool exchanged = false;
		std::jthread client([&] {
			exchanged = secondary.Exchange({
				"request-1", InstanceControlRequestType::Execute,
				{L"--json", L"世界"}, {}},
				clientResponse, clientError, std::chrono::seconds(5));
		});
		std::vector<InstanceControlExchange> controls;
		const auto controlDeadline = std::chrono::steady_clock::now()
			+ std::chrono::seconds(2);
		while (controls.empty() && std::chrono::steady_clock::now() < controlDeadline) {
			controls = primary.PollControlRequests(error);
			if (controls.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		test.Expect(controls.size() == 1
				&& controls.front().request.requestId == "request-1"
				&& controls.front().request.arguments == std::vector<std::wstring>{L"--json", L"世界"},
			"protocol v2 should preserve request identity and Unicode argument vectors");
		if (!controls.empty()) {
			InstanceControlResponse serverResponse;
			serverResponse.requestId = controls.front().request.requestId;
			serverResponse.accepted = true;
			serverResponse.role = InstanceRuntimeRole::Serve;
			serverResponse.capabilities = {"execute", "cancel", "status", "stop"};
			serverResponse.operationId = "operation-1";
			serverResponse.exitCode = 0;
			serverResponse.payload = R"({"schemaVersion":1,"code":"success"})";
			test.Expect(primary.Reply(
				controls.front().connectionId, serverResponse, error),
				"protocol v2 server should send a final response on the accepted connection");
		}
		client.join();
		if (!exchanged && !clientError.empty()) {
			std::wcerr << L"[DETAIL] control IPC: " << clientError << L'\n';
		}
		test.Expect(exchanged && clientResponse.accepted
				&& clientResponse.role == InstanceRuntimeRole::Serve
				&& clientResponse.operationId == "operation-1"
				&& clientResponse.exitCode == 0,
			"protocol v2 client should receive role, capabilities, operationId and final exit code");

		InstanceControlResponse largeClientResponse;
		std::wstring largeClientError;
		bool largeExchanged = false;
		std::jthread largeClient([&] {
			largeExchanged = secondary.Exchange({
				"request-large", InstanceControlRequestType::Execute,
				{L"--json"}, {}}, largeClientResponse,
				largeClientError, std::chrono::seconds(5));
		});
		controls.clear();
		const auto largeDeadline = std::chrono::steady_clock::now()
			+ std::chrono::seconds(2);
		while (controls.empty() && std::chrono::steady_clock::now() < largeDeadline) {
			controls = primary.PollControlRequests(error);
			if (controls.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		test.Expect(controls.size() == 1,
			"protocol v2 should accept a response larger than the Unix socket buffer");
		if (!controls.empty()) {
			InstanceControlResponse largeResponse;
			largeResponse.requestId = controls.front().request.requestId;
			largeResponse.accepted = true;
			largeResponse.role = InstanceRuntimeRole::Serve;
			largeResponse.capabilities = {"execute"};
			largeResponse.payload = std::string(3u * 1024u * 1024u, 'x');
			test.Expect(primary.Reply(
				controls.front().connectionId, largeResponse, error),
				"protocol v2 should send a multi-megabyte response completely");
		}
		largeClient.join();
		if (!largeExchanged && !largeClientError.empty()) {
			std::wcerr << L"[DETAIL] large control IPC: " << largeClientError << L'\n';
		}
		test.Expect(largeExchanged && largeClientResponse.accepted
				&& largeClientResponse.payload.size() == 3u * 1024u * 1024u,
			"protocol v2 client should receive the complete multi-megabyte response");

		InstanceControlResponse oversizedClientResponse;
		std::wstring oversizedClientError;
		bool oversizedExchanged = false;
		std::jthread oversizedClient([&] {
			oversizedExchanged = secondary.Exchange({
				"request-oversized", InstanceControlRequestType::Execute,
				{L"--json"}, {}}, oversizedClientResponse,
				oversizedClientError, std::chrono::seconds(2));
		});
		controls.clear();
		const auto oversizedDeadline = std::chrono::steady_clock::now()
			+ std::chrono::seconds(2);
		while (controls.empty() && std::chrono::steady_clock::now() < oversizedDeadline) {
			controls = primary.PollControlRequests(error);
			if (controls.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		test.Expect(controls.size() == 1,
			"protocol v2 should retain an accepted connection for oversized responses");
		if (!controls.empty()) {
			InstanceControlResponse oversizedResponse;
			oversizedResponse.requestId = controls.front().request.requestId;
			oversizedResponse.accepted = true;
			oversizedResponse.role = InstanceRuntimeRole::Serve;
			oversizedResponse.capabilities = {"execute"};
			oversizedResponse.payload = std::string(9u * 1024u * 1024u, 'x');
			const bool replyResult = primary.Reply(
				controls.front().connectionId, oversizedResponse, error);
			test.Expect(!replyResult,
				"oversized protocol responses should report a bounded reply failure");
		}
		oversizedClient.join();
		test.Expect(oversizedExchanged && !oversizedClientResponse.accepted
				&& oversizedClientResponse.error == "instance_response_too_large",
			"oversized protocol responses should reach the client as a compact error");

        const std::wstring oversizedId(70u * 1024u, L'x');
        test.Expect(!secondary.Send({InstanceRequestType::SelectConfig, oversizedId}, error),
            "instance IPC should reject payloads above the protocol limit");

        SingleInstanceService otherProfile;
        test.Expect(otherProfile.Acquire(profileTwo, runtime, error) == InstanceAcquireResult::Acquired,
            "a different profile should acquire an independent lock");
    }
    std::error_code cleanupError;
    std::filesystem::remove_all(runtime, cleanupError);
    test.Expect(!cleanupError, "the single-instance test runtime should be removable");
}

void TestLegacyLocationDiscovery(TestContext& test, const std::filesystem::path& root) {
    const auto targetConfig = root / "profile" / "config" / "config.ini";
    const auto targetHistory = root / "profile" / "data" / "history.json";
    const auto legacy = root / "legacy";
    std::filesystem::create_directories(legacy);
    std::ofstream(legacy / "config.ini") << "[General]\n";
    std::ofstream(legacy / "history.json") << "[]\n";

    auto result = DiscoverLegacyLocations(targetConfig, targetHistory, {
        {legacy, LegacyLocationOrigin::ExecutableDirectory},
        {legacy / ".", LegacyLocationOrigin::OriginalWorkingDirectory},
        {targetConfig.parent_path(), LegacyLocationOrigin::KnownPlatformLocation}
    });
    test.Expect(!result.targetInitialized, "an empty 1.16 destination should be reported as uninitialized");
    test.Expect(result.candidates.size() == 1 && result.candidates.front().origins.size() == 2,
        "legacy source aliases should be normalized and deduplicated");
    test.Expect(!result.candidates.front().configFile.empty() && !result.candidates.front().historyFile.empty(),
        "a legacy candidate should report its available data units");

    std::filesystem::create_directories(targetConfig.parent_path());
    std::ofstream(targetConfig) << "[General]\n";
    result = DiscoverLegacyLocations(targetConfig, targetHistory, {
        {legacy, LegacyLocationOrigin::ExecutableDirectory},
        {targetConfig.parent_path(), LegacyLocationOrigin::KnownPlatformLocation}
    });
    test.Expect(result.targetInitialized, "an existing target config should forbid automatic startup merging");
    test.Expect(result.candidates.size() == 1, "the current target must not be rediscovered as a legacy source");
}

void TestLegacyLocationMigration(TestContext& test, const std::filesystem::path& root) {
    const auto sourceRoot = root / "legacy-import";
    const auto targetConfig = root / "imported-profile" / "config" / "config.ini";
    const auto targetHistory = root / "imported-profile" / "data" / "history.json";
    std::filesystem::create_directories(sourceRoot);
    const std::string sourceConfig = "[General]\nLanguage=en_US\n";
    const std::string sourceHistory = "[]\n";
    std::ofstream(sourceRoot / "config.ini", std::ios::binary) << sourceConfig;
    std::ofstream(sourceRoot / "history.json", std::ios::binary) << sourceHistory;
    LegacyLocationCandidate source{sourceRoot, sourceRoot / "config.ini", sourceRoot / "history.json", {}};

    auto result = ImportLegacyLocation(source, targetConfig, targetHistory);
    test.Expect(result.success, "a selected legacy location should import transactionally into an empty profile");
    test.Expect(ReadText(targetConfig) == sourceConfig && ReadText(targetHistory) == sourceHistory,
        "startup location migration should preserve source bytes");
    test.Expect(ReadText(source.configFile) == sourceConfig && ReadText(source.historyFile) == sourceHistory,
        "startup location migration must not move, rename, or delete legacy files");
    test.Expect(!ImportLegacyLocation(source, targetConfig, targetHistory).success,
        "startup location migration should refuse to merge into an initialized target");

    const auto invalidRoot = root / "invalid-legacy-import";
    std::filesystem::create_directories(invalidRoot);
    std::ofstream(invalidRoot / "config.ini") << "[General]\n";
    std::ofstream(invalidRoot / "history.json") << "not-json";
    const auto rejectedConfig = root / "rejected-profile" / "config" / "config.ini";
    const auto rejectedHistory = root / "rejected-profile" / "data" / "history.json";
    result = ImportLegacyLocation({invalidRoot, invalidRoot / "config.ini", invalidRoot / "history.json", {}},
        rejectedConfig, rejectedHistory);
    test.Expect(!result.success && !std::filesystem::exists(rejectedConfig) && !std::filesystem::exists(rejectedHistory),
        "invalid legacy history should leave the destination untouched");
}

} // namespace

void RunStorageMigrationTests(TestContext& test, const std::filesystem::path& root) {
    TestFormat(test);
    TestUnicodeFilesystemPath(test, root);
    TestAtomicWriter(test, root);
    TestMetadataRoundTrip(test, root);
    TestHistoryRoundTrip(test, root);
    TestMigrationCoordinator(test, root);
    TestLaunchOptionsAndAppPaths(test, root);
    TestLegacyServicePolicy(test);
    TestSingleInstance(test, root);
    TestLegacyLocationDiscovery(test, root);
    TestLegacyLocationMigration(test, root);
}
