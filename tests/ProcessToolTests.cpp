#include "AtomicFileWriter.h"
#include "AppPaths.h"
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

#include "ProcessToolTests.h"

namespace {

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
    const std::string manifestText = ReadText(manifestPath);
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
    const std::string assetBytes = ReadText(asset);
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

} // namespace

void RunProcessToolTests(
    TestContext& test,
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    TestProcessRunner(test, executable, root);
    TestSevenZipArgumentVector(test, root);
    TestExternalToolManager(test, root, executable);
    TestPortableConfigDocument(test);
}
