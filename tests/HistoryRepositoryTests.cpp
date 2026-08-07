#include "HistoryRepositoryTests.h"

#include "HistoryRepository.h"

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
}
