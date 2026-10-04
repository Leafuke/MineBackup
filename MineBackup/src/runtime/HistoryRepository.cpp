#include "HistoryRepository.h"
#include "FolderRewindFormat.h"
#include "WorldIdentity.h"
#include "PathIdentity.h"
#include "AtomicFileWriter.h"

using namespace std;

HistoryRepository::HistoryRepository(Writer writer)
    : writer_(writer ? std::move(writer) : [](const auto& path, const auto& content) {
        return AtomicFileWriter::WriteText(path, content);
    }), snapshot_(make_shared<HistorySnapshot>()) {}

shared_ptr<const HistorySnapshot> HistoryRepository::Snapshot() const {
    lock_guard<mutex> lock(mutex_);
    return snapshot_;
}

HistoryRepository::EntriesView HistoryRepository::EntriesForConfig(
    const wstring& configId) const {
    const auto snapshot = Snapshot();
    const auto it = snapshot->byConfigId.find(configId);
    if (it != snapshot->byConfigId.end()) return it->second;
    static const EntriesView empty = make_shared<const Entries>();
    return empty;
}

bool HistoryRepository::Load(
    const filesystem::path& path,
    const map<int, Config>& configs) {
    unique_lock operation(ArchiveMutationMutex(), try_to_lock);
    if (!operation.owns_lock()) return false;
    FolderRewindHistoryStore::HistoryByConfigId loaded;
    if (!FolderRewindHistoryStore::LoadHistoryFileByConfigId(path, configs, loaded)) {
        return false;
    }

    lock_guard<mutex> lock(mutex_);
    snapshot_ = MakeSnapshot(std::move(loaded), snapshot_->revision + 1);
    return true;
}

bool HistoryRepository::ReplaceAll(
    FolderRewindHistoryStore::HistoryByConfigId history,
    const filesystem::path& path,
    const map<int, Config>& configs,
    bool persist) {
    unique_lock operation(ArchiveMutationMutex(), try_to_lock);
    if (!operation.owns_lock()) return false;
    lock_guard<mutex> lock(mutex_);
    auto next = MakeSnapshot(std::move(history), snapshot_->revision + 1);
    if (persist && !FolderRewindHistoryStore::SaveHistoryFileByConfigId(
            path, configs, Flatten(*next))) {
        return false;
    }
    snapshot_ = std::move(next);
    return true;
}

bool HistoryRepository::Save(
    const filesystem::path& path,
    const map<int, Config>& configs) const {
    lock_guard<mutex> lock(mutex_);
    return FolderRewindHistoryStore::SaveHistoryFileByConfigId(
        path, configs, Flatten(*snapshot_));
}

HistoryMutationResult HistoryRepository::Mutate(
    const wstring& configId,
    const filesystem::path& path,
    const map<int, Config>& configs,
    bool persist,
    const Mutator& mutator) {
    if (configId.empty() || !mutator) return {};
    lock_guard<mutex> lock(mutex_);
    auto next = make_shared<HistorySnapshot>(*snapshot_);
    Entries entries;
    const auto current = snapshot_->byConfigId.find(configId);
    if (current != snapshot_->byConfigId.end()) entries = *current->second;
    if (!mutator(entries)) return {};

    next->revision = snapshot_->revision + 1;
    next->byConfigId[configId] = make_shared<const Entries>(std::move(entries));
    if (persist) {
        const auto write = writer_(path,
            FolderRewindHistoryStore::SerializeHistoryFileByConfigId(configs, Flatten(*next)));
        if (!write.WasReplaced()) return {true, false, false};
        snapshot_ = std::move(next);
        return {true, write.IsDurable(), true};
    }
    snapshot_ = std::move(next);
    return {true, true, false};
}

FolderRewindHistoryStore::HistoryByConfigId HistoryRepository::Flatten(
    const HistorySnapshot& snapshot) {
    FolderRewindHistoryStore::HistoryByConfigId flattened;
    for (const auto& pair : snapshot.byConfigId) {
        flattened.emplace(pair.first, *pair.second);
    }
    return flattened;
}

shared_ptr<HistorySnapshot> HistoryRepository::MakeSnapshot(
    FolderRewindHistoryStore::HistoryByConfigId history,
    uint64_t revision) {
    auto snapshot = make_shared<HistorySnapshot>();
    snapshot->revision = revision;
    for (auto& pair : history) {
        snapshot->byConfigId.emplace(
            std::move(pair.first),
            make_shared<const Entries>(std::move(pair.second)));
    }
    return snapshot;
}


recursive_mutex& HistoryRepository::ArchiveMutationMutex() {
    static recursive_mutex mutex;
    return mutex;
}

ImportanceResult HistoryRepository::QueryImportance(const Config& config,
    const wstring& world, const wstring& file) const {
    ImportanceResult result;
    unique_lock operation(ArchiveMutationMutex(), try_to_lock);
    if (!operation.owns_lock()) { result.error = "Archive operation is busy."; return result; }
    if (!FolderRewindFormat::IsSafeSinglePathSegment(file)) {
        result.error = "file must be an exact archive filename."; return result;
    }
    for (const auto& entry : *EntriesForConfig(config.configId)) {
        if (!WorldIdentity::Matches(config, world, entry, file)) continue;
        if (result.entry) { result.error = "Backup history identity is ambiguous."; return result; }
        result.entry = entry;
    }
    if (!result.entry) { result.error = "Backup history entry was not found."; return result; }
    WorldIdentity::Value identity, recordedIdentity;
    FolderRewindFormat::StoragePaths storage, recordedStorage;
    if (!WorldIdentity::TryBuild(config, world, identity)
        || !WorldIdentity::TryResolveHistory(config, *result.entry, recordedIdentity)
        || !PathIdentity::PathsEqual(identity.sourcePath, recordedIdentity.sourcePath)
        || !FolderRewindFormat::TryResolveStoragePaths(config.backupPath,
            identity.relativeWorldPath, identity.sourcePath.wstring(), storage)
        || !FolderRewindFormat::TryResolveStoragePaths(config.backupPath,
            result.entry->worldName, result.entry->worldPath, recordedStorage)
        || !PathIdentity::PathsEqual(storage.backupSubDir, recordedStorage.backupSubDir)) {
        result.error = "Backup source and recorded storage identities do not agree."; return result;
    }
    error_code error;
    const auto archive = storage.backupSubDir / file;
    if (!filesystem::is_regular_file(filesystem::symlink_status(archive, error)) || error) {
        result.error = "The exact local archive is missing or unsafe."; return result;
    }
    result.success = true;
    result.file = result.entry->backupFile;
    result.important = result.entry->isImportant;
    return result;
}

ImportanceResult HistoryRepository::SetImportance(const Config& config, const wstring& world,
    const wstring& file, bool important, const filesystem::path& historyFile,
    const map<int, Config>& configs) {
    unique_lock operation(ArchiveMutationMutex(), try_to_lock);
    if (!operation.owns_lock()) {
        ImportanceResult busy; busy.error = "Archive operation is busy."; return busy;
    }
    auto result = QueryImportance(config, world, file);
    if (!result.success) return result;
    // Persist even an idempotent request, so an acknowledgement always confirms
    // the flag reached the same authoritative history file used after restart.
    const auto mutation = Mutate(config.configId, historyFile, configs, true,
        [&](Entries& entries) {
            for (auto& entry : entries) {
                if (WorldIdentity::SameHistoryEntry(config, entry, *result.entry)) {
                    entry.isImportant = important;
                    return true;
                }
            }
            return false;
        });
    if (!mutation.changed || !mutation.persisted) {
        result.success = false;
        if (mutation.committed) { result.important = important; result.entry->isImportant = important; }
        result.error = mutation.committed
            ? "Importance committed, but history durability could not be confirmed."
            : "Importance flag could not be committed to history.";
        return result;
    }
    result.important = important;
    result.entry->isImportant = important;
    return result;
}
