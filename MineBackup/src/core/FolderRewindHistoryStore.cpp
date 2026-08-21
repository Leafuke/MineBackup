#include "FolderRewindHistoryStore.h"

#include "AtomicFileWriter.h"
#include "PlatformCompat.h"
#include "text_to_text.h"

#include <algorithm>
#include <fstream>

using namespace std;

namespace FolderRewindHistoryStore {
namespace {

wstring JsonString(const nlohmann::json& item, const char* key) {
    const auto it = item.find(key);
    if (it == item.end() || !it->is_string()) return L"";
    return utf8_to_wstring(it->get<string>());
}

bool JsonBool(const nlohmann::json& item, const char* key, bool defaultValue = false) {
    const auto it = item.find(key);
    if (it == item.end() || !it->is_boolean()) return defaultValue;
    return it->get<bool>();
}

int JsonInt(const nlohmann::json& item, const char* key, int defaultValue = -1) {
    const auto it = item.find(key);
    if (it == item.end()) return defaultValue;
    if (it->is_number_integer()) return it->get<int>();
    if (it->is_string()) {
        try {
            size_t consumed = 0;
            const int value = stoi(it->get<string>(), &consumed);
            return consumed == it->get<string>().size() ? value : defaultValue;
        }
        catch (...) {
            return defaultValue;
        }
    }
    return defaultValue;
}

void SetJsonString(nlohmann::json& item, const char* key, const wstring& value) {
    item[key] = wstring_to_utf8(value);
}

wstring GetConfigIdForEntry(const Config& config, const HistoryEntry& entry) {
    if (!config.configId.empty()) return config.configId;
    return entry.configId;
}

bool IsSameTextIgnoreCase(const wstring& lhs, const wstring& rhs) {
    return _wcsicmp(lhs.c_str(), rhs.c_str()) == 0;
}

} // namespace

nlohmann::json SerializeHistoryItem(const Config& config, const HistoryEntry& entry) {
    nlohmann::json item;
    SetJsonString(item, "ConfigId", GetConfigIdForEntry(config, entry));
    SetJsonString(item, "FolderPath", entry.worldPath);
    SetJsonString(item, "FolderName", entry.worldName);
    SetJsonString(item, "FileName", entry.backupFile);
    SetJsonString(item, "Timestamp", entry.timestamp_str);
    SetJsonString(item, "BackupType", entry.backupType);
    item["IsPartialBackup"] = entry.isPartialBackup;
    SetJsonString(item, "Comment", entry.comment);
    item["IsImportant"] = entry.isImportant;
    item["IsCloudArchived"] = entry.isCloudArchived;
    SetJsonString(item, "CloudArchivedAtUtc", entry.cloudArchivedAtUtc);
    SetJsonString(item, "CloudArchiveRemotePath", entry.cloudArchiveRemotePath);
    SetJsonString(item, "CloudMetadataRecordRemotePath", entry.cloudMetadataRecordRemotePath);
    SetJsonString(item, "CloudMetadataStateRemotePath", entry.cloudMetadataStateRemotePath);
    return item;
}

bool TryParseHistoryItem(const nlohmann::json& item, HistoryEntry& outEntry, wstring& outConfigId) {
    if (!item.is_object()) return false;

    outConfigId = JsonString(item, "ConfigId");
    if (outConfigId.empty()) return false;

    HistoryEntry entry;
    entry.configId = outConfigId;
    entry.worldPath = JsonString(item, "FolderPath");
    entry.worldName = JsonString(item, "FolderName");
    entry.backupFile = JsonString(item, "FileName");
    entry.timestamp_str = JsonString(item, "Timestamp");
    entry.backupType = JsonString(item, "BackupType");
    entry.isPartialBackup = JsonBool(item, "IsPartialBackup");
    entry.comment = JsonString(item, "Comment");
    entry.isImportant = JsonBool(item, "IsImportant");
    entry.isCloudArchived = JsonBool(item, "IsCloudArchived");
    entry.cloudArchivedAtUtc = JsonString(item, "CloudArchivedAtUtc");
    entry.cloudArchiveRemotePath = JsonString(item, "CloudArchiveRemotePath");
    entry.cloudMetadataRecordRemotePath = JsonString(item, "CloudMetadataRecordRemotePath");
    entry.cloudMetadataStateRemotePath = JsonString(item, "CloudMetadataStateRemotePath");

    if (entry.worldName.empty() || entry.backupFile.empty()) return false;

    outEntry = std::move(entry);
    return true;
}

bool TryParseLegacyHistoryItem(const nlohmann::json& item, HistoryEntry& outEntry, int& outConfigIndex) {
    if (!item.is_object()) return false;

    HistoryEntry entry;
    outConfigIndex = JsonInt(item, "configIndex");
    entry.timestamp_str = JsonString(item, "timestamp");
    entry.worldPath = JsonString(item, "worldPath");
    entry.worldName = JsonString(item, "worldName");
    entry.backupFile = JsonString(item, "backupFile");
    entry.backupType = JsonString(item, "backupType");
    entry.isPartialBackup = FolderRewindFormat::IsSmartBackupType(entry.backupType);
    entry.comment = JsonString(item, "comment");
    entry.isImportant = JsonBool(item, "isImportant");
    entry.isCloudArchived = JsonBool(item, "isCloudArchived");
    entry.cloudArchivedAtUtc = JsonString(item, "cloudArchivedAtUtc");
    entry.cloudArchiveRemotePath = JsonString(item, "cloudArchiveRemotePath");
    entry.cloudMetadataRecordRemotePath = JsonString(item, "cloudMetadataRecordRemotePath");
    entry.cloudMetadataStateRemotePath = JsonString(item, "cloudMetadataStateRemotePath");

    if (entry.worldName.empty() || entry.backupFile.empty()) return false;

    outEntry = std::move(entry);
    return true;
}

int ResolveConfigIndexByConfigId(const map<int, Config>& configs, const wstring& configId, int fallbackConfigIndex) {
    if (configId.empty()) return fallbackConfigIndex;
    for (const auto& pair : configs) {
        if (!pair.second.configId.empty() && IsSameTextIgnoreCase(pair.second.configId, configId)) {
            return pair.first;
        }
    }
    return fallbackConfigIndex;
}

bool LoadHistoryFile(const filesystem::path& path, const map<int, Config>& configs, map<int, vector<HistoryEntry>>& outHistory) {
    HistoryByConfigId byConfigId;
    if (!LoadHistoryFileByConfigId(path, configs, byConfigId)) return false;

    map<int, vector<HistoryEntry>> loadedHistory;
    for (auto& historyPair : byConfigId) {
        const int configIndex = ResolveConfigIndexByConfigId(configs, historyPair.first);
        if (configIndex >= 0) {
            loadedHistory.emplace(configIndex, std::move(historyPair.second));
        }
    }
    outHistory = std::move(loadedHistory);
    return true;
}

bool LoadHistoryFileByConfigId(const filesystem::path& path, const map<int, Config>& configs, HistoryByConfigId& outHistory) {
    ifstream in(path, ios::binary);
    if (!in.is_open()) return false;

    const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
    if (root.is_discarded() || !root.is_array()) return false;

    HistoryByConfigId loadedHistory;
    const int inputItemCount = static_cast<int>(root.size());
    int parseableItemCount = 0;
    int unmappedLegacyEntryCount = 0;
    for (const auto& item : root) {
        HistoryEntry entry;
        wstring configId;
        int configIndex = -1;
        if (TryParseHistoryItem(item, entry, configId)) {
            ++parseableItemCount;
            configIndex = ResolveConfigIndexByConfigId(configs, configId);
			// 现代 history 以 ConfigId 自身为稳定归属；配置被 prune 后仍必须保留 orphan 记录。
			if (configIndex < 0) configId = entry.configId;
        }
        else {
            int legacyConfigIndex = -1;
            if (!TryParseLegacyHistoryItem(item, entry, legacyConfigIndex)) {
                continue;
            }
            ++parseableItemCount;
            if (configs.find(legacyConfigIndex) != configs.end()) {
                configIndex = legacyConfigIndex;
            }
			else {
				++unmappedLegacyEntryCount;
			}
        }

		if (configId.empty() && configIndex < 0) {
            continue;
        }

        const auto configIt = configs.find(configIndex);
        if (configIt != configs.end()) {
            entry.configId = configIt->second.configId;
        }
        else {
            entry.configId = configId;
        }
        if (entry.configId.empty()) continue;
        loadedHistory[entry.configId].push_back(std::move(entry));
    }

    if (inputItemCount > 0 && parseableItemCount == 0) {
        return false;
    }
    if (unmappedLegacyEntryCount > 0) {
        return false;
    }

    outHistory = std::move(loadedHistory);
    return true;
}

bool SaveHistoryFile(const filesystem::path& path, const map<int, Config>& configs, const map<int, vector<HistoryEntry>>& history) {
    HistoryByConfigId byConfigId;
    for (const auto& historyPair : history) {
        const auto configIt = configs.find(historyPair.first);
        if (configIt == configs.end() || configIt->second.configId.empty()) continue;
        byConfigId[configIt->second.configId] = historyPair.second;
    }
    return SaveHistoryFileByConfigId(path, configs, byConfigId);
}

bool SaveHistoryFileByConfigId(const filesystem::path& path, const map<int, Config>& configs, const HistoryByConfigId& history) {
    nlohmann::json root = nlohmann::json::array();

    for (const auto& historyPair : history) {
        const Config* config = nullptr;
        for (const auto& pair : configs) {
            if (IsSameTextIgnoreCase(pair.second.configId, historyPair.first)) {
                config = &pair.second;
                break;
            }
        }
        Config fallbackConfig;
        fallbackConfig.configId = historyPair.first;
        if (!config) config = &fallbackConfig;

        for (const auto& entry : historyPair.second) {
            root.push_back(SerializeHistoryItem(*config, entry));
        }
    }

	return AtomicFileWriter::WriteText(path, root.dump(2)).success;
}

nlohmann::json SerializeActiveHistoryManifest(const Config& config, const vector<HistoryEntry>& entries) {
    vector<HistoryEntry> sortedEntries = entries;
    sort(sortedEntries.begin(), sortedEntries.end(), [](const HistoryEntry& lhs, const HistoryEntry& rhs) {
        if (lhs.timestamp_str != rhs.timestamp_str) return lhs.timestamp_str < rhs.timestamp_str;
        return lhs.backupFile < rhs.backupFile;
    });

    nlohmann::json root;
    SetJsonString(root, "ConfigId", config.configId);
    SetJsonString(root, "ConfigName", utf8_to_wstring(config.name));
    SetJsonString(root, "UpdatedAtUtc", FolderRewindFormat::MakeUtcTimestampString());
    root["Entries"] = nlohmann::json::array();

    for (const auto& entry : sortedEntries) {
        nlohmann::json item;
        SetJsonString(item, "FolderPath", entry.worldPath);
        SetJsonString(item, "FolderName", entry.worldName);
        SetJsonString(item, "FileName", entry.backupFile);
        SetJsonString(item, "Timestamp", entry.timestamp_str);
        root["Entries"].push_back(std::move(item));
    }

    return root;
}

bool TryParseActiveHistoryManifest(const nlohmann::json& root, CloudActiveHistoryManifest& outManifest) {
    if (!root.is_object()) return false;

    CloudActiveHistoryManifest manifest;
    manifest.configId = JsonString(root, "ConfigId");
    manifest.configName = JsonString(root, "ConfigName");
    manifest.updatedAtUtc = JsonString(root, "UpdatedAtUtc");

    const auto entriesIt = root.find("Entries");
    if (entriesIt != root.end()) {
        if (!entriesIt->is_array()) return false;

        for (const auto& item : *entriesIt) {
            if (!item.is_object()) continue;

            CloudActiveHistoryEntry entry;
            entry.folderPath = JsonString(item, "FolderPath");
            entry.folderName = JsonString(item, "FolderName");
            entry.fileName = JsonString(item, "FileName");
            entry.timestamp = JsonString(item, "Timestamp");
            entry.worldPath = entry.folderPath;
            entry.worldName = entry.folderName;
            entry.backupFile = entry.fileName;

            if (!entry.folderName.empty() && !entry.fileName.empty()) {
                manifest.entries.push_back(std::move(entry));
            }
        }
    }

    outManifest = std::move(manifest);
    return true;
}

bool ManifestContainsHistoryItem(const CloudActiveHistoryManifest& manifest, const HistoryEntry& entry) {
    for (const auto& item : manifest.entries) {
        const wstring itemFileName = item.fileName.empty() ? item.backupFile : item.fileName;
        if (itemFileName != entry.backupFile) continue;

        if (!item.timestamp.empty() && !entry.timestamp_str.empty() && item.timestamp != entry.timestamp_str) {
            continue;
        }

        const wstring itemFolderName = item.folderName.empty() ? item.worldName : item.folderName;
        const wstring itemFolderPath = item.folderPath.empty() ? item.worldPath : item.folderPath;
        if (itemFolderName == entry.worldName || (!itemFolderPath.empty() && itemFolderPath == entry.worldPath)) {
            return true;
        }
    }
    return false;
}

} // namespace FolderRewindHistoryStore
