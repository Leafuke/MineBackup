#pragma once

#include "DataModels.h"
#include "FolderRewindHistoryStore.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct HistorySnapshot {
    using Entries = std::vector<HistoryEntry>;
    using EntriesView = std::shared_ptr<const Entries>;

    std::uint64_t revision = 0;
    std::map<std::wstring, EntriesView> byConfigId;
};

struct HistoryMutationResult {
    bool changed = false;
    bool persisted = true;
};

class HistoryRepository {
public:
    using Entries = HistorySnapshot::Entries;
    using EntriesView = HistorySnapshot::EntriesView;
    using Mutator = std::function<bool(Entries&)>;

    HistoryRepository();

    std::shared_ptr<const HistorySnapshot> Snapshot() const;
    EntriesView EntriesForConfig(const std::wstring& configId) const;

    bool Load(
        const std::filesystem::path& path,
        const std::map<int, Config>& configs);
    bool ReplaceAll(
        FolderRewindHistoryStore::HistoryByConfigId history,
        const std::filesystem::path& path,
        const std::map<int, Config>& configs,
        bool persist);
    bool Save(
        const std::filesystem::path& path,
        const std::map<int, Config>& configs) const;

    HistoryMutationResult Mutate(
        const std::wstring& configId,
        const std::filesystem::path& path,
        const std::map<int, Config>& configs,
        bool persist,
        const Mutator& mutator);

private:
    static FolderRewindHistoryStore::HistoryByConfigId Flatten(
        const HistorySnapshot& snapshot);
    static std::shared_ptr<HistorySnapshot> MakeSnapshot(
        FolderRewindHistoryStore::HistoryByConfigId history,
        std::uint64_t revision);

    mutable std::mutex mutex_;
    std::shared_ptr<const HistorySnapshot> snapshot_;
};

