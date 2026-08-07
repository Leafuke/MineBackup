#include "HistoryManager.h"

#include "AppPaths.h"
#include "AppState.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "Logging.h"
#include "MigrationCoordinator.h"
#include "PlatformCompat.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

using namespace std;

namespace {

HistoryRepository g_historyRepository;

bool IsSameHistoryEntry(
    const HistoryEntry& entry,
    const wstring& worldName,
    const wstring& backupFile) {
    return entry.worldName == worldName && entry.backupFile == backupFile;
}

map<int, Config> SnapshotConfigs() {
    lock_guard<mutex> lock(g_appState.configsMutex);
    return g_appState.configs;
}

wstring ResolveConfigId(int configIndex) {
    lock_guard<mutex> lock(g_appState.configsMutex);
    const auto it = g_appState.configs.find(configIndex);
    if (it == g_appState.configs.end()) return {};
    it->second.configId = FolderRewindFormat::EnsureConfigId(it->second.configId);
    return it->second.configId;
}

bool PersistenceBlocked() {
    return MigrationCoordinator::IsHistoryPersistenceBlocked();
}

void PrepareHistoryFileForWrite(bool persist) {
#ifdef _WIN32
    if (persist) SetFileAttributesWin(GetAppPaths().HistoryFile().wstring(), 0);
#else
    (void)persist;
#endif
}

void FinishHistoryFileWrite(bool persist, bool saved) {
#ifdef _WIN32
    if (persist && saved) {
        SetFileAttributesWin(GetAppPaths().HistoryFile().wstring(), 1);
    }
#else
    (void)persist;
    (void)saved;
#endif
}

HistoryMutationResult MutateHistory(
    int configIndex,
    const HistoryRepository::Mutator& mutator) {
    const wstring configId = ResolveConfigId(configIndex);
    if (configId.empty()) return {};
    const map<int, Config> configs = SnapshotConfigs();
    const bool persist = !PersistenceBlocked();
    PrepareHistoryFileForWrite(persist);
    const HistoryMutationResult result = g_historyRepository.Mutate(
        configId,
        GetAppPaths().HistoryFile(),
        configs,
        persist,
        mutator);
    FinishHistoryFileWrite(persist, result.persisted);
    if (result.changed && !result.persisted) {
        MB_LOG_ERROR(
            minebackup::logging::LogCategory::History,
            "history.save.failed",
            "Failed to persist the history store; the in-memory snapshot was not published.");
    }
    return result;
}

FolderRewindHistoryStore::HistoryByConfigId LoadLegacyHistoryFile(
    const filesystem::path& filename,
    const map<int, Config>& configs,
    bool& loaded) {
    loaded = false;
    FolderRewindHistoryStore::HistoryByConfigId history;
    ifstream in(filename, ios::binary);
    if (!in.is_open()) return history;

    string lineUtf8;
    int currentConfigIndex = -1;
    while (getline(in, lineUtf8)) {
        const wstring line = utf8_to_wstring(lineUtf8);
        if (line.empty() || line.front() == L'#') continue;

        if (line.front() == L'[' && line.back() == L']') {
            const wstring section = line.substr(1, line.size() - 2);
            if (section.rfind(L"Config", 0) == 0) {
                try {
                    currentConfigIndex = stoi(section.substr(6));
                }
                catch (...) {
                    currentConfigIndex = -1;
                }
            }
            continue;
        }
        if (currentConfigIndex < 0) continue;

        const auto configIt = configs.find(currentConfigIndex);
        if (configIt == configs.end() || configIt->second.configId.empty()) continue;
        const auto separator = line.find(L'=');
        if (separator == wstring::npos || line.substr(0, separator) != L"Entry") continue;

        wstringstream stream(line.substr(separator + 1));
        vector<wstring> segments;
        for (wstring segment; getline(stream, segment, L'|');) {
            segments.push_back(std::move(segment));
        }
        if (segments.size() < 4) continue;

        HistoryEntry entry;
        entry.configId = configIt->second.configId;
        entry.timestamp_str = segments[0];
        entry.worldName = segments[1];
        entry.backupFile = segments[2];
        entry.backupType = segments[3];
        entry.isPartialBackup = FolderRewindFormat::IsSmartBackupType(entry.backupType);
        entry.comment = segments.size() >= 5 ? segments[4] : L"";
        entry.isImportant = segments.size() >= 6 && segments[5] == L"important";
        history[entry.configId].push_back(std::move(entry));
    }
    loaded = true;
    return history;
}

} // namespace

HistoryRepository& GetHistoryRepository() {
    return g_historyRepository;
}

void LoadHistory() {
    const filesystem::path jsonFilename = GetAppPaths().HistoryFile();
    const filesystem::path legacyFilename = GetAppPaths().dataRoot / L"history.dat";
    const map<int, Config> configs = SnapshotConfigs();

    if (filesystem::exists(jsonFilename)) {
        if (g_historyRepository.Load(jsonFilename, configs)) {
            MB_LOG_DEBUG(
                minebackup::logging::LogCategory::History,
                "history.load.completed",
                "Loaded {} configuration history groups.",
                g_historyRepository.Snapshot()->byConfigId.size());
            return;
        }
        MB_LOG_WARNING(
            minebackup::logging::LogCategory::History,
            "history.load.invalid",
            "The history store could not be parsed; trying legacy recovery.");
    }

    bool legacyLoaded = false;
    auto legacy = LoadLegacyHistoryFile(legacyFilename, configs, legacyLoaded);
    if (legacyLoaded) {
        const bool persist = !PersistenceBlocked();
        PrepareHistoryFileForWrite(persist);
        const bool replaced = g_historyRepository.ReplaceAll(
            std::move(legacy), jsonFilename, configs, persist);
        FinishHistoryFileWrite(persist, replaced);
        if (replaced) {
            MB_LOG_INFO(
                minebackup::logging::LogCategory::Migration,
                "history.migration.completed",
                "Migrated the legacy history store.");
            return;
        }
    }

    (void)g_historyRepository.ReplaceAll({}, jsonFilename, configs, false);
}

void AddHistoryEntry(
    int configIndex,
    const wstring& worldName,
    const wstring& backupFile,
    const wstring& backupType,
    const wstring& comment,
    const wstring& worldPath) {
    const wstring configId = ResolveConfigId(configIndex);
    if (configId.empty()) return;

    HistoryEntry entry;
    entry.configId = configId;
    entry.timestamp_str = FolderRewindFormat::MakeLocalHistoryTimestampString();
    entry.worldPath = worldPath;
    entry.worldName = worldName;
    entry.backupFile = backupFile;
    entry.backupType = backupType;
    entry.isPartialBackup = FolderRewindFormat::IsSmartBackupType(backupType);
    entry.comment = comment;
    (void)MutateHistory(configIndex, [&](vector<HistoryEntry>& entries) {
        entries.push_back(entry);
        return true;
    });
}

void RemoveHistoryEntry(int configIndex, const wstring& backupFileToRemove) {
    (void)MutateHistory(configIndex, [&](vector<HistoryEntry>& entries) {
        const auto oldSize = entries.size();
        erase_if(entries, [&](const HistoryEntry& entry) {
            return entry.backupFile == backupFileToRemove;
        });
        return entries.size() != oldSize;
    });
}

void RemoveHistoryEntry(
    int configIndex,
    const wstring& worldName,
    const wstring& backupFileToRemove) {
    (void)MutateHistory(configIndex, [&](vector<HistoryEntry>& entries) {
        const auto oldSize = entries.size();
        erase_if(entries, [&](const HistoryEntry& entry) {
            return IsSameHistoryEntry(entry, worldName, backupFileToRemove);
        });
        return entries.size() != oldSize;
    });
}

bool ExportHistoryToFile(const wstring& destinationPath, int configIndex) {
    const map<int, Config> configs = SnapshotConfigs();
    const auto snapshot = g_historyRepository.Snapshot();
    FolderRewindHistoryStore::HistoryByConfigId history;
    if (configIndex < 0) {
        for (const auto& pair : snapshot->byConfigId) history[pair.first] = *pair.second;
    }
    else {
        const auto configIt = configs.find(configIndex);
        if (configIt == configs.end()) return false;
        const auto historyIt = snapshot->byConfigId.find(configIt->second.configId);
        if (historyIt != snapshot->byConfigId.end()) {
            history[configIt->second.configId] = *historyIt->second;
        }
    }
    return FolderRewindHistoryStore::SaveHistoryFileByConfigId(
        filesystem::path(destinationPath), configs, history);
}

bool ImportHistoryFromFile(
    const wstring& sourcePath,
    int configIndex,
    bool mergeExisting) {
    const wstring targetConfigId = ResolveConfigId(configIndex);
    if (targetConfigId.empty()) return false;

    ifstream in{filesystem::path(sourcePath), ios::binary};
    if (!in.is_open()) return false;
    const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
    if (root.is_discarded() || !root.is_array()) return false;

    vector<HistoryEntry> parsedEntries;
    for (const auto& item : root) {
        HistoryEntry entry;
        wstring importedConfigId;
        int importedConfigIndex = -1;
        if (!FolderRewindHistoryStore::TryParseHistoryItem(
                item, entry, importedConfigId)
            && !FolderRewindHistoryStore::TryParseLegacyHistoryItem(
                item, entry, importedConfigIndex)) {
            continue;
        }
        entry.configId = targetConfigId;
        parsedEntries.push_back(std::move(entry));
    }
    if (!root.empty() && parsedEntries.empty()) return false;

    const HistoryMutationResult result = MutateHistory(
        configIndex,
        [&](vector<HistoryEntry>& entries) {
            bool changed = !mergeExisting && !entries.empty();
            if (!mergeExisting) entries.clear();
            for (const auto& entry : parsedEntries) {
                auto existing = find_if(entries.begin(), entries.end(), [&](const auto& value) {
                    return IsSameHistoryEntry(value, entry.worldName, entry.backupFile);
                });
                if (existing == entries.end()) {
                    entries.push_back(entry);
                    changed = true;
                }
            }
            return changed || (!mergeExisting && parsedEntries.empty());
        });
    return !result.changed || result.persisted || PersistenceBlocked();
}

HistoryRepository::EntriesView GetHistoryEntriesViewForConfig(int configIndex) {
    return g_historyRepository.EntriesForConfig(ResolveConfigId(configIndex));
}

shared_ptr<const HistorySnapshot> GetHistorySnapshot() {
    return g_historyRepository.Snapshot();
}

vector<HistoryEntry> GetHistoryEntriesForConfig(int configIndex) {
    return *GetHistoryEntriesViewForConfig(configIndex);
}

vector<HistoryEntry> GetHistoryEntriesForWorld(
    int configIndex,
    const wstring& worldName) {
    vector<HistoryEntry> result;
    for (const auto& entry : *GetHistoryEntriesViewForConfig(configIndex)) {
        if (entry.worldName == worldName) result.push_back(entry);
    }
    sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.timestamp_str < rhs.timestamp_str;
    });
    return result;
}

bool TryGetHistoryEntry(
    int configIndex,
    const wstring& worldName,
    const wstring& backupFile,
    HistoryEntry& outEntry) {
    for (const auto& entry : *GetHistoryEntriesViewForConfig(configIndex)) {
        if (IsSameHistoryEntry(entry, worldName, backupFile)) {
            outEntry = entry;
            return true;
        }
    }
    return false;
}

bool UpsertHistoryEntry(
    int configIndex,
    const HistoryEntry& entry,
    bool overwriteExisting) {
    if (entry.worldName.empty() || entry.backupFile.empty()) return false;
    const HistoryMutationResult result = MutateHistory(
        configIndex,
        [&](vector<HistoryEntry>& entries) {
            for (auto& existing : entries) {
                if (!IsSameHistoryEntry(existing, entry.worldName, entry.backupFile)) continue;
                if (overwriteExisting) {
                    existing = entry;
                    existing.configId = ResolveConfigId(configIndex);
                    return true;
                }
                bool changed = false;
                auto fill = [&](wstring& target, const wstring& value) {
                    if (target.empty() && !value.empty()) {
                        target = value;
                        changed = true;
                    }
                };
                fill(existing.configId, entry.configId);
                fill(existing.timestamp_str, entry.timestamp_str);
                fill(existing.worldPath, entry.worldPath);
                fill(existing.comment, entry.comment);
                fill(existing.backupType, entry.backupType);
                if (!existing.isPartialBackup && entry.isPartialBackup) {
                    existing.isPartialBackup = true;
                    changed = true;
                }
                if (!existing.isImportant && entry.isImportant) {
                    existing.isImportant = true;
                    changed = true;
                }
                if (!existing.isCloudArchived && entry.isCloudArchived) {
                    existing.isCloudArchived = true;
                    changed = true;
                }
                fill(existing.cloudArchivedAtUtc, entry.cloudArchivedAtUtc);
                fill(existing.cloudArchiveRemotePath, entry.cloudArchiveRemotePath);
                fill(existing.cloudMetadataRecordRemotePath, entry.cloudMetadataRecordRemotePath);
                fill(existing.cloudMetadataStateRemotePath, entry.cloudMetadataStateRemotePath);
                return changed;
            }
            HistoryEntry copy = entry;
            copy.configId = ResolveConfigId(configIndex);
            entries.push_back(std::move(copy));
            return true;
        });
    return result.changed && (result.persisted || PersistenceBlocked());
}

bool UpdateHistoryEntry(
    int configIndex,
    const wstring& worldName,
    const wstring& backupFile,
    const function<void(HistoryEntry&)>& update) {
    if (!update) return false;
    const HistoryMutationResult result = MutateHistory(
        configIndex,
        [&](vector<HistoryEntry>& entries) {
            for (auto& entry : entries) {
                if (!IsSameHistoryEntry(entry, worldName, backupFile)) continue;
                update(entry);
                return true;
            }
            return false;
        });
    return result.changed && (result.persisted || PersistenceBlocked());
}

bool ReplaceHistoryEntriesForConfig(int configIndex, vector<HistoryEntry> entries) {
    const wstring configId = ResolveConfigId(configIndex);
    if (configId.empty()) return false;
    for (auto& entry : entries) entry.configId = configId;
    const HistoryMutationResult result = MutateHistory(
        configIndex,
        [entries = std::move(entries)](vector<HistoryEntry>& target) mutable {
            target = std::move(entries);
            return true;
        });
    return result.persisted || PersistenceBlocked();
}

bool ClearHistoryEntriesForWorld(int configIndex, const wstring& worldName) {
    const HistoryMutationResult result = MutateHistory(
        configIndex,
        [&](vector<HistoryEntry>& entries) {
            const auto oldSize = entries.size();
            erase_if(entries, [&](const HistoryEntry& entry) {
                return entry.worldName == worldName;
            });
            return entries.size() != oldSize;
        });
    return !result.changed || result.persisted || PersistenceBlocked();
}

bool UpdateHistoryCloudState(
    int configIndex,
    const wstring& worldName,
    const wstring& backupFile,
    bool isCloudArchived,
    const wstring& archivedAtUtc,
    const wstring& archiveRemotePath,
    const wstring& metadataRecordRemotePath,
    const wstring& metadataStateRemotePath) {
    return UpdateHistoryEntry(
        configIndex,
        worldName,
        backupFile,
        [&](HistoryEntry& entry) {
            entry.isCloudArchived = isCloudArchived;
            if (isCloudArchived) {
                if (!archivedAtUtc.empty()) entry.cloudArchivedAtUtc = archivedAtUtc;
                if (!archiveRemotePath.empty()) entry.cloudArchiveRemotePath = archiveRemotePath;
                if (!metadataRecordRemotePath.empty()) {
                    entry.cloudMetadataRecordRemotePath = metadataRecordRemotePath;
                }
                if (!metadataStateRemotePath.empty()) {
                    entry.cloudMetadataStateRemotePath = metadataStateRemotePath;
                }
            }
            else {
                entry.cloudArchivedAtUtc.clear();
                entry.cloudArchiveRemotePath.clear();
                entry.cloudMetadataRecordRemotePath.clear();
                entry.cloudMetadataStateRemotePath.clear();
            }
        });
}
