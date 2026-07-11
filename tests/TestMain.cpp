#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "FolderRewindMetadataStore.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
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

    const auto serialized = FolderRewindHistoryStore::SerializeHistoryItem(config, entry);
    HistoryEntry loaded;
    std::wstring loadedConfigId;
    test.Expect(FolderRewindHistoryStore::TryParseHistoryItem(serialized, loaded, loadedConfigId),
        "serialized history item should parse");
    test.Expect(loadedConfigId == config.configId, "history ConfigId should round-trip");
    test.Expect(loaded.backupFile == entry.backupFile, "history archive name should round-trip");
}

} // namespace

int main() {
    TestContext test;
    TemporaryDirectory temporary;
    TestFormat(test);
    TestMetadataRoundTrip(test, temporary.path);
    TestHistoryRoundTrip(test, temporary.path);

    if (test.failures == 0) {
        std::cout << "[PASS] MineBackup data-core tests\n";
        return 0;
    }
    std::cerr << test.failures << " test assertion(s) failed\n";
    return 1;
}
