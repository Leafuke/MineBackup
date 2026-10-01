#include "HistoryRepositoryTests.h"

#include "HistoryRepository.h"
#include "ChainSafeRetention.h"
#include <barrier>

#include <atomic>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace std;

void RunHistoryRepositoryTests(
    TestContext& test,
    const filesystem::path& root) {
    const filesystem::path historyPath = root / "history-repository.json";
    map<int, Config> configs;
    configs[1].configId = L"config-a";
    configs[2].configId = L"config-b";

    HistoryRepository repository;
    const auto originalSnapshot = repository.Snapshot();
    constexpr int threadCount = 6;
    constexpr int entriesPerThread = 20;
    atomic<bool> mutationsSucceeded{true};
    vector<thread> workers;
    for (int threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        workers.emplace_back([&, threadIndex] {
            const wstring configId = threadIndex % 2 == 0 ? L"config-a" : L"config-b";
            for (int entryIndex = 0; entryIndex < entriesPerThread; ++entryIndex) {
                HistoryEntry entry;
                entry.configId = configId;
                entry.worldName = L"world-" + to_wstring(threadIndex);
                entry.backupFile = L"backup-" + to_wstring(entryIndex) + L"-"
                    + to_wstring(threadIndex) + L".7z";
                entry.timestamp_str = L"2026-08-07T00:00:00";
                const auto result = repository.Mutate(
                    configId,
                    historyPath,
                    configs,
                    false,
                    [&](vector<HistoryEntry>& entries) {
                        entries.push_back(entry);
                        return true;
                    });
                if (!result.changed || !result.persisted) {
                    mutationsSucceeded = false;
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();

    test.Expect(mutationsSucceeded.load(),
        "Concurrent history mutation should publish successfully");

    test.Expect(originalSnapshot->revision == 0
            && originalSnapshot->byConfigId.empty(),
        "Published history snapshots must remain immutable");
    const auto finalSnapshot = repository.Snapshot();
    size_t entryCount = 0;
    for (const auto& pair : finalSnapshot->byConfigId) {
        entryCount += pair.second->size();
    }
    test.Expect(entryCount == threadCount * entriesPerThread,
        "Concurrent history mutations must not lose entries");
    test.Expect(finalSnapshot->revision == threadCount * entriesPerThread,
        "Each history mutation should advance the snapshot revision");

    test.Expect(repository.Save(historyPath, configs),
        "History repository should persist the latest snapshot");
    HistoryRepository reloaded;
    test.Expect(reloaded.Load(historyPath, configs),
        "Persisted history should load successfully");
    size_t reloadedCount = 0;
    for (const auto& pair : reloaded.Snapshot()->byConfigId) {
        reloadedCount += pair.second->size();
    }
    test.Expect(reloadedCount == entryCount,
        "Persisted history should contain every concurrent entry");
    Config config; config.configId = L"retention"; config.saveRoot = (root / "worlds").wstring();
    config.backupPath = (root / "archives").wstring();
    config.worlds = {{L"one", L""}, {L"two", L""}};
    HistoryEntry target; target.configId = config.configId; target.worldName = L"one";
    target.worldPath = (root / "worlds" / "one").wstring(); target.backupFile = L"old.7z";
    auto tail = target; tail.backupFile = L"tail.7z";
    auto other = target; other.worldName = L"two"; other.worldPath = (root / "worlds" / "two").wstring(); other.backupFile = L"other.7z";
    HistoryRepository concurrent;
    map<int, Config> retentionConfigs{{1, config}};
    const auto path = root / "retention-history.json";
    concurrent.ReplaceAll({{config.configId, {target, tail}}}, path, retentionConfigs, false);
    ChainSafeRetention::HistoryChanges changes{{target}, {{tail, L"promoted.7z", L"Full"}}};
    barrier rendezvous(2);
    jthread writer([&] {
        rendezvous.arrive_and_wait();
        concurrent.Mutate(config.configId, path, retentionConfigs, false, [&](auto& entries) {
            entries.push_back(other); entries[1].comment = L"new comment"; entries[1].isImportant = true;
            entries[1].isCloudArchived = true; entries[1].cloudArchiveRemotePath = L"remote"; return true;
        });
        rendezvous.arrive_and_wait();
    });
    rendezvous.arrive_and_wait(); rendezvous.arrive_and_wait();
    const auto merged = concurrent.Mutate(config.configId, path, retentionConfigs, true, [&](auto& entries) {
        return ChainSafeRetention::ApplyHistoryChanges(config, entries, changes);
    });
    const auto latest = concurrent.EntriesForConfig(config.configId);
    test.Expect(merged.changed && merged.persisted && latest->size() == 2 && latest->at(1).backupFile == other.backupFile,
        "retention must preserve another world's concurrently inserted history");
    test.Expect(latest->at(0).backupFile == L"promoted.7z" && latest->at(0).isImportant && latest->at(0).isCloudArchived
        && latest->at(0).comment == L"new comment" && latest->at(0).cloudArchiveRemotePath == L"remote",
        "archive rename preserves latest annotations and cloud state");
    auto important = target; important.isImportant = true;
    vector<HistoryEntry> conflict{important, tail};
    test.Expect(!ChainSafeRetention::ApplyHistoryChanges(config, conflict, changes) && conflict.size() == 2,
        "concurrent important marking must reject the entire retention change set");
    conflict = {target, tail}; conflict[0].worldPath += L"changed";
    test.Expect(!ChainSafeRetention::ApplyHistoryChanges(config, conflict, changes) && conflict[1].backupFile == L"tail.7z",
        "identity changes must reject rename and deletion together");
    filesystem::create_directories(root / "blocked-history");
    concurrent.ReplaceAll({{config.configId, {target, tail}}}, path, retentionConfigs, false);
    const auto failed = concurrent.Mutate(config.configId, root / "blocked-history", retentionConfigs, true, [&](auto& entries) {
        return ChainSafeRetention::ApplyHistoryChanges(config, entries, changes);
    });
    test.Expect(failed.changed && !failed.persisted && concurrent.EntriesForConfig(config.configId)->size() == 2,
        "history persistence failure must leave the published latest snapshot unchanged");

}
