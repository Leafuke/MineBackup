#include "HistoryRepository.h"

using namespace std;

HistoryRepository::HistoryRepository()
    : snapshot_(make_shared<HistorySnapshot>()) {}

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
    if (persist && !FolderRewindHistoryStore::SaveHistoryFileByConfigId(
            path, configs, Flatten(*next))) {
        return {true, false};
    }
    snapshot_ = std::move(next);
    return {true, true};
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

