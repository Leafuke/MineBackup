#pragma once

#include "DataModels.h"
#include "AtomicFileWriter.h"
#include "FolderRewindHistoryStore.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    // A post-replacement sync failure is not a safe-to-rollback write failure.
    bool committed = false;
};

struct ImportanceResult {
    bool success = false;
    std::wstring file;
    bool important = false;
    std::string error;
    std::optional<HistoryEntry> entry;
};

class HistoryRepository {
public:
    using Entries = HistorySnapshot::Entries;
    using EntriesView = HistorySnapshot::EntriesView;
    using Mutator = std::function<bool(Entries&)>;

    using Writer = std::function<AtomicFileWriter::WriteResult(
        const std::filesystem::path&, const std::string&)>;
    explicit HistoryRepository(Writer writer = {});

    // The single-instance profile owns local archive mutations. Serialize the
    // entire compaction transaction, including filesystem changes, with pins.
    static std::recursive_mutex& ArchiveMutationMutex();
    ImportanceResult QueryImportance(const Config& config, const std::wstring& world,
        const std::wstring& file) const;
    ImportanceResult SetImportance(const Config& config, const std::wstring& world,
        const std::wstring& file, bool important, const std::filesystem::path& historyFile,
        const std::map<int, Config>& configs);

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

    Writer writer_;
    mutable std::mutex mutex_;
    std::shared_ptr<const HistorySnapshot> snapshot_;
};

