#include "AtomicFileWriter.h"
#include "AppPaths.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "FolderRewindMetadataStore.h"
#include "MigrationCoordinator.h"
#include "RotatingFileLog.h"
#include "SingleInstanceService.h"
#include "LegacyLocationDiscovery.h"
#include "LegacyLocationMigration.h"
#include "ProcessRunner.h"
#include "TaskCoordinator.h"
#include "InterruptedTaskRecovery.h"
#include "NetworkService.h"
#include "RemoteContentService.h"
#include "Sha256.h"
#include "ExternalToolManager.h"
#include "PortableConfigDocument.h"
#include "DesktopServices.h"
#include "SpecialConfigPolicy.h"
#include "LegacyServicePolicy.h"
#include "text_to_text.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {

struct TestContext {
    int failures = 0;

    void Expect(bool condition, const char* message) {
        if (condition) return;
        ++failures;
        std::cerr << "[FAIL] " << message << '\n';
    }
};

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("MineBackupDataTests-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

class FakeNetworkBackend final : public NetworkBackend {
public:
    NetworkResult configuredResult{NetworkStatus::Succeeded, 200, "https://example.test/final", 0, {}};
    std::string body;
    std::size_t syntheticChunkSize = 0;

    NetworkResult Get(const NetworkRequest&, const NetworkChunkSink& sink, std::stop_token stopToken) override {
        if (stopToken.stop_requested()) {
            auto cancelled = configuredResult;
            cancelled.status = NetworkStatus::Cancelled;
            return cancelled;
        }
        if (configuredResult.status != NetworkStatus::Succeeded) return configuredResult;
        const std::size_t size = syntheticChunkSize ? syntheticChunkSize : body.size();
        if (size && !sink(body.empty() ? "x" : body.data(), size)) {
            auto rejected = configuredResult;
            rejected.status = NetworkStatus::SinkRejected;
            return rejected;
        }
        auto result = configuredResult;
        result.transferredBytes = size;
        return result;
    }
};

class MockDesktopServices final : public DesktopServices {
public:
    PlatformCapabilities capabilities{
        CapabilityStatus::Ready(), CapabilityStatus::Ready(),
        CapabilityStatus::Unavailable(L"notifications disabled by test"),
        CapabilityStatus::PermissionRequired(L"tray permission required"),
        CapabilityStatus::Unavailable(L"hotkeys disabled by test"),
        CapabilityStatus::Ready(), CapabilityStatus::Ready()};
    std::filesystem::path selectedPath = L"mock/selected.txt";
    bool autostartEnabled = false;
    int autostartSettingsOpenCount = 0;
    int activationCount = 0;
    std::vector<GlobalHotkeyBinding> configuredHotkeys;

    PlatformCapabilities Capabilities() const override { return capabilities; }
    void SetNativeWindow(void* nativeWindow) override { window = nativeWindow; }
    DesktopPathResult SelectFile() override {
        return {capabilities.fileDialogs, selectedPath, false};
    }
    DesktopPathResult SelectFolder() override {
        return {capabilities.fileDialogs, selectedPath.parent_path(), false};
    }
    DesktopPathResult SelectSaveFile(const std::wstring&, const std::wstring&) override {
        return {capabilities.fileDialogs, selectedPath, false};
    }
    CapabilityStatus OpenUri(const std::wstring&) override { return capabilities.openUri; }
    CapabilityStatus OpenFolder(const std::filesystem::path&) override { return capabilities.openUri; }
    CapabilityStatus RevealInFolder(
        const std::filesystem::path&, const std::filesystem::path&) override {
        return capabilities.openUri;
    }
    CapabilityStatus Notify(const std::wstring&, const std::wstring&) override {
        return capabilities.notifications;
    }
    CapabilityStatus SetTrayVisible(bool) override { return capabilities.tray; }
    CapabilityStatus ConfigureGlobalHotkeys(
        const std::vector<GlobalHotkeyBinding>& bindings) override {
        configuredHotkeys = bindings;
        return capabilities.globalHotkeys;
    }
    CapabilityStatus SetAutostart(bool enabled) override {
        autostartEnabled = enabled;
        return capabilities.autostart;
    }
    CapabilityStatus OpenAutostartSettings() override {
        ++autostartSettingsOpenCount;
        return capabilities.autostart;
    }
    CapabilityStatus ActivateWindow() override {
        ++activationCount;
        return capabilities.windowActivation;
    }
    CapabilityStatus RestartApplication() override { return capabilities.windowActivation; }

    void* window = nullptr;
};

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

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

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void TestAtomicWriter(TestContext& test, const std::filesystem::path& root) {
    const auto target = root / "atomic" / "value.txt";
    test.Expect(AtomicFileWriter::WriteText(target, "first").success, "first atomic write should succeed");
    test.Expect(AtomicFileWriter::WriteText(target, "second").success, "replacement atomic write should succeed");
    test.Expect(ReadFile(target) == "second", "atomic target should contain the replacement");
    test.Expect(ReadFile(target.wstring() + L".bak") == "first", "atomic backup should contain the previous value");

    int temporaryFiles = 0;
    for (const auto& entry : std::filesystem::directory_iterator(target.parent_path())) {
        if (entry.path().filename().wstring().find(L".tmp.") != std::wstring::npos) ++temporaryFiles;
    }
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
    record.createdAtUtc = state.lastBackupTime;
    record.fullFileList = {L"level.dat"};

    const auto metadata = root / "metadata";
    test.Expect(FolderRewindMetadataStore::Save(metadata, state, record), "metadata save should succeed");

    const auto loaded = FolderRewindMetadataStore::Load(metadata, {record.archiveFileName});
    test.Expect(loaded.stateLoaded, "metadata state should load");
    test.Expect(!loaded.recordLoadFailed, "metadata record should load");
    test.Expect(loaded.state.lastBackupFileName == state.lastBackupFileName, "state archive name should round-trip");
    test.Expect(loaded.records.count(record.archiveFileName) == 1, "record should be indexed by archive name");
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

void TestRotatingLog(TestContext& test, const std::filesystem::path& root) {
    const auto path = root / "logs" / "minebackup.log";
    test.Expect(RotatingFileLog::Append(path, std::string(45, 'a'), 16, 3),
        "a large log append should rotate across bounded files");
    test.Expect(std::filesystem::exists(path), "the active rotated log should exist");
    test.Expect(std::filesystem::exists(path.wstring() + L".1"), "the first archived log should exist");
    test.Expect(std::filesystem::exists(path.wstring() + L".2"), "the final retained archived log should exist");
    test.Expect(!std::filesystem::exists(path.wstring() + L".3"), "the log file count should stay bounded");
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        test.Expect(entry.file_size() <= 16, "each rotated log file should stay under its size limit");
    }
}

void TestLaunchOptionsAndAppPaths(TestContext& test, const std::filesystem::path& root) {
    LaunchOptions options;
    std::wstring error;
    const auto profile = std::filesystem::absolute(root / "explicit-profile");
    test.Expect(ParseLaunchOptions(
        {L"MineBackup", L"--data-dir", profile.wstring(), L"--autostart",
            L"--select-config", L"config-id", L"--run-special", L"special-id"},
        options, error), "known launch options should parse");
    test.Expect(options.autostart && options.selectConfigId == L"config-id"
        && options.runSpecialId == L"special-id", "launch option values should be retained");
    LaunchOptions rejected;
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
    test.Expect(ResolveAppPaths(options, executable, paths, error), "an absolute --data-dir should resolve");
    test.Expect(paths.mode == AppPathMode::Explicit, "--data-dir should take precedence over other modes");
    test.Expect(paths.ConfigFile() == profile / "config" / "config.ini",
        "the explicit profile should own the config root");
    test.Expect(paths.HistoryFile() == profile / "data" / "history.json",
        "the explicit profile should own the data root");
    test.Expect(std::filesystem::is_empty(appDirectory),
        "resolving an explicit profile should not write beside the application");

    const auto previousWorkingDirectory = std::filesystem::current_path();
    const auto unrelatedWorkingDirectory = root / "unrelated-working-directory";
    std::filesystem::create_directories(unrelatedWorkingDirectory);
    std::filesystem::current_path(unrelatedWorkingDirectory);
    AppPaths pathsFromAnotherDirectory;
    const bool resolvedFromAnotherDirectory = ResolveAppPaths(options, executable, pathsFromAnotherDirectory, error);
    std::filesystem::current_path(previousWorkingDirectory);
    test.Expect(resolvedFromAnotherDirectory && pathsFromAnotherDirectory.ConfigFile() == paths.ConfigFile(),
        "the profile should not depend on the process working directory");

    LaunchOptions invalid;
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
    LaunchOptions portable;
    test.Expect(ResolveAppPaths(portable, executable, paths, error), "the Windows portable profile should resolve");
    test.Expect(paths.mode == AppPathMode::Portable
        && paths.configRoot == appDirectory / "MineBackupData" / "config",
        "portable.flag should select the adjacent MineBackupData layout on Windows");
    std::error_code markerError;
    std::filesystem::remove(appDirectory / "portable.flag", markerError);
    const wchar_t* previousLocalAppDataValue = _wgetenv(L"LOCALAPPDATA");
    const std::wstring previousLocalAppData = previousLocalAppDataValue ? previousLocalAppDataValue : L"";
    const auto localAppData = std::filesystem::absolute(root / "local-app-data");
    _wputenv_s(L"LOCALAPPDATA", localAppData.wstring().c_str());
    LaunchOptions installed;
    test.Expect(ResolveAppPaths(installed, executable, paths, error), "the Windows installed profile should resolve");
    test.Expect(paths.mode == AppPathMode::Installed
        && paths.configRoot == localAppData / "MineBackup" / "config",
        "the Windows installed profile should use LOCALAPPDATA");
    _wputenv_s(L"LOCALAPPDATA", previousLocalAppData.c_str());
#elif !defined(__APPLE__)
    const auto fakeAppImage = appDirectory / "MineBackup.AppImage";
    setenv("APPIMAGE", fakeAppImage.c_str(), 1);
    test.Expect(AtomicFileWriter::WriteText(appDirectory / "portable.flag", "").success,
        "the AppImage portable marker should be created for the test");
    LaunchOptions portable;
    test.Expect(ResolveAppPaths(portable, executable, paths, error), "the AppImage portable profile should resolve");
    test.Expect(paths.mode == AppPathMode::Portable
        && paths.configRoot == appDirectory / "MineBackupData" / "config",
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
    LaunchOptions installed;
    test.Expect(ResolveAppPaths(installed, executable, paths, error), "invalid relative XDG roots should fall back safely");
    test.Expect(paths.configRoot == fakeHome / ".config" / "MineBackup"
        && paths.runtimeRoot == fakeHome / ".local" / "state" / "MineBackup" / "runtime",
        "invalid XDG and absent runtime roots should use the documented fallbacks");
#endif

    test.Expect(ResolveAppPaths(options, executable, paths, error) && paths.mode == AppPathMode::Explicit,
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
    std::wstring error;
    SingleInstanceService primary;
    test.Expect(primary.Acquire(L"profile-one", runtime, error) == InstanceAcquireResult::Acquired,
        "the first instance should acquire its profile lock");

    SingleInstanceService secondary;
    test.Expect(secondary.Acquire(L"profile-one", runtime, error) == InstanceAcquireResult::AlreadyRunning,
        "a second instance of the same profile should be rejected");
    test.Expect(secondary.Send({InstanceRequestType::SelectConfig, L"stable-config-id"}, error),
        "the second instance should deliver a bounded IPC request");
    const auto requests = primary.PollRequests(error);
    if (requests.empty() && !error.empty()) {
        std::wcerr << L"[DETAIL] single-instance IPC: " << error << L'\n';
    }
    test.Expect(requests.size() == 1 && requests.front().type == InstanceRequestType::SelectConfig
        && requests.front().stableId == L"stable-config-id", "the primary instance should decode the IPC request");

    const std::wstring oversizedId(70u * 1024u, L'x');
    test.Expect(!secondary.Send({InstanceRequestType::SelectConfig, oversizedId}, error),
        "instance IPC should reject payloads above the protocol limit");

    SingleInstanceService otherProfile;
    test.Expect(otherProfile.Acquire(L"profile-two", runtime, error) == InstanceAcquireResult::Acquired,
        "a different profile should acquire an independent lock");
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

void TestProcessRunner(TestContext& test, const std::filesystem::path& executable, const std::filesystem::path& root) {
    ProcessSpec echo;
    echo.executable = executable;
    echo.arguments = {L"--process-helper-echo", L"space value", L"\u4e16\u754c", L"$()", L"`value`", L"semi;colon", L"line\nbreak"};
    auto result = ProcessRunner::Run(echo);
    if (result.status != ProcessStatus::Succeeded) {
        std::wcerr << L"[DETAIL] ProcessRunner echo: status=" << static_cast<int>(result.status)
            << L", exit=" << result.exitCode << L", error=" << result.error << L'\n';
        std::cerr << "[DETAIL] stderr=" << result.standardError << " stdout=" << result.standardOutput << '\n';
    }
    test.Expect(result.status == ProcessStatus::Succeeded,
        "ProcessRunner should execute an argument-vector process without a shell");
    const std::string expectedArguments = "space value\n\xE4\xB8\x96\xE7\x95\x8C\n$()\n`value`\nsemi;colon\nline\nbreak\n";
    if (result.standardOutput != expectedArguments) {
        std::cerr << "[DETAIL] argument output size=" << result.standardOutput.size()
            << " expected=" << expectedArguments.size() << " value=" << result.standardOutput << '\n';
    }
    test.Expect(result.standardOutput == expectedArguments,
        "ProcessRunner should preserve spaces, Unicode, and shell metacharacters as literal arguments");

    ProcessSpec output;
    output.executable = executable;
    output.arguments = {L"--process-helper-output"};
    output.maximumCapturedBytes = 1024;
    result = ProcessRunner::Run(output);
    test.Expect(result.status == ProcessStatus::Succeeded && result.outputTruncated
        && result.standardOutput.size() == 1024 && result.standardError.size() == 1024,
        "ProcessRunner should bound both output streams and report truncation");

	ProcessSpec workingDirectory;
	workingDirectory.executable = executable;
	workingDirectory.arguments = {L"--process-helper-cwd"};
	workingDirectory.workingDirectory = root;
	result = ProcessRunner::Run(workingDirectory);
	while (!result.standardOutput.empty() && (result.standardOutput.back() == '\r' || result.standardOutput.back() == '\n')) {
		result.standardOutput.pop_back();
	}
	test.Expect(result.status == ProcessStatus::Succeeded
		&& std::filesystem::equivalent(std::filesystem::path(utf8_to_wstring(result.standardOutput)), root),
		"ProcessRunner should apply a working directory without mutating the parent process");

	ShellTaskSpec shell;
#ifdef _WIN32
	shell.command = L"echo shell-task";
#else
	shell.command = L"printf shell-task";
#endif
	result = ProcessRunner::RunShellTask(shell);
	test.Expect(result.status == ProcessStatus::Succeeded && result.standardOutput.find("shell-task") != std::string::npos,
		"ShellTaskSpec should be the explicit raw-command execution boundary");

    const auto marker = root / "process-grandchild-marker";
    ProcessSpec timeout;
    timeout.executable = executable;
    timeout.arguments = {L"--process-helper-spawn", marker.wstring()};
    timeout.timeout = std::chrono::milliseconds(150);
    result = ProcessRunner::Run(timeout);
    test.Expect(result.status == ProcessStatus::TimedOut, "ProcessRunner should distinguish timeout from process failure");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    test.Expect(!std::filesystem::exists(marker), "timing out a process should terminate its complete descendant tree");

    ProcessSpec cancelled = timeout;
    cancelled.timeout = std::chrono::seconds(10);
    std::stop_source cancellation;
    std::jthread requestStop([&](std::stop_token) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancellation.request_stop();
    });
    result = ProcessRunner::Run(cancelled, cancellation.get_token());
    test.Expect(result.status == ProcessStatus::Cancelled, "ProcessRunner should distinguish cooperative cancellation");
}

void TestSevenZipArgumentVector(TestContext& test, const std::filesystem::path& root) {
#ifdef _WIN32
    const auto sevenZip = std::filesystem::path(__FILE__).parent_path().parent_path()
        / "MineBackup" / "Assets" / "7za.exe";
    const auto source = root / "7z source $();";
    const auto restored = root / "7z restored";
    const auto archive = root / "argument-vector.7z";
    std::filesystem::create_directories(source);
    std::ofstream(source / "literal $(); file.txt") << "literal-content";
    ProcessSpec create;
    create.executable = sevenZip;
    create.arguments = {L"a", L"-t7z", L"-m0=LZMA2", L"-mx=1", L"-mmt", L"-ssw", archive.wstring(), L"*"};
    create.workingDirectory = source;
    auto result = ProcessRunner::Run(create);
    test.Expect(result.status == ProcessStatus::Succeeded && std::filesystem::exists(archive),
        "7-Zip should create an archive through a literal argument vector");

    ProcessSpec extract;
    extract.executable = sevenZip;
    extract.arguments = {L"x", archive.wstring(), L"-o" + restored.wstring(), L"-y"};
    result = ProcessRunner::Run(extract);
    test.Expect(result.status == ProcessStatus::Succeeded
        && ReadText(restored / "literal $(); file.txt") == "literal-content",
        "7-Zip restore should preserve shell metacharacters as filename data");
#else
    (void)test;
    (void)root;
#endif
}

void TestExternalToolManager(TestContext& test, const std::filesystem::path& root, const std::filesystem::path& testExecutable) {
#ifdef _WIN32
    const auto asset = std::filesystem::path(__FILE__).parent_path().parent_path()
        / "MineBackup" / "Assets" / "7za.exe";
    const auto manifestPath = asset.parent_path() / "tool-manifest.json";
    const std::string manifestText = ReadFile(manifestPath);
    test.Expect(manifestText.find(ExternalToolManager::SevenZipWindowsSha256) != std::string::npos
        && manifestText.find("1.74.4") != std::string::npos
        && manifestText.find("ef097ef9de37a57feb7d9f9c7afb34148ad3c65be8025f1d8f7f521554a701ea") != std::string::npos,
        "the auditable tool manifest should match compiled Windows pins");
    std::string assetHash;
    std::wstring hashError;
    test.Expect(Sha256::FileHex(asset, assetHash, hashError)
        && assetHash == ExternalToolManager::SevenZipWindowsSha256,
        "the embedded 7-Zip asset should match the pinned supply-chain hash");

    AppPaths paths;
    paths.toolsRoot = root / "managed-tools";
    paths.resourcesRoot = root / "resources";
    const std::string assetBytes = ReadFile(asset);
    const auto install = ExternalToolManager::InstallBundledSevenZipForWindows(
        assetBytes.data(), assetBytes.size(), paths);
    test.Expect(install.success
        && install.executable == paths.toolsRoot / L"7zip" / ExternalToolManager::SevenZipVersion / L"7za.exe",
        "the embedded 7-Zip should install into an immutable version directory");

    const auto sevenZipProbe = ExternalToolManager::ProbeSevenZip(install.executable);
    test.Expect(sevenZipProbe.available,
        "the pinned 7-Zip should expose 7z, ZIP, LZMA2, Deflate, BZip2 and zstd capabilities");
    const auto sevenZipFallback = ExternalToolManager::ResolveSevenZip(root / "missing-custom-7z.exe", paths);
    test.Expect(sevenZipFallback.available && sevenZipFallback.fellBackFromUserPath
        && sevenZipFallback.source == ExternalToolSource::Bundled
        && sevenZipFallback.executable == install.executable,
        "an invalid custom 7-Zip path should visibly fall back to the verified bundle");

    const auto source = root / "codec-source";
    std::filesystem::create_directories(source);
    std::ofstream(source / "payload.txt") << "codec-round-trip";
    for (const auto& [codec, archiveName] : std::vector<std::pair<std::wstring, std::wstring>>{
        {L"LZMA2", L"lzma2.7z"}, {L"zstd", L"zstd.7z"}}) {
        const auto archive = root / archiveName;
        const auto restored = root / (archiveName + L"-restored");
        ProcessSpec create;
        create.executable = install.executable;
        create.arguments = {L"a", L"-t7z", L"-m0=" + codec, L"-mx=1", archive.wstring(), L"payload.txt"};
        create.workingDirectory = source;
        auto process = ProcessRunner::Run(create);
        test.Expect(process.status == ProcessStatus::Succeeded,
            codec == L"LZMA2" ? "the pinned tool should create an LZMA2 archive" : "the pinned tool should create a zstd archive");
        ProcessSpec extract;
        extract.executable = install.executable;
        extract.arguments = {L"x", archive.wstring(), L"-o" + restored.wstring(), L"-y"};
        process = ProcessRunner::Run(extract);
        test.Expect(process.status == ProcessStatus::Succeeded
            && ReadText(restored / "payload.txt") == "codec-round-trip",
            codec == L"LZMA2" ? "the pinned tool should restore an LZMA2 archive" : "the pinned tool should restore a zstd archive");
    }

    const auto userRclone = root / "user-rclone.exe";
    const auto managedRclone = paths.toolsRoot / L"rclone" / L"versions"
        / ExternalToolManager::RcloneVersion / L"rclone.exe";
    std::filesystem::create_directories(managedRclone.parent_path());
    std::filesystem::copy_file(testExecutable, userRclone);
    std::filesystem::copy_file(testExecutable, managedRclone);
    auto rclone = ExternalToolManager::ResolveRclone(userRclone, paths);
    test.Expect(rclone.available && rclone.source == ExternalToolSource::User
        && rclone.executable == userRclone,
        "an absolute verified user rclone should take priority over a managed version");
    rclone = ExternalToolManager::ResolveRclone(root / "missing-rclone.exe", paths);
    test.Expect(rclone.available && rclone.source == ExternalToolSource::Managed
        && rclone.fellBackFromUserPath && rclone.executable == managedRclone,
        "an invalid user rclone should visibly fall back to the managed pinned version");

    std::filesystem::remove_all(managedRclone.parent_path());
    const auto previousVersion = paths.toolsRoot / "rclone" / "versions" / "1.70.0" / "sentinel.txt";
    std::filesystem::create_directories(previousVersion.parent_path());
    std::ofstream(previousVersion) << "current-version-remains";
    auto backend = std::make_shared<FakeNetworkBackend>();
    backend->body = "not-the-pinned-rclone-archive";
    NetworkService network(backend);
    auto failedInstall = ExternalToolManager::InstallPinnedRclone(network, install.executable, paths);
    test.Expect(!failedInstall.success && ReadText(previousVersion) == "current-version-remains",
        "a wrong rclone archive hash should not change an existing managed version");
    backend->configuredResult.status = NetworkStatus::Truncated;
    failedInstall = ExternalToolManager::InstallPinnedRclone(network, install.executable, paths);
    test.Expect(!failedInstall.success && ReadText(previousVersion) == "current-version-remains",
        "a truncated rclone download should not change an existing managed version");
    int stagingDirectories = 0;
    for (const auto& entry : std::filesystem::directory_iterator(paths.toolsRoot / "rclone")) {
        if (entry.path().filename().wstring().find(L".staging-") == 0) ++stagingDirectories;
    }
    test.Expect(stagingDirectories == 0,
        "failed rclone installations should remove their private staging directories");
#else
    (void)test;
    (void)root;
    (void)testExecutable;
#endif
}

void TestPortableConfigDocument(TestContext& test) {
    Config localConfig;
    localConfig.configId = L"11111111-1111-4111-8111-111111111111";
    localConfig.name = "Local profile";
    localConfig.worlds = {{L"World One", L"Description"}};
    localConfig.saveRoot = L"C:\\secret\\saves";
    localConfig.backupPath = L"D:\\private\\backups";
    localConfig.zipPath = L"C:\\tools\\7za.exe";
    localConfig.rclonePath = L"C:\\tools\\rclone.exe";
    localConfig.rcloneRemotePath = L"credential-alias:private";
    localConfig.cloudWorkingDirectory = L"C:\\private\\cwd";
    localConfig.zipMethod = L"zstd";
    localConfig.zipLevel = 17;
    localConfig.blacklist = {L"session.lock", L"cache/*"};
    localConfig.cloudSyncEnabled = true;
    localConfig.cloudSyncMode = 1;
    localConfig.backupOnGameStart = true;

    Config localOnly;
    localOnly.configId = L"22222222-2222-4222-8222-222222222222";
    localOnly.name = "Local only";
    localOnly.worlds = {{L"LocalWorld", L""}};

    std::map<int, Config> local{{0, localConfig}, {1, localOnly}};
    const auto localDocument = PortableConfigDocument::FromLocalConfigs(local);
    const std::string serialized = localDocument.Serialize();
    test.Expect(serialized.find("C:\\\\secret") == std::string::npos
        && serialized.find("D:\\\\private") == std::string::npos
        && serialized.find("rclone.exe") == std::string::npos
        && serialized.find("credential-alias") == std::string::npos,
        "portable configuration should exclude paths, tools and credential-bearing remote bindings");
    test.Expect(serialized.find("backupOnGameStart") == std::string::npos
        && serialized.find("commands") == std::string::npos,
        "portable configuration should exclude automation, commands and scripts");

    PortableConfigDocument parsed;
    std::wstring error;
    test.Expect(PortableConfigDocument::Parse(serialized, parsed, error)
        && parsed.configs.at(localConfig.configId).zipMethod == L"zstd",
        "portable configuration should round-trip its explicit whitelist");
    PortableConfigDocument invalid;
    test.Expect(!PortableConfigDocument::Parse(R"({"schemaVersion":99,"configs":{}})", invalid, error),
        "an unknown portable configuration schema should be rejected");
    test.Expect(!PortableConfigDocument::Parse(R"({"schemaVersion":1,"configs":{"not-a-uuid":{}}})", invalid, error),
        "portable configuration should reject a non-canonical ConfigId");
    test.Expect(!PortableConfigDocument::Parse("{broken", invalid, error),
        "damaged portable configuration JSON should be rejected");
    test.Expect(!PortableConfigDocument::Parse(std::string(PortableConfigDocument::MaximumBytes + 1, 'x'), invalid, error),
        "oversized portable configuration JSON should be rejected before parsing");

    std::string injected = serialized;
    const std::string nameField = "\"name\": \"Local profile\",";
    const auto namePosition = injected.find(nameField);
    if (namePosition != std::string::npos) {
        injected.insert(namePosition + nameField.size(), "\n      \"saveRoot\": \"C:/attacker/path\",");
    }
    PortableConfigDocument injectedDocument;
    test.Expect(PortableConfigDocument::Parse(injected, injectedDocument, error),
        "unknown non-whitelisted fields should be ignored rather than adopted");

    Config remoteSame = localConfig;
    remoteSame.name = "Remote old value";
    remoteSame.zipLevel = 1;
    Config remoteOnly;
    remoteOnly.configId = L"33333333-3333-4333-8333-333333333333";
    remoteOnly.name = "Remote only";
    remoteOnly.worlds = {{L"RemoteWorld", L"remote"}};
    const auto remoteDocument = PortableConfigDocument::FromLocalConfigs({{0, remoteSame}, {1, remoteOnly}});

    PortableConfigMergePreview uploadPreview;
    const auto uploaded = PortableConfigDocument::MergeForUpload(local, remoteDocument, uploadPreview);
    test.Expect(uploaded.configs.at(localConfig.configId).name == localConfig.name
        && uploaded.configs.at(remoteOnly.configId).name == remoteOnly.name,
        "upload merge should let local portable fields win while preserving remote-only ConfigIds");
    test.Expect(uploadPreview.added.size() == 1 && uploadPreview.updated.size() == 1
        && uploadPreview.preserved.size() == 1 && !uploadPreview.excludedFields.empty(),
        "upload preview should report added, updated, preserved and excluded fields");
    test.Expect(std::find(uploadPreview.excludedFields.begin(), uploadPreview.excludedFields.end(),
            L"useServiceMode") != uploadPreview.excludedFields.end()
        && std::find(uploadPreview.excludedFields.begin(), uploadPreview.excludedFields.end(),
            L"serviceName") != uploadPreview.excludedFields.end(),
        "portable configuration previews should explicitly exclude legacy Service Mode fields");

    auto importTarget = local;
    const auto beforePreview = importTarget;
    const auto importPreview = PortableConfigDocument::PreviewImport(importTarget, remoteDocument);
    test.Expect(importTarget.at(0).name == beforePreview.at(0).name
        && importPreview.added.size() == 1 && importPreview.updated.size() == 1
        && importPreview.preserved.size() == 1,
        "previewing an import should not mutate local data");
    PortableConfigMergePreview appliedPreview;
    test.Expect(PortableConfigDocument::ApplyImport(importTarget, remoteDocument, appliedPreview, error),
        "confirmed portable import should apply successfully");
    test.Expect(importTarget.at(0).name == remoteSame.name && importTarget.at(0).zipLevel == remoteSame.zipLevel
        && importTarget.at(0).saveRoot == localConfig.saveRoot
        && importTarget.at(0).backupPath == localConfig.backupPath
        && importTarget.at(0).rclonePath == localConfig.rclonePath,
        "import should let remote portable fields win without overwriting local machine bindings");
    test.Expect(importTarget.at(1).name == localOnly.name,
        "import should preserve local-only ConfigIds");
    const auto imported = std::find_if(importTarget.begin(), importTarget.end(), [&](const auto& item) {
        return item.second.configId == remoteOnly.configId;
    });
    test.Expect(imported != importTarget.end() && imported->second.pendingLocalBinding
        && imported->second.saveRoot.empty() && imported->second.backupPath.empty()
        && imported->second.rclonePath.empty() && !imported->second.cloudSyncEnabled,
        "a remote-only ConfigId should import as pending local binding with dangerous actions disabled");
}

void TestTaskCoordinator(TestContext& test, const std::filesystem::path& root) {
    auto& coordinator = TaskCoordinator::Instance();
    std::atomic<int> active{0};
    std::atomic<int> maximumActive{0};
    std::atomic<int> completed{0};
    const auto resource = TaskCoordinator::WorldResourceKey(L"config-id", root / "world");
    test.Expect(resource == TaskCoordinator::WorldResourceKey(L"config-id", root / "alias" / ".." / "world"),
        "world resource keys should normalize path aliases");

    for (int index = 0; index < 3; ++index) {
        test.Expect(coordinator.Submit(L"serialized test", {resource},
            [&coordinator, &active, &maximumActive, &completed](std::stop_token token) {
                const int nowActive = active.fetch_add(1) + 1;
                int observed = maximumActive.load();
                while (nowActive > observed && !maximumActive.compare_exchange_weak(observed, nowActive)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                active.fetch_sub(1);
                completed.fetch_add(1);
                coordinator.PostEvent({L"test-complete", token.stop_possible() ? L"stoppable" : L"unstoppable"});
            }), "TaskCoordinator should accept work before shutdown");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::vector<TaskEvent> events;
    while (completed.load() != 3 && std::chrono::steady_clock::now() < deadline) {
        auto current = coordinator.PollEvents();
        events.insert(events.end(), current.begin(), current.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto remaining = coordinator.PollEvents();
    events.insert(events.end(), remaining.begin(), remaining.end());

    test.Expect(completed.load() == 3, "TaskCoordinator should run all accepted work");
    test.Expect(maximumActive.load() == 1, "tasks for one world resource should never overlap");
    test.Expect(events.size() == 3 && std::all_of(events.begin(), events.end(), [](const TaskEvent& event) {
        return event.type == L"test-complete" && event.message == L"stoppable";
    }), "workers should publish immutable events and inherit a stop token");

    std::atomic<bool> cancellationStarted{false};
    std::atomic<bool> cancellationObserved{false};
    test.Expect(coordinator.Submit(L"cancellation test", {},
        [&cancellationStarted, &cancellationObserved](std::stop_token token) {
            cancellationStarted = true;
            while (!token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            cancellationObserved = true;
        }), "TaskCoordinator should accept a cancellable task");
    const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!cancellationStarted.load() && std::chrono::steady_clock::now() < startDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    test.Expect(coordinator.RequestStop(L"cancellation test"), "a named running task should be cancellable");
    const auto cancellationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!cancellationObserved.load() && std::chrono::steady_clock::now() < cancellationDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    test.Expect(cancellationObserved.load(), "a cancelled task should observe its stop token promptly");

    std::atomic<bool> synchronousWork{false};
    test.Expect(coordinator.SubmitAndWait(L"synchronous test", {resource},
        [&synchronousWork](std::stop_token) { synchronousWork = true; }) && synchronousWork.load(),
        "SubmitAndWait should preserve resource serialization for synchronous callers");

    test.Expect(coordinator.Submit(L"exception test", {}, [](std::stop_token) { throw 1; }),
        "TaskCoordinator should contain worker exceptions");
    const auto exceptionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool exceptionReported = false;
    while (!exceptionReported && std::chrono::steady_clock::now() < exceptionDeadline) {
        for (const auto& event : coordinator.PollEvents()) {
            if (event.type == L"task-failed" && event.message == L"exception test") exceptionReported = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    test.Expect(exceptionReported, "TaskCoordinator should convert worker exceptions into immutable failure events");

    std::atomic<bool> shutdownStarted{false};
    std::atomic<bool> shutdownObserved{false};
    test.Expect(coordinator.Submit(L"shutdown test", {}, [&shutdownStarted, &shutdownObserved](std::stop_token token) {
        shutdownStarted = true;
        while (!token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        shutdownObserved = true;
    }), "TaskCoordinator should accept work before coordinated shutdown");
    const auto shutdownStartDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!shutdownStarted.load() && std::chrono::steady_clock::now() < shutdownStartDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto shutdownBegin = std::chrono::steady_clock::now();
    coordinator.StopAndJoin();
    test.Expect(shutdownObserved.load(), "coordinated shutdown should request stop before joining workers");
    test.Expect(std::chrono::steady_clock::now() - shutdownBegin < std::chrono::seconds(1),
        "cooperative task shutdown should complete promptly");
    test.Expect(!coordinator.IsAcceptingTasks(), "TaskCoordinator should reject work after shutdown begins");
    test.Expect(!coordinator.Submit(L"rejected test", {}, [](std::stop_token) {}),
        "TaskCoordinator must not accept work after shutdown");
}

void TestInterruptedTaskRecovery(TestContext& test, const std::filesystem::path& root) {
    const auto runtime = root / "recovery-runtime";
    const auto interruptedDirectory = runtime / "MineBackup_Filelist_interrupted";
    const auto interruptedFile = runtime / "MineBackup_cloud_history_interrupted.json";
    const auto unrelated = runtime / "user-owned.txt";
    std::filesystem::create_directories(interruptedDirectory);
    std::ofstream(interruptedDirectory / "filelist.txt") << "temporary";
    std::ofstream(interruptedFile) << "temporary-cloud";
    std::ofstream(unrelated) << "keep";

    const auto reportPath = root / "state" / "task-recovery" / "last-interrupted.json";
    const auto report = RecoverInterruptedTaskArtifacts(runtime, reportPath);
    test.Expect(report.removedPaths.size() == 2 && report.removedBytes > 0,
        "startup recovery should remove only known interrupted-task artifacts");
    test.Expect(!std::filesystem::exists(interruptedDirectory) && !std::filesystem::exists(interruptedFile),
        "startup recovery should remove interrupted files and directories");
    test.Expect(std::filesystem::exists(unrelated), "startup recovery must preserve unrelated runtime files");
    test.Expect(report.reportPath == reportPath && ReadText(reportPath).find("RemovedPaths") != std::string::npos,
        "startup recovery should persist a report through AtomicFileWriter");
}

void TestNetworkService(TestContext& test, const std::filesystem::path& root) {
    Sha256 hash;
    hash.Update("abc", 3);
    const std::string abcHash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    test.Expect(hash.FinalHex() == abcHash, "SHA-256 should match the standard abc vector");

    auto backend = std::make_shared<FakeNetworkBackend>();
    NetworkService network(backend);
    NetworkRequest request;
    request.url = "https://example.test/content";
    backend->body = "network text";
    auto textResult = network.GetText(request);
    test.Expect(textResult.status == NetworkStatus::Succeeded && textResult.text == backend->body,
        "NetworkService should return bounded HTTPS text");

    request.url = "http://example.test/insecure";
    textResult = network.GetText(request);
    test.Expect(textResult.status == NetworkStatus::InvalidRequest,
        "NetworkService should reject an initial non-HTTPS request");
    request.url = "https://example.test/content";

    backend->syntheticChunkSize = NetworkService::MaximumTextBytes + 1;
    textResult = network.GetText(request);
    test.Expect(textResult.status == NetworkStatus::TooLarge && textResult.text.empty(),
        "NetworkService should distinguish an oversized text response");
    backend->syntheticChunkSize = 0;

    backend->configuredResult.status = NetworkStatus::RedirectLimit;
    test.Expect(network.GetText(request).status == NetworkStatus::RedirectLimit,
        "NetworkService should preserve redirect-loop diagnostics");
    backend->configuredResult.status = NetworkStatus::TlsError;
    test.Expect(network.GetText(request).status == NetworkStatus::TlsError,
        "NetworkService should preserve TLS diagnostics");
    backend->configuredResult.status = NetworkStatus::InsecureRedirect;
    test.Expect(network.GetText(request).status == NetworkStatus::InsecureRedirect,
        "NetworkService should distinguish an HTTPS-to-HTTP redirect rejection");
    backend->configuredResult.status = NetworkStatus::HttpError;
    test.Expect(network.GetText(request).status == NetworkStatus::HttpError,
        "NetworkService should preserve HTTP diagnostics");
    backend->configuredResult.status = NetworkStatus::Truncated;
    test.Expect(network.GetText(request).status == NetworkStatus::Truncated,
        "NetworkService should distinguish a truncated response");
    backend->configuredResult.status = NetworkStatus::Succeeded;

    std::stop_source cancelled;
    cancelled.request_stop();
    test.Expect(network.GetText(request, cancelled.get_token()).status == NetworkStatus::Cancelled,
        "NetworkService should preserve cancellation status");

    const auto destination = root / "network" / "download.bin";
    backend->body = "abc";
    auto download = network.Download(request, destination, abcHash);
    test.Expect(download.status == NetworkStatus::Succeeded && ReadText(destination) == "abc"
        && download.sha256 == abcHash, "a verified download should commit atomically");

    std::ofstream(destination, std::ios::binary | std::ios::trunc) << "current";
    download = network.Download(request, destination, std::string(64, '0'));
    test.Expect(download.status == NetworkStatus::HashMismatch && ReadText(destination) == "current",
        "a hash mismatch should preserve the current destination");

    backend->syntheticChunkSize = static_cast<std::size_t>(NetworkService::MaximumDownloadBytes + 1);
    download = network.Download(request, destination, abcHash);
    test.Expect(download.status == NetworkStatus::TooLarge && ReadText(destination) == "current",
        "an oversized download should preserve the current destination");
    backend->syntheticChunkSize = 0;
    int stagingFiles = 0;
    for (const auto& entry : std::filesystem::directory_iterator(destination.parent_path())) {
        if (entry.path().filename().wstring().find(L".download.") != std::wstring::npos) ++stagingFiles;
    }
    test.Expect(stagingFiles == 0, "failed downloads should remove their unique staging files");

    backend->body = R"({"tag_name":"v9.8.7","body":"notes","assets":[{"browser_download_url":"https://evil.test/payload"}]})";
    const auto update = CheckMineBackupUpdate(network, "1.16.0", "en_US");
    test.Expect(update.success && update.updateAvailable && update.latestTag == "v9.8.7",
        "update parsing should accept only the version and release notes model");
    test.Expect(BuildMineBackupOfficialReleaseUrl(update.latestTag)
        == "https://github.com/Leafuke/MineBackup/releases/tag/v9.8.7",
        "the update action should construct an exact official Release URL");
    backend->body = R"({"tag_name":"v9.8.7/../../payload","body":"notes"})";
    test.Expect(!CheckMineBackupUpdate(network, "1.16.0", "en_US").success,
        "update parsing should reject a tag that could alter the application-built Release URL");
    test.Expect(BuildMineBackupOfficialReleaseUrl("v9.8.7/../../payload").empty(),
        "the official Release URL builder should reject path-altering tags");
    backend->body = "404: Not Found";
    test.Expect(!CheckMineBackupNotice(network, "en_US", "").success,
        "notice parsing should reject 404 bodies from both direct and mirror text sources");
}

void TestDesktopServicesAndCapabilities(TestContext& test) {
    ResetDesktopServices();
    const auto unavailable = GetDesktopServices();
    test.Expect(!unavailable->Capabilities().tray.IsAvailable(),
        "the desktop registry should provide an unavailable service before native initialization");
    test.Expect(unavailable->SelectFile().status.state == CapabilityState::Unavailable,
        "desktop calls before initialization should fail safely instead of dereferencing null");
    test.Expect(!CanHideToTray(unavailable->Capabilities()),
        "an unavailable tray must disable hide-to-tray for the current run");

    auto mock = std::make_shared<MockDesktopServices>();
    InstallDesktopServices(mock);
    test.Expect(GetDesktopServices() == mock, "tests should be able to inject a desktop service mock");
    test.Expect(mock->Capabilities().tray.state == CapabilityState::PermissionRequired
        && !mock->Capabilities().tray.diagnostic.empty(),
        "capability states should retain a user-readable degradation reason");
    test.Expect(!CanHideToTray(mock->Capabilities()),
        "permission-required tray state must not make the window unreachable");
    test.Expect(mock->SelectFile().path == mock->selectedPath,
        "desktop file selection should flow through the injected service");
    test.Expect(mock->SetAutostart(true).IsAvailable() && mock->autostartEnabled,
        "desktop autostart should expose both status and the requested state");
    test.Expect(mock->OpenAutostartSettings().IsAvailable()
        && mock->autostartSettingsOpenCount == 1,
        "desktop services should expose the platform autostart settings entry");
    test.Expect(mock->ActivateWindow().IsAvailable() && mock->activationCount == 1,
        "window activation should flow through the desktop service");
    const std::vector<GlobalHotkeyBinding> hotkeys{{1, 'B', L"Backup"}, {2, 'R', L"Restore"}};
    test.Expect(!mock->ConfigureGlobalHotkeys(hotkeys).IsAvailable()
        && mock->configuredHotkeys.size() == 2,
        "global hotkeys should be configured as one coherent set for portal sessions");

    const auto failed = CapabilityStatus::Failed(L"synthetic desktop failure");
    test.Expect(failed.state == CapabilityState::Failed && !failed.IsAvailable()
        && failed.diagnostic == L"synthetic desktop failure",
        "failed capabilities should retain a distinct state and diagnostic");

    mock->capabilities.tray = CapabilityStatus::Ready();
    test.Expect(CanHideToTray(mock->Capabilities()),
        "an available tray should allow hide-to-tray for the current run");
    ResetDesktopServices();
}

void TestSpecialConfigExecutionPolicy(TestContext& test) {
    std::map<int, SpecialConfig> configs;
    configs[8].autoExecute = true;
    configs[8].runOnStartup = true;
    configs[2].autoExecute = true;
    configs[2].runOnStartup = true;
    configs[5].autoExecute = true;
    configs[5].runOnStartup = true;

    const auto normalized = NormalizeSpecialConfigExecutionPolicy(configs);
    test.Expect(normalized.autoExecuteIndex == 2 && normalized.runOnStartupIndex == 2,
        "duplicate startup selections should retain the lowest deterministic config index");
    test.Expect(normalized.disabledDuplicateAutoExecute == 2
        && normalized.disabledDuplicateRunOnStartup == 2,
        "normalization should report every disabled duplicate startup selection");
    test.Expect(configs[2].autoExecute && !configs[5].autoExecute && !configs[8].autoExecute,
        "at most one special configuration may auto-execute");

    SetExclusiveSpecialAutoExecute(configs, 8, true);
    test.Expect(configs[8].autoExecute && !configs[2].autoExecute && !configs[5].autoExecute,
        "enabling a new auto-execute target should disable the old target");
    SetExclusiveSpecialRunOnStartup(configs, 5, true);
    test.Expect(FindSpecialRunOnStartup(configs) == 5
        && configs[5].runOnStartup && !configs[2].runOnStartup && !configs[8].runOnStartup,
        "OS autostart should resolve to exactly one persisted special configuration");
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "version") {
        const auto executable = std::filesystem::absolute(argv[0]).wstring();
        const bool managed = executable.find(ExternalToolManager::RcloneVersion) != std::wstring::npos;
        std::cout << (managed ? "rclone v1.74.4\n" : "rclone v9.9.9\n");
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--process-helper-echo") {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
        int wideArgumentCount = 0;
        LPWSTR* wideArguments = CommandLineToArgvW(GetCommandLineW(), &wideArgumentCount);
        if (!wideArguments || wideArgumentCount != argc) return 2;
#endif
        for (int index = 2; index < argc; ++index) {
#ifdef _WIN32
            std::cout << wstring_to_utf8(wideArguments[index]) << '\n';
#else
            std::cout << argv[index] << '\n';
#endif
        }
#ifdef _WIN32
        LocalFree(wideArguments);
#endif
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--process-helper-output") {
        std::cout << std::string(10000, 'o');
        std::cerr << std::string(10000, 'e');
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--process-helper-cwd") {
        std::cout << wstring_to_utf8(std::filesystem::current_path().wstring()) << '\n';
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--process-helper-grandchild") {
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        std::ofstream(argv[2]) << "should-not-exist";
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--process-helper-spawn") {
        ProcessSpec child;
        child.executable = std::filesystem::absolute(argv[0]);
        child.arguments = {L"--process-helper-grandchild", std::filesystem::path(argv[2]).wstring()};
        return ProcessRunner::Run(child).status == ProcessStatus::Succeeded ? 0 : 1;
    }
    TestContext test;
    TemporaryDirectory temporary;
    TestFormat(test);
    TestAtomicWriter(test, temporary.path);
    TestMetadataRoundTrip(test, temporary.path);
    TestHistoryRoundTrip(test, temporary.path);
    TestMigrationCoordinator(test, temporary.path);
    TestRotatingLog(test, temporary.path);
    TestLaunchOptionsAndAppPaths(test, temporary.path);
    TestLegacyServicePolicy(test);
    TestSingleInstance(test, temporary.path);
    TestLegacyLocationDiscovery(test, temporary.path);
    TestLegacyLocationMigration(test, temporary.path);
    TestProcessRunner(test, std::filesystem::absolute(argv[0]), temporary.path);
    TestSevenZipArgumentVector(test, temporary.path);
    TestExternalToolManager(test, temporary.path, std::filesystem::absolute(argv[0]));
    TestPortableConfigDocument(test);
    TestInterruptedTaskRecovery(test, temporary.path);
    TestNetworkService(test, temporary.path);
    TestTaskCoordinator(test, temporary.path);
    TestDesktopServicesAndCapabilities(test);
    TestSpecialConfigExecutionPolicy(test);

    if (test.failures == 0) {
        std::cout << "[PASS] MineBackup data-core tests\n";
        return 0;
    }
    std::cerr << test.failures << " test assertion(s) failed\n";
    return 1;
}
