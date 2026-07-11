#include "AtomicFileWriter.h"
#include "AppPaths.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "FolderRewindMetadataStore.h"
#include "MigrationCoordinator.h"
#include "RotatingFileLog.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

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

void TestFormat(TestContext& test) {
    test.Expect(FolderRewindFormat::IsSafeSinglePathSegment(L"World One"), "normal world name should be safe");
    test.Expect(!FolderRewindFormat::IsSafeSinglePathSegment(L"../World"), "parent traversal must be rejected");
    const auto sanitized = FolderRewindFormat::SanitizePathSegment(L"../World");
    test.Expect(FolderRewindFormat::IsSafeSinglePathSegment(sanitized), "sanitized world name should be a safe segment");
    test.Expect(FolderRewindFormat::IsSmartBackupType(L"[Smart]-World.7z"), "smart archive should be recognized");
    test.Expect(FolderRewindFormat::EnsureConfigId(L"").size() == 36, "empty ConfigId should produce a UUID");
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

} // namespace

int main() {
    TestContext test;
    TemporaryDirectory temporary;
    TestFormat(test);
    TestAtomicWriter(test, temporary.path);
    TestMetadataRoundTrip(test, temporary.path);
    TestHistoryRoundTrip(test, temporary.path);
    TestMigrationCoordinator(test, temporary.path);
    TestRotatingLog(test, temporary.path);
    TestLaunchOptionsAndAppPaths(test, temporary.path);

    if (test.failures == 0) {
        std::cout << "[PASS] MineBackup data-core tests\n";
        return 0;
    }
    std::cerr << test.failures << " test assertion(s) failed\n";
    return 1;
}
