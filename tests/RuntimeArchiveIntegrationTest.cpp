#include "ArchiveRunner.h"
#include "FolderRewindFormat.h"
#include "ProfileManifest.h"
#include "ProfileRuntime.h"
#include "text_to_text.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

class ArchiveIntegrationTest {
public:
    int checks = 0;
    int failures = 0;

    void Expect(bool condition, const std::string& message) {
        ++checks;
        if (!condition) {
            ++failures;
            std::cerr << "[FAIL] " << message << '\n';
        }
    }

    void Diagnostics(const std::vector<Diagnostic>& diagnostics) {
        for (const auto& diagnostic : diagnostics) {
            if (diagnostic.severity == DiagnosticSeverity::Error)
                std::cerr << "[DETAIL] " << diagnostic.eventId << ": " << diagnostic.detail << '\n';
        }
    }
};

fs::path Utf8Path(const std::string& value) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) throw std::runtime_error("Could not write integration fixture");
}

std::string ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read integration fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

using FileSnapshot = std::map<fs::path, std::string>;

FileSnapshot Snapshot(const fs::path& world) {
    FileSnapshot snapshot;
    for (const auto& entry : fs::recursive_directory_iterator(world)) {
        if (entry.is_regular_file() && entry.path().filename() != "session.lock"
                && entry.path().filename() != "operator-note.txt") {
            snapshot[entry.path().lexically_relative(world)] = ReadFile(entry.path());
        }
    }
    return snapshot;
}

struct Fixture {
    AppPaths paths;
    ServerProfileManifest manifest;
    fs::path world;

    Fixture(const fs::path& root, const fs::path& sevenZip,
        const std::string& worldName = "world", const std::string& saveName = "server",
        const std::string& backupName = "backups") {
        AppPathRequest request;
        request.dataDirectory = root / "profile";
        std::wstring error;
        if (!ResolveAppPaths(request, GetExecutablePath(), paths, error))
            throw std::runtime_error(wstring_to_utf8(error));
        manifest = ProfileManifest::CreateTemplate();
        auto& config = manifest.configs.front();
        config.saveRoot = (root / Utf8Path(saveName)).wstring();
        config.backupPath = (root / Utf8Path(backupName)).wstring();
        config.zipPath = sevenZip.wstring();
        config.zipFormat = L"7z";
        config.zipMethod = L"LZMA2";
        config.zipLevel = 1;
        config.cpuThreads = 1;
        config.keepCount = 0;
        config.backupMode = 2;
        config.maxSmartBackupsPerFull = 5;
        config.backupBefore = false;
        config.useLowPriority = false;
        config.skipIfUnchanged = true;
        config.cloudSyncEnabled = false;
        config.blacklist.clear();
        config.worlds = {{utf8_to_wstring(worldName), L"Integration world"}};
        for (auto& job : manifest.jobs.jobs) {
            for (auto& stage : job.stages) {
                for (auto& step : stage.steps) {
                    if (step.type == JobStepType::Backup) {
                        step.backup.configId = config.configId;
                        step.backup.worldPath = utf8_to_wstring(worldName);
                    }
                }
            }
        }
        world = root / Utf8Path(saveName) / Utf8Path(worldName);
        WriteFile(world / "level.dat", std::string(10000, 'A'));
        WriteFile(world / "region" / "r.0.0.mca", std::string(20000, 'B'));
    }

    const Config& Configuration() const { return manifest.configs.front(); }
    const std::wstring& WorldName() const { return Configuration().worlds.front().first; }

    bool Apply(ArchiveIntegrationTest& test) {
        const auto plan = ProfileManifest::Plan(paths, manifest, false);
        test.Expect(plan.code == OperationCode::Success, "manifest plan succeeds");
        if (plan.code != OperationCode::Success) return false;
        const auto result = ProfileManifest::Apply(paths, plan);
        test.Expect(result.code == OperationCode::Success, "manifest apply succeeds");
        if (result.code != OperationCode::Success) return false;
        const auto exported = ProfileManifest::Export(paths);
        test.Expect(exported.IsLoaded(), "manifest export succeeds");
        if (exported.IsLoaded()) {
            test.Expect(ProfileManifest::Plan(paths, exported.manifest, false).code == OperationCode::Success,
                "exported manifest can be planned again");
        }
        return true;
    }

    RestoreRequest RestoreFrom(const fs::path& archive) const {
        RestoreRequest request;
        request.config = Configuration();
        request.world = {Configuration().configId, WorldName()};
        request.archive = archive;
        request.restorePreserve = {L"session.lock", L"operator-note.txt"};
        return request;
    }
};

void TestRoundTrip(ArchiveIntegrationTest& test, Fixture& fixture, bool unicodeFile) {
    if (unicodeFile) WriteFile(fixture.world / Utf8Path("你好世界.txt"), "Unicode fixture contents");
    if (!fixture.Apply(test)) return;
    ProfileRuntime runtime(fixture.paths, {.noNetwork = true});
    const auto initialized = runtime.Reload();
    test.Expect(initialized.code == OperationCode::Success, "runtime reload succeeds");
    if (initialized.code != OperationCode::Success) return;
    const auto& id = fixture.Configuration().configId;
    const auto full = runtime.RunBackup(id, fixture.WorldName(), L"Full integration", {}, true);
    test.Expect(full.code == OperationCode::Success && fs::is_regular_file(full.archivePath),
        "real Full archive is created");
    test.Diagnostics(full.diagnostics);
    if (full.code != OperationCode::Success) return;
    test.Expect(full.historyEntry && full.historyEntry->backupType == L"Full", "first history entry is Full");
    test.Expect(fs::is_regular_file(fixture.paths.HistoryFile()), "history is persisted");
    test.Expect(runtime.RunBackup(id, fixture.WorldName(), {}, {}, true).code == OperationCode::NoChanges,
        "unchanged world returns no_changes");

    WriteFile(fixture.world / "level.dat", std::string(9000, 'C'));
    fs::remove(fixture.world / "region" / "r.0.0.mca");
    WriteFile(fixture.world / "region" / "r.1.0.mca", std::string(12000, 'D'));
    const auto expected = Snapshot(fixture.world);
    const auto smart = runtime.RunBackup(id, fixture.WorldName(), L"Smart integration", {}, true);
    test.Expect(smart.code == OperationCode::Success && fs::is_regular_file(smart.archivePath),
        "real Smart archive is created");
    test.Diagnostics(smart.diagnostics);
    if (smart.code != OperationCode::Success) return;
    test.Expect(smart.historyEntry && smart.historyEntry->backupType == L"Smart", "delta history entry is Smart");
    auto request = fixture.RestoreFrom(smart.archivePath);
    const auto verified = runtime.Verify(request);
    test.Expect(verified.code == OperationCode::Success && verified.checkedArchiveCount == 2,
        "7zz verifies the complete Full plus Smart chain");
    test.Diagnostics(verified.diagnostics);

    WriteFile(fixture.world / "level.dat", "corrupted");
    WriteFile(fixture.world / "extra.txt", "remove during clean restore");
    WriteFile(fixture.world / "operator-note.txt", "preserve this note");
    const auto beforeDryRun = Snapshot(fixture.world);
    const auto dryRun = runtime.Restore(request, true, {}, true);
    test.Expect(dryRun.code == OperationCode::Success && Snapshot(fixture.world) == beforeDryRun,
        "dry run leaves all world bytes unchanged");
    const auto restored = runtime.Restore(request, false, {}, true);
    test.Expect(restored.code == OperationCode::Success, "real clean restore succeeds");
    test.Diagnostics(restored.diagnostics);
    test.Expect(Snapshot(fixture.world) == expected,
        "clean restore reproduces exact bytes, additions and deletions without extras");
    test.Expect(ReadFile(fixture.world / "operator-note.txt") == "preserve this note",
        "clean restore preserves the requested local note");

    WriteFile(fixture.world / "level.dat", "broken again");
    WriteFile(fixture.world / "extra.txt", "keep during overwrite");
    request.mode = RestoreMode::Overwrite;
    const auto overwritten = runtime.Restore(request, false, {}, true);
    test.Expect(overwritten.code == OperationCode::Success
            && ReadFile(fixture.world / "level.dat") == expected.at("level.dat")
            && ReadFile(fixture.world / "extra.txt") == "keep during overwrite",
        "overwrite restore recovers data while retaining unarchived files");
    test.Diagnostics(overwritten.diagnostics);

    const auto archiveBytes = ReadFile(smart.archivePath);
    WriteFile(smart.archivePath, "invalid archive");
    const auto beforeCorruptRestore = Snapshot(fixture.world);
    test.Expect(runtime.Verify(request).code != OperationCode::Success, "corrupt archive fails verification");
    request.mode = RestoreMode::Clean;
    const auto corruptRestore = runtime.Restore(request, false, {}, true);
    test.Expect(corruptRestore.code != OperationCode::Success && Snapshot(fixture.world) == beforeCorruptRestore,
        "a corrupt archive cannot mutate the existing world");
    WriteFile(smart.archivePath, archiveBytes);
}

std::string BigEndian(unsigned value, unsigned bytes) {
    std::string output(bytes, '\0');
    for (unsigned index = 0; index < bytes; ++index)
        output[bytes - index - 1] = static_cast<char>(value >> (index * 8));
    return output;
}

std::string NbtTag(unsigned char type, const std::string& name, const std::string& payload) {
    return std::string(1, static_cast<char>(type)) + BigEndian(static_cast<unsigned>(name.size()), 2) + name + payload;
}

std::string LevelData(unsigned worldTime) {
    const auto data = NbtTag(3, "DataVersion", BigEndian(4671, 4))
        + NbtTag(3, "Time", BigEndian(worldTime, 4)) + '\0';
    return NbtTag(10, "", NbtTag(10, "Data", data) + '\0');
}

std::string PlayerData(unsigned playerLevel, unsigned unselectedValue) {
    const auto text = "rollback-" + std::to_string(unselectedValue);
    const auto item = NbtTag(1, "Slot", std::string(1, '\0'))
        + NbtTag(3, "count", BigEndian(playerLevel, 4)) + '\0';
    const std::string uuid("\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff", 16);
    return NbtTag(10, "", NbtTag(9, "Inventory", std::string(1, 10) + BigEndian(1, 4) + item)
        + NbtTag(11, "UUID", BigEndian(4, 4) + uuid)
        + NbtTag(8, "Unselected", BigEndian(static_cast<unsigned>(text.size()), 2) + text)
        + NbtTag(3, "XpLevel", BigEndian(playerLevel, 4)) + '\0');
}

void TestRealPlayerPreservation(ArchiveIntegrationTest& test, Fixture& fixture) {
    auto runner = ArchiveRunner::Resolve(fs::path(fixture.Configuration().zipPath), fixture.paths);
    const auto player = fixture.world / "playerdata" / "00112233-4455-6677-8899-aabbccddeeff.dat";
    auto writeGzip = [&](const fs::path& destination, const std::string& bytes) {
        const auto raw = fixture.paths.runtimeRoot / "input.nbt";
        WriteFile(raw, bytes);
        const auto gzip = runner.Execute({L"a", L"-tgzip", L"-so", L"-an", L"--", raw.wstring()}, {}, false,
            1024 * 1024, std::chrono::seconds(15));
        if (gzip.status != ProcessStatus::Succeeded) throw std::runtime_error("Real gzip fixture creation failed");
        WriteFile(destination, gzip.standardOutput);
    };
    auto decoded = [&](const fs::path& source) {
        const auto result = runner.Execute({L"x", L"-tgzip", L"-so", L"--", source.wstring()},
            {}, false, 1024 * 1024, std::chrono::seconds(15));
        if (result.status != ProcessStatus::Succeeded) throw std::runtime_error("Real gzip fixture decode failed");
        return result.standardOutput;
    };
    writeGzip(fixture.world / "level.dat", LevelData(100));
    writeGzip(player, PlayerData(2, 2));
    WriteFile(fixture.world / "ftbquests" / "same.txt", "backup quest");
    WriteFile(fixture.world / "ftbquests" / "deleted.txt", "deleted after backup");
    WriteFile(fixture.world / "ftbteams" / "team.txt", "backup team");
    if (!fixture.Apply(test)) return;
    ProfileRuntime runtime(fixture.paths, {.noNetwork = true});
    test.Expect(runtime.Reload().code == OperationCode::Success, "player fixture loads");
    const auto backup = runtime.RunBackup(fixture.Configuration().configId, fixture.WorldName(), {}, {}, true);
    test.Expect(backup.code == OperationCode::Success, "real gzip NBT and FTB fixture is backed up");
    if (backup.code != OperationCode::Success) return;
    writeGzip(fixture.world / "level.dat", LevelData(200));
    writeGzip(player, PlayerData(9, 9));
    WriteFile(fixture.world / "region" / "r.0.0.mca", "current region must roll back");
    WriteFile(fixture.world / "ftbquests" / "same.txt", "current quest");
    WriteFile(fixture.world / "ftbquests" / "new.txt", "new quest");
    fs::remove(fixture.world / "ftbquests" / "deleted.txt");
    WriteFile(fixture.world / "ftbteams" / "team.txt", "current team");
    auto request = fixture.RestoreFrom(backup.archivePath);
    request.preservePlayerData = true;
    request.restorePreservePaths = {L"ftbquests/", L"ftbteams/"};
    const auto restored = runtime.Restore(request, false, {}, true);
    test.Expect(restored.code == OperationCode::Success, "real clean archive restore preserves player and FTB state");
    test.Diagnostics(restored.diagnostics);
    test.Expect(decoded(player) == PlayerData(9, 2),
        "real gzip roundtrip keeps current UUID player inventory and XP while unselected fields roll back");
    test.Expect(decoded(fixture.world / "level.dat") == LevelData(100), "world time rolls back independently of player preservation");
    test.Expect(ReadFile(fixture.world / "region" / "r.0.0.mca") == std::string(20000, 'B'),
        "player-preserving restore still restores archived region bytes");
    test.Expect(ReadFile(fixture.world / "ftbquests" / "same.txt") == "current quest"
            && ReadFile(fixture.world / "ftbquests" / "new.txt") == "new quest"
            && !fs::exists(fixture.world / "ftbquests" / "deleted.txt")
            && ReadFile(fixture.world / "ftbteams" / "team.txt") == "current team",
        "operation-scoped FTB directory preservation keeps current changes, additions and deletions");

    request.preservePlayerData = false;
    request.restorePreservePaths.clear();
    const auto ordinary = runtime.Restore(request, false, {}, true);
    test.Expect(ordinary.code == OperationCode::Success, "explicitly disabled player preservation restores normally");
    test.Diagnostics(ordinary.diagnostics);
    test.Expect(decoded(player) == PlayerData(2, 2), "preservePlayerData=false restores archived inventory and XP");
    test.Expect(ReadFile(fixture.world / "ftbquests" / "same.txt") == "backup quest"
            && fs::is_regular_file(fixture.world / "ftbquests" / "deleted.txt")
            && !fs::exists(fixture.world / "ftbquests" / "new.txt")
            && ReadFile(fixture.world / "ftbteams" / "team.txt") == "backup team",
        "operation-scoped FTB preservation does not leak into later restores");
}

void TestDeletionAndJob(ArchiveIntegrationTest& test, Fixture& fixture) {
    if (!fixture.Apply(test)) return;
    ProfileRuntime runtime(fixture.paths, {.noNetwork = true});
    test.Expect(runtime.Reload().code == OperationCode::Success, "deletion fixture loads");
    const auto& id = fixture.Configuration().configId;
    const auto full = runtime.RunBackup(id, fixture.WorldName(), {}, {}, true);
    test.Expect(full.code == OperationCode::Success, "deletion fixture Full backup succeeds");
    if (full.code != OperationCode::Success) return;
    fs::remove(fixture.world / "region" / "r.0.0.mca");
    const auto deletion = runtime.RunBackup(id, fixture.WorldName(), {}, {}, true);
    test.Expect(deletion.code == OperationCode::Success, "deletion-only Smart archive succeeds");
    if (deletion.code != OperationCode::Success) return;
    WriteFile(fixture.world / "region" / "r.0.0.mca", "wrong regenerated data");
    const auto restored = runtime.Restore(fixture.RestoreFrom(deletion.archivePath), false, {}, true);
    test.Expect(restored.code == OperationCode::Success && !fs::exists(fixture.world / "region" / "r.0.0.mca"),
        "deletion-only Smart restore removes the deleted file");
    test.Expect(IsSuccessful(runtime.RunJob(fixture.manifest.jobs.jobs.front().jobId, {}, true).code),
        "manifest backup Job runs through the real runtime");
}

#ifndef _WIN32
void TestExcludedLinuxPaths(ArchiveIntegrationTest& test, Fixture& fixture, bool symlink) {
    fixture.manifest.configs.front().blacklist = {L"cache"};
    if (symlink) {
        const auto outside = fixture.world.parent_path().parent_path() / "outside.txt";
        WriteFile(outside, "outside contents must not enter the archive");
        fs::create_directories(fixture.world / "cache");
        fs::create_symlink(outside, fixture.world / "cache" / "linked.txt");
    }
    else {
        WriteFile(fixture.world / "cache" / "namespace:data.txt", "excluded Linux filename");
    }
    if (!fixture.Apply(test)) return;
    ProfileRuntime runtime(fixture.paths, {.noNetwork = true});
    test.Expect(runtime.Reload().code == OperationCode::Success, "excluded-path fixture loads");
    const auto backup = runtime.RunBackup(fixture.Configuration().configId, fixture.WorldName(), {}, {}, true);
    test.Expect(backup.code == OperationCode::Success,
        "excluded unsafe Linux path must not abort the world backup");
    test.Diagnostics(backup.diagnostics);
    if (backup.code != OperationCode::Success) return;
    test.Expect(runtime.Verify(fixture.RestoreFrom(backup.archivePath)).code == OperationCode::Success,
        "archive from excluded-path fixture verifies");
    auto runner = ArchiveRunner::Resolve(fs::path(fixture.Configuration().zipPath), fixture.paths);
    const auto listing = runner.Execute({L"l", L"-slt", backup.archivePath.wstring()}, {}, false,
        1024 * 1024, std::chrono::seconds(15));
    test.Expect(listing.status == ProcessStatus::Succeeded && listing.standardOutput.find("Path = cache") == std::string::npos,
        "excluded directory contents are absent from the real archive");
}
#endif

fs::path CreateFixtureRoot(const fs::path& requestedRoot) {
#ifdef _WIN32
    // A checkout-relative root plus archive GUIDs exceeded MAX_PATH in Windows
    // CI. Exercise the same workflows under a compact system-temp root; this
    // test does not assert production support for extended-length Win32 paths.
    const auto base = fs::canonical(fs::temp_directory_path());
    // Leave room for scenario, staging GUID and generated archive names. Fail
    // setup explicitly on unusually long TEMP/TMP roots rather than skipping
    // any archive assertions or reporting misleading backup failures.
    constexpr std::size_t maximumRootLength = 64;
    if (base.native().size() + 1 + 16 > maximumRootLength)
        throw std::runtime_error("Windows archive integration requires a shorter TEMP/TMP directory");
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const auto id = FolderRewindFormat::GenerateGuidString();
        const auto root = base / (L"mbi-" + id.substr(0, 8) + id.substr(9, 4));
        std::error_code error;
        if (fs::create_directory(root, error)) return root;
        if (error) throw fs::filesystem_error("Could not create archive integration fixture root", root, error);
    }
    throw std::runtime_error("Could not allocate a unique archive integration fixture root");
#else
    return requestedRoot / (L"runtime-archive-" + FolderRewindFormat::GenerateGuidString());
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: minebackup_runtime_archive_integration_tests <7zz-path> <temporary-work-root>\n";
        return 2;
    }
    const auto sevenZip = fs::absolute(argv[1]);
    // Only our unique fixture child is removed, never either parent directory.
    const auto requestedRoot = fs::absolute(argv[2]);
    fs::path root;
    ArchiveIntegrationTest test;
    struct Case { const char* name; const char* world; const char* saves; const char* backups; };
    const std::vector<Case> cases = {
        {"ascii", "world", "server", "backups"},
        {"unicode-world", "我的世界", "server", "backups"},
        {"unicode-save", "world", "游戏 存档", "backups"},
        {"unicode-backup", "world", "server", "备份 目录"},
        {"unicode-file", "world", "server", "backups"},
        {"spaces", "world name", "server folder", "backup folder"}
    };
    try {
        root = CreateFixtureRoot(requestedRoot);
        std::cout << "[ENV] 7-Zip executable: " << sevenZip << '\n';
        std::cout << "[ENV] temporary fixture root: " << root << '\n';
        std::cout << "[ENV] requested work root: " << requestedRoot << '\n';
        for (const auto& item : cases) {
            std::cout << "[SCENARIO] " << item.name << '\n';
            Fixture fixture(root / item.name, sevenZip, item.world, item.saves, item.backups);
            TestRoundTrip(test, fixture, std::string(item.name) == "unicode-file");
        }
        Fixture player(root / "real-player-preservation", sevenZip);
        TestRealPlayerPreservation(test, player);
        Fixture deletion(root / "deletion-and-job", sevenZip);
        TestDeletionAndJob(test, deletion);
#ifndef _WIN32
        Fixture excludedName(root / "excluded-linux-name", sevenZip);
        TestExcludedLinuxPaths(test, excludedName, false);
        Fixture excludedSymlink(root / "excluded-symlink", sevenZip);
        TestExcludedLinuxPaths(test, excludedSymlink, true);
#endif
    }
    catch (const std::exception& exception) {
        test.Expect(false, std::string("integration exception: ") + exception.what());
    }
    std::cout << test.checks << " assertions; " << test.failures << " failures\n";
    if (test.failures == 0 && !root.empty()) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    else if (!root.empty()) {
        std::cerr << "Fixtures retained at " << root << '\n';
    }
    return test.failures ? 1 : 0;
}
