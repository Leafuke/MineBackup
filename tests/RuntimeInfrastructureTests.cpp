#include "AtomicFileWriter.h"
#include "AppPaths.h"
#include "DiagnosticLogExporter.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "FolderRewindMetadataStore.h"
#include "HotRestoreCoordinator.h"
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
#include "ProfileConfigRepository.h"
#include "ProfileKnotLinkCommands.h"
#include "ProfileRuntime.h"
#include "RuntimeIntegration.h"
#include "JobDocument.h"
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

#include "RuntimeInfrastructureTests.h"

namespace {

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
                coordinator.PostEvent({L"test-complete", token.stop_possible() ? L"stoppable" : L"unstoppable"});
                completed.fetch_add(1);
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

    download = network.Download(request, destination, {});
    test.Expect(download.status == NetworkStatus::Succeeded && ReadText(destination) == "abc"
        && download.sha256 == abcHash,
        "an explicitly unverified HTTPS download should still report its calculated hash");

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
    const auto links = BuildMineBackupUpdateLinks("9.8.7");
    test.Expect(links.changelogUrl
            == "https://github.com/Leafuke/MineBackup/releases/tag/v9.8.7",
        "update links should normalize an unprefixed version tag");
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const std::string expectedAsset = "MineBackup-windows-x64.exe";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    const std::string expectedAsset = "MineBackup-9.8.7-macos-arm64.dmg";
#elif defined(__linux__) && defined(__x86_64__)
    const std::string expectedAsset = "minebackup_9.8.7_amd64.deb";
#else
    const std::string expectedAsset;
#endif
    if (expectedAsset.empty()) {
        test.Expect(!links.supported && links.officialDownloadUrl.empty()
                && links.acceleratedDownloadUrl.empty(),
            "unsupported platform builds should not expose a direct update asset");
    }
    else {
        const std::string expectedOfficial =
            "https://github.com/Leafuke/MineBackup/releases/download/v9.8.7/" + expectedAsset;
        test.Expect(links.supported && links.officialDownloadUrl == expectedOfficial,
            "update links should select the current platform asset");
        test.Expect(links.acceleratedDownloadUrl == "https://gh-proxy.org/" + expectedOfficial,
            "accelerated update links should prefix the official asset URL");
    }
    const auto invalidLinks = BuildMineBackupUpdateLinks("v9.8.7/../../payload");
    test.Expect(!invalidLinks.supported && invalidLinks.officialDownloadUrl.empty()
            && invalidLinks.acceleratedDownloadUrl.empty() && invalidLinks.changelogUrl.empty(),
        "update link construction should reject path-altering version tags");
    backend->body = R"({"tag_name":"v9.8.7/../../payload","body":"notes"})";
    test.Expect(!CheckMineBackupUpdate(network, "1.16.0", "en_US").success,
        "update parsing should reject a tag that could alter the application-built Release URL");
    test.Expect(BuildMineBackupOfficialReleaseUrl("v9.8.7/../../payload").empty(),
        "the official Release URL builder should reject path-altering tags");
    backend->body = "404: Not Found";
    test.Expect(!CheckMineBackupNotice(network, "en_US", "").success,
        "notice parsing should reject 404 bodies from both direct and mirror text sources");
}

void TestKnotLinkPackageManifest(TestContext& test) {
    const auto& package = minebackup::knotlink::CurrentKnotLinkPackage();
    test.Expect(package.supported,
        "the release build platforms should have a KnotLinkService package");
    test.Expect(package.version == "3.2.0.0",
        "the current KnotLinkService package version should be 3.2.0.0");
    test.Expect(!package.fileName.empty() &&
            package.officialUrl.ends_with(package.fileName),
        "the official KnotLinkService URL should end with the platform asset name");
    test.Expect(package.mirrorUrl == "https://gh-proxy.org/" + package.officialUrl,
        "the KnotLinkService mirror URL should prefix the official URL");
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

void TestLoggingCore(TestContext& test, const std::filesystem::path& root) {
    using namespace minebackup::logging;

    bool valid = false;
    test.Expect(ParseLogLevel("WARNING", &valid) == LogLevel::Warning && valid,
        "log view levels should parse case-insensitively");
    test.Expect(ParseLogLevel("unexpected", &valid) == LogLevel::Info && !valid,
        "invalid log view levels should safely fall back to info");
    test.Expect(ParseFileLevel("DEBUG", &valid) == LogFileLevel::Debug && valid,
        "log file levels should parse case-insensitively");
    test.Expect(ParseFileLevel("unexpected", &valid) == LogFileLevel::Info && !valid,
        "invalid log file levels should safely fall back to info");
    const auto newValueWins = ResolveFileLevel("debug", false);
    test.Expect(newValueWins.level == LogFileLevel::Debug
        && !newValueWins.usedLegacyAutoLog
        && !newValueWins.invalidConfiguredValue,
        "the new LogFileLevel value should take precedence over legacy AutoLog");
    const auto legacyDisabled = ResolveFileLevel(std::nullopt, false);
    const auto legacyEnabled = ResolveFileLevel(std::nullopt, true);
    const auto defaultLevel = ResolveFileLevel(std::nullopt, std::nullopt);
    const auto invalidLevel = ResolveFileLevel("verbose", true);
    test.Expect(legacyDisabled.level == LogFileLevel::Off
        && legacyDisabled.usedLegacyAutoLog
        && legacyEnabled.level == LogFileLevel::Info
        && defaultLevel.level == LogFileLevel::Info,
        "legacy AutoLog and missing keys should resolve to the documented defaults");
    test.Expect(invalidLevel.level == LogFileLevel::Info
        && invalidLevel.invalidConfiguredValue
        && !invalidLevel.usedLegacyAutoLog,
        "an invalid new level should fall back to info without consulting AutoLog");

    {
        ScopedLogContext context{{"operation_id", "backup;42"}, {"world", "测试世界"}};
        Write(minebackup::logging::LogLevel::Warning, LogCategory::Backup, "backup.test",
            "\x1b[31mfirst\r\nsecond\x01", {"LoggingCoreTest.cpp", 42});
    }
    LogPrintf(minebackup::logging::LogLevel::Info, LogCategory::Application, "format.bad",
        {"LoggingCoreTest.cpp", 50}, "%");
    Write(minebackup::logging::LogLevel::Debug, LogCategory::Process, "process.long",
        std::string(70 * 1024, 'x'), {"LoggingCoreTest.cpp", 53});

    const auto read = ReadAfter(0);
    test.Expect(read.records.size() == 4,
        "multiline logging and formatting failures should create independent records");
    test.Expect(read.records[0]->message == "first" && read.records[1]->message == "second",
        "logging should normalize CRLF and remove ANSI/control characters");
    test.Expect(read.records[0]->sequence + 1 == read.records[1]->sequence
        && read.records[1]->sequence + 1 == read.records[2]->sequence,
        "structured records should receive monotonically increasing sequences");
    test.Expect(read.records[0]->context.size() == 2
        && read.records[0]->context[0].value == "backup;42",
        "scoped logging context should be copied into immutable records");
    test.Expect(read.records[2]->eventId == "logging.format_error",
        "runtime printf failures should degrade to a safe logging event");
    test.Expect(read.records[3]->message.size() <= 64 * 1024
        && read.records[3]->message.find("[truncated at 64 KiB]") != std::string::npos,
        "oversized log lines should be explicitly truncated at 64 KiB");

    InitializeOptions options;
    options.logsDirectory = root / "logging-off";
    options.fileLevel = LogFileLevel::Off;
    Initialize(options);
    Write(minebackup::logging::LogLevel::Info, LogCategory::Application, "logging.off", "memory only");
    Shutdown();
    test.Expect(!std::filesystem::exists(options.logsDirectory / "minebackup.log"),
        "file logging off should not create a log file");

    options.logsDirectory = root / "logging-info";
    options.fileLevel = LogFileLevel::Info;
    Initialize(options);
    test.Expect(std::filesystem::is_regular_file(options.logsDirectory / ".active-session"),
        "enabled file logging should create an active-session marker");
    const auto fileSession = GetStatus().sessionId;
    Write(minebackup::logging::LogLevel::Debug, LogCategory::Application,
        "logging.filtered", "debug should be filtered");
    Write(minebackup::logging::LogLevel::Info, LogCategory::Application,
        "logging.persisted", "info should be persisted");
    SetFileLevel(LogFileLevel::Debug);
    Write(minebackup::logging::LogLevel::Debug, LogCategory::Application,
        "logging.debug_enabled", "debug should now be persisted");
    SetFileLevel(LogFileLevel::Info);
    Write(minebackup::logging::LogLevel::Debug, LogCategory::Application,
        "logging.debug_disabled", "debug should be filtered again");
    SetFileLevel(LogFileLevel::Off);
    test.Expect(!std::filesystem::exists(options.logsDirectory / ".active-session"),
        "disabling file logging should remove the active-session marker");
    Write(minebackup::logging::LogLevel::Error, LogCategory::Application,
        "logging.disabled", "file remains disabled");
    Shutdown();
    std::ifstream persistedLog(options.logsDirectory / "minebackup.log", std::ios::binary);
    const std::string persisted((std::istreambuf_iterator<char>(persistedLog)),
        std::istreambuf_iterator<char>());
    test.Expect(persisted.find("[INFO] [Application] info should be persisted")
            != std::string::npos
        && persisted.find("event=logging.debug_enabled") != std::string::npos
        && persisted.find("event=logging.filtered") == std::string::npos
        && persisted.find("event=logging.debug_disabled") == std::string::npos
        && persisted.find("event=logging.disabled") == std::string::npos,
        "info files should stay concise while debug mode adds structured fields");
    test.Expect(persisted.find("[INFO] [Session] ===== MineBackup session started (")
            != std::string::npos
        && persisted.find(fileSession) == std::string::npos
        && persisted.find(" session=") == std::string::npos
        && persisted.find(" seq=") == std::string::npos,
        "local files should use short session boundaries instead of repeating identifiers");

    options.logsDirectory = root / "logging-abnormal";
    std::filesystem::create_directories(options.logsDirectory);
    std::ofstream(options.logsDirectory / ".active-session") << "abandoned-session\n";
    options.fileLevel = LogFileLevel::Off;
    Initialize(options);
    const auto abnormalStatus = GetStatus();
    test.Expect(abnormalStatus.previousSessionAbnormal
        && !std::filesystem::exists(options.logsDirectory / ".active-session"),
        "a stale session marker should be reported once and cleared when logging is off");
    Shutdown();
}

std::size_t CountOccurrences(
    std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

std::vector<std::filesystem::path> OrderedLogFiles(
    const std::filesystem::path& directory) {
    std::map<int, std::filesystem::path, std::greater<>> archived;
    std::filesystem::path active;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name == "minebackup.log") {
            active = entry.path();
            continue;
        }
        constexpr std::string_view prefix = "minebackup.";
        constexpr std::string_view suffix = ".log";
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) continue;
        const auto number = name.substr(
            prefix.size(), name.size() - prefix.size() - suffix.size());
        try {
            archived.emplace(std::stoi(number), entry.path());
        } catch (...) {
        }
    }
    std::vector<std::filesystem::path> result;
    for (const auto& [index, path] : archived) {
        (void)index;
        result.push_back(path);
    }
    if (!active.empty()) result.push_back(active);
    return result;
}

void TestLoggingStressAndRotation(
    TestContext& test, const std::filesystem::path& root) {
    using namespace minebackup::logging;

    Shutdown();
    const auto startupBaseline = GetStatus().latestSequence;
    for (int index = 0; index < 300; ++index) {
        Write(LogLevel::Debug, LogCategory::Application,
            "logging.startup_buffer", "startup-record-" + std::to_string(index));
    }
    InitializeOptions startupOptions{
        root / "logging-startup-buffer", LogFileLevel::Debug, false};
    Initialize(startupOptions);
    Shutdown();
    const auto startupText = ReadText(
        startupOptions.logsDirectory / "minebackup.log");
    test.Expect(GetStatus().latestSequence == startupBaseline + 302,
        "pre-initialization writes and session boundaries should receive global sequences");
    test.Expect(CountOccurrences(
        startupText, "event=logging.startup_buffer") == 256,
        "only the newest 256 pre-initialization records should replay to disk");
    test.Expect(startupText.find("startup-record-43") == std::string::npos
        && startupText.find("startup-record-44") != std::string::npos,
        "startup replay should retain the documented newest-record boundary");

    InitializeOptions options{
        root / "logging-concurrent", LogFileLevel::Debug, false};
    Initialize(options);
    const auto before = GetStatus();
    constexpr int threadCount = 4;
    constexpr int recordsPerThread = 25'000;
    std::vector<std::jthread> writers;
    for (int worker = 0; worker < threadCount; ++worker) {
        writers.emplace_back([worker] {
            for (int index = 0; index < recordsPerThread; ++index) {
                Write(LogLevel::Debug, LogCategory::Task,
                    "logging.concurrent",
                    "worker=" + std::to_string(worker)
                        + " record=" + std::to_string(index) + " utf8=世界");
            }
        });
    }
    writers.clear();

    const auto after = GetStatus();
    const auto retained = ReadAfter(before.latestSequence);
    test.Expect(after.latestSequence == before.latestSequence
            + threadCount * recordsPerThread,
        "four concurrent producers should allocate exactly 100,000 sequences");
    test.Expect(after.retainedCount == 20'000 && retained.records.size() == 20'000
            && retained.requestedSequenceWasEvicted,
        "the session ring should retain 20,000 records and report stale cursors");
    bool contiguous = !retained.records.empty();
    for (std::size_t index = 1; index < retained.records.size(); ++index) {
        contiguous = contiguous
            && retained.records[index - 1]->sequence + 1
                == retained.records[index]->sequence;
    }
    test.Expect(contiguous
            && retained.records.back()->sequence == after.latestSequence,
        "retained concurrent records should have unique contiguous global sequences");
    Shutdown();
    test.Expect(!std::filesystem::exists(options.logsDirectory / ".active-session"),
        "normal shutdown should remove the active-session marker");

    const auto files = OrderedLogFiles(options.logsDirectory);
    test.Expect(files.size() >= 2 && files.size() <= 5,
        "the stress log should rotate while retaining at most four archives");
    std::size_t persistedRecords = 0;
    bool completeUtf8Lines = true;
    for (const auto& path : files) {
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(path, sizeError);
        test.Expect(!sizeError && size <= 10 * 1024 * 1024 + 64 * 1024,
            "rotated files should stay within one maximum log line of 10 MiB");
        std::istringstream lines(ReadText(path));
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find("event=logging.concurrent") == std::string::npos) continue;
            ++persistedRecords;
            completeUtf8Lines = completeUtf8Lines
                && line.find("[DEBUG] [Task] worker=") != std::string::npos
                && line.find("utf8=世界 | event=logging.concurrent") != std::string::npos;
        }
    }
    test.Expect(persistedRecords == threadCount * recordsPerThread,
        "blocking overflow and shutdown drain should persist all 100,000 records");
    test.Expect(completeUtf8Lines,
        "rotation should preserve complete UTF-8 log lines");
}

void TestLoggingFailureAndDiagnostics(
    TestContext& test, const std::filesystem::path& root) {
    using namespace minebackup::logging;
    using namespace minebackup::diagnostics;

    const auto invalidTarget = root / "logging-invalid-target";
    std::ofstream(invalidTarget) << "not a directory";
    Initialize({invalidTarget, LogFileLevel::Info, false});
    const auto failedStatus = GetStatus();
    const auto beforeFailureWrite = failedStatus.latestSequence;
    Write(LogLevel::Error, LogCategory::Application,
        "logging.degraded", "memory remains available");
    const auto degradedRead = ReadAfter(beforeFailureWrite);
    test.Expect(!failedStatus.fileBackendActive
            && !failedStatus.lastBackendError.empty(),
        "an invalid log directory should disable only the file backend");
    test.Expect(degradedRead.records.size() == 1
            && degradedRead.records.front()->eventId == "logging.degraded",
        "backend initialization failure should leave the session store usable");
    Shutdown();

    const std::string home = "C:\\Users\\Alice";
    const std::string profile = home + "\\Profiles\\Private";
    const std::string remote = "secret-remote:private/backups";
    const std::string secretText =
        profile + "\\world https://alice:password@example.test/api"
        "?token=top-secret#fragment " + remote;
    const std::vector<RedactionRule> rules{
        {home, "<user-home>"},
        {profile, "<profile-root>"},
        {remote, "<rclone-remote>"}};
    const auto redacted = RedactText(secretText, rules);
    test.Expect(redacted.find(profile) == std::string::npos
            && redacted.find("alice:password") == std::string::npos
            && redacted.find("top-secret") == std::string::npos
            && redacted.find(remote) == std::string::npos,
        "diagnostic redaction should remove paths, URL credentials/query, and remotes");
    test.Expect(redacted.find("<profile-root>") != std::string::npos
            && redacted.find("<userinfo>") != std::string::npos
            && redacted.find("?<query>#fragment") != std::string::npos,
        "longest path and URL redactions should leave explicit diagnostic markers");

    const auto exportDirectory = root / "diagnostics";
    Initialize({exportDirectory, LogFileLevel::Off, false});
    Write(LogLevel::Error, LogCategory::Cloud,
        "diagnostics.secret_fixture", secretText,
        {"DiagnosticExporterTest.cpp", 77});
    DiagnosticExportOptions options;
    options.logsDirectory = exportDirectory;
    options.applicationVersion = "test-version";
    options.platform = "test-platform";
    options.profileMode = "explicit";
    options.redactions = rules;
    const auto result = ExportDiagnostics(options);
    test.Expect(result.success && std::filesystem::is_regular_file(result.path),
        "diagnostic export should create a timestamped UTF-8 text file");
    const auto exported = ReadText(result.path);
    test.Expect(exported.find("version=test-version") != std::string::npos
            && exported.find("platform=test-platform") != std::string::npos
            && exported.find("profile_mode=explicit") != std::string::npos
            && exported.find("event=diagnostics.secret_fixture") != std::string::npos,
        "diagnostics should contain only the declared metadata and retained records");
    test.Expect(exported.find(profile) == std::string::npos
            && exported.find("alice:password") == std::string::npos
            && exported.find("top-secret") == std::string::npos
            && exported.find(remote) == std::string::npos,
        "exported diagnostics must not retain the configured secret fixtures");
    Shutdown();
}

void TestProfileRuntimeReload(
	TestContext& test,
	const std::filesystem::path& root) {
	const auto profileRoot = root / "profile-runtime";
	AppPaths paths;
	paths.configRoot = profileRoot / "config";
	paths.dataRoot = profileRoot / "data";
	paths.stateRoot = profileRoot / "state";
	paths.cacheRoot = profileRoot / "cache";
	paths.runtimeRoot = profileRoot / "runtime";
	paths.toolsRoot = profileRoot / "tools";
	paths.logsRoot = profileRoot / "logs";
	paths.resourcesRoot = profileRoot / "resources";
	paths.profileIdentity = L"profile-runtime-test";
	std::filesystem::create_directories(paths.configRoot);
	std::filesystem::create_directories(paths.dataRoot);
	std::filesystem::create_directories(profileRoot / "server" / "world");

	Config config;
	config.configId = L"11111111-1111-4111-8111-111111111111";
	config.name = "Server";
	config.saveRoot = (profileRoot / "server").wstring();
	config.backupPath = (profileRoot / "backups").wstring();
	config.worlds = {{L"world", L"Primary"}};
	test.Expect(ProfileConfigRepository(paths.ConfigFile()).Save(
		{{1, config}}, {L"session.lock"}, true).success,
		"ProfileRuntime fixture should persist a server config");

	JobStep step;
	step.stepId = L"44444444-4444-4444-8444-444444444444";
	step.name = "World";
	step.type = JobStepType::Backup;
	step.backup = {config.configId, L"world", {}};
	JobStage stage;
	stage.stageId = L"33333333-3333-4333-8333-333333333333";
	stage.name = "Backup";
	stage.steps = {step};
	Job job;
	job.jobId = L"22222222-2222-4222-8222-222222222222";
	job.name = "Nightly";
	job.stages = {stage};
	JobDocument document;
	document.jobs = {job};
	std::wstring saveError;
	test.Expect(JobStorage::Save(paths.JobsFile(), document, saveError),
		"ProfileRuntime fixture should persist jobs.json");

	ProfileRuntime runtime(paths, {true, {}});
	const auto loaded = runtime.Reload();
	test.Expect(IsSuccessful(loaded.code) && runtime.IsReady()
			&& runtime.Catalog().configs.size() == 1
			&& runtime.Jobs().jobs.size() == 1
			&& runtime.ResolveBackup(config.configId, L"world").has_value(),
		"ProfileRuntime should own a coherent catalog, Job and history snapshot");

	auto bridge = std::make_shared<HeadlessKnotLinkBridge>();
	ProfileKnotLinkCommands commands(runtime, bridge);
	minebackup::knotlink::KnotLinkCommandDispatcher dispatcher(
		[&commands](const auto& context) { return commands.Handle(context); });
	const auto listed = minebackup::knotlink::KnotLinkKeyValueCodec::Parse(
		dispatcher.Dispatch("cmd=LIST_CONFIGS"));
	const auto folders = minebackup::knotlink::KnotLinkKeyValueCodec::Parse(
		dispatcher.Dispatch(
			"cmd=LIST_FOLDERS;config_id=11111111-1111-4111-8111-111111111111"));
	test.Expect(listed.values.at("status") == "ok"
			&& listed.values.at("data").find("11111111-1111-4111-8111-111111111111")
				!= std::string::npos
			&& folders.values.at("data") == "world",
		"the headless KnotLink dispatcher should query the ProfileRuntime snapshot");
	const auto removedSchedule = minebackup::knotlink::KnotLinkKeyValueCodec::Parse(
		dispatcher.Dispatch(
			"cmd=AUTO_BACKUP;from=test;request_id=removed-schedule"));
	test.Expect(removedSchedule.values.at("status") == "error"
			&& removedSchedule.values.at("code") == "unknown_command",
		"the headless dispatcher should not reintroduce internal scheduling");

	document.jobs.front().stages.front().steps.front().backup.configId =
		L"99999999-9999-4999-8999-999999999999";
	test.Expect(JobStorage::Save(paths.JobsFile(), document, saveError),
		"ProfileRuntime invalid reload fixture should persist");
	const auto rejected = runtime.Reload();
	test.Expect(rejected.code == OperationCode::InvalidProfile
			&& runtime.IsReady()
			&& runtime.Jobs().jobs.front().jobId == job.jobId,
		"a failed ProfileRuntime reload should retain the previous coherent snapshot");
}

void TestHotRestoreCoordinator(TestContext& test) {
	std::vector<std::string> events;
	bool reset = false;
	HotRestoreDependencies dependencies;
	dependencies.transport.reset = [&] { reset = true; };
	dependencies.transport.emit = [&](std::string_view event,
		const std::vector<std::pair<std::string, std::string>>&) {
		events.emplace_back(event);
		return true;
	};
	dependencies.transport.waitHandshake = [](
		std::chrono::milliseconds, std::stop_token) {
		return HotRestoreHandshakeStatus::Compatible;
	};
	dependencies.transport.waitSaveAndExit = [](
		std::chrono::milliseconds, std::stop_token) {
		return true;
	};
	dependencies.transport.waitRejoin = [](
		std::chrono::milliseconds, std::stop_token) {
		return std::optional<bool>{true};
	};
	dependencies.isWorldOccupied = [](const std::filesystem::path&) { return false; };
	dependencies.executeRestore = [](std::stop_token) {
		RestoreResult result;
		result.code = OperationCode::Success;
		return result;
	};
	HotRestoreRequest request;
	request.configId = L"11111111-1111-4111-8111-111111111111";
	request.worldPath = L"world";
	request.fullWorldPath = L"C:\\server\\world";
	request.requestId = "hot-restore-test";
	const auto succeeded = HotRestoreCoordinator(dependencies).Run(request);
	test.Expect(reset && succeeded.code == OperationCode::Success
			&& succeeded.handshake == HotRestoreHandshakeStatus::Compatible
			&& succeeded.saveAndExitCompleted && succeeded.worldReleased
			&& succeeded.rejoin == HotRestoreRejoinStatus::Succeeded,
		"hot restore should complete handshake, release, restore, and rejoin");
	test.Expect(events == std::vector<std::string>{"handshake", "pre_hot_restore",
			"restore_finished", "rejoin_world", "hot_restore_complete"},
		"hot restore should publish the stable lifecycle sequence");

	dependencies.isWorldOccupied = [](const std::filesystem::path&) { return true; };
	HotRestoreTimeouts shortTimeouts;
	shortTimeouts.worldRelease = std::chrono::milliseconds(2);
	shortTimeouts.releasePoll = std::chrono::milliseconds(1);
	const auto occupied = HotRestoreCoordinator(dependencies).Run(
		request, {}, shortTimeouts);
	test.Expect(occupied.code == OperationCode::RestoreFailed
			&& !occupied.worldReleased
			&& std::any_of(occupied.diagnostics.begin(), occupied.diagnostics.end(),
				[](const Diagnostic& item) {
					return item.eventId == "restore.hot.world_release_timeout";
				}),
		"hot restore should refuse to write until the world is released");

	dependencies.isWorldOccupied = [](const std::filesystem::path&) { return false; };
	dependencies.transport.waitRejoin = [](
		std::chrono::milliseconds, std::stop_token) {
		return std::optional<bool>{false};
	};
	const auto rejoinFailed = HotRestoreCoordinator(dependencies).Run(request);
	test.Expect(rejoinFailed.code == OperationCode::Success
			&& rejoinFailed.rejoin == HotRestoreRejoinStatus::Failed,
		"a failed rejoin should preserve the successful restore result");

	dependencies.transport.waitHandshake = [](
		std::chrono::milliseconds, std::stop_token) {
		return HotRestoreHandshakeStatus::TimedOut;
	};
	const auto handshakeTimedOut = HotRestoreCoordinator(dependencies).Run(request);
	test.Expect(handshakeTimedOut.code == OperationCode::RestoreFailed
			&& handshakeTimedOut.handshake == HotRestoreHandshakeStatus::TimedOut
			&& std::any_of(handshakeTimedOut.diagnostics.begin(),
				handshakeTimedOut.diagnostics.end(), [](const Diagnostic& item) {
					return item.eventId == "restore.hot.handshake_timeout";
				}),
		"hot restore should fail before save-and-exit when the handshake times out");

	dependencies.transport.waitHandshake = [](
		std::chrono::milliseconds, std::stop_token) {
		return HotRestoreHandshakeStatus::Compatible;
	};
	dependencies.executeRestore = [](std::stop_token) {
		RestoreResult result;
		result.code = OperationCode::RestoreFailed;
		return result;
	};
	const auto restoreFailed = HotRestoreCoordinator(dependencies).Run(request);
	test.Expect(restoreFailed.code == OperationCode::RestoreFailed
			&& restoreFailed.rejoin == HotRestoreRejoinStatus::NotRequested,
		"hot restore should not request rejoin after a failed filesystem restore");

	std::stop_source cancelledBeforeStart;
	cancelledBeforeStart.request_stop();
	const auto cancelled = HotRestoreCoordinator(dependencies).Run(
		request, cancelledBeforeStart.get_token());
	test.Expect(cancelled.code == OperationCode::Cancelled,
		"hot restore should honor cancellation before changing the world");

	std::stop_source cancelDuringRejoin;
	dependencies.executeRestore = [](std::stop_token) {
		RestoreResult result;
		result.code = OperationCode::Success;
		return result;
	};
	dependencies.transport.waitRejoin = [&cancelDuringRejoin](
		std::chrono::milliseconds, std::stop_token) -> std::optional<bool> {
		cancelDuringRejoin.request_stop();
		return std::nullopt;
	};
	const auto rejoinCancelled = HotRestoreCoordinator(dependencies).Run(
		request, cancelDuringRejoin.get_token());
	test.Expect(rejoinCancelled.code == OperationCode::Success
			&& rejoinCancelled.rejoin == HotRestoreRejoinStatus::Cancelled
			&& std::any_of(rejoinCancelled.diagnostics.begin(),
				rejoinCancelled.diagnostics.end(), [](const Diagnostic& item) {
					return item.eventId == "restore.hot.rejoin_cancelled";
				}),
		"cancellation after restore commit should not misreport the restore as cancelled");
}

} // namespace

void RunRuntimeInfrastructureTests(
    TestContext& test,
    const std::filesystem::path& root) {
    TestTaskCoordinator(test, root);
    TestInterruptedTaskRecovery(test, root);
    TestNetworkService(test, root);
    TestKnotLinkPackageManifest(test);
    TestDesktopServicesAndCapabilities(test);
    TestLoggingCore(test, root);
    TestLoggingStressAndRotation(test, root);
    TestLoggingFailureAndDiagnostics(test, root);
	TestProfileRuntimeReload(test, root);
	TestHotRestoreCoordinator(test);
}
