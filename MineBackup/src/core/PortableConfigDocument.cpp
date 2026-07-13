#include "PortableConfigDocument.h"

#include "FolderRewindFormat.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <cwctype>
#include <set>

using namespace std;

namespace {

const vector<wstring> kExcludedFields = {
    L"saveRoot", L"backupPath", L"zipPath", L"fontPath", L"rclonePath",
    L"rcloneRemotePath", L"cloudWorkingDirectory", L"snapshotPath", L"othersPath",
    L"weSnapshotPath", L"credentials", L"windowState", L"runtimeResults",
    L"specialConfigs", L"commands", L"scripts", L"automation"};

bool IsCanonicalConfigId(const wstring& value) {
    if (value.size() != 36) return false;
    for (size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != L'-') return false;
        }
        else if (!iswxdigit(value[index])) {
            return false;
        }
    }
    return true;
}

bool ReadString(const nlohmann::json& value, const char* key, string& output, size_t maximum, wstring& error) {
    if (!value.contains(key) || !value[key].is_string()) {
        error = L"Portable configuration field is missing or has the wrong type: " + utf8_to_wstring(key);
        return false;
    }
    output = value[key].get<string>();
    if (output.size() > maximum || output.find('\0') != string::npos) {
        error = L"Portable configuration string exceeds its limit: " + utf8_to_wstring(key);
        return false;
    }
    return true;
}

bool ReadWide(const nlohmann::json& value, const char* key, wstring& output, size_t maximum, wstring& error) {
    string encoded;
    if (!ReadString(value, key, encoded, maximum * 4, error)) return false;
    output = utf8_to_wstring(encoded);
    if (output.size() > maximum) {
        error = L"Portable configuration string exceeds its character limit: " + utf8_to_wstring(key);
        return false;
    }
    return true;
}

bool ReadInt(const nlohmann::json& value, const char* key, int minimum, int maximum, int& output, wstring& error) {
    if (!value.contains(key) || !value[key].is_number_integer()) {
        error = L"Portable configuration integer is missing or invalid: " + utf8_to_wstring(key);
        return false;
    }
    const auto parsed = value[key].get<long long>();
    if (parsed < minimum || parsed > maximum) {
        error = L"Portable configuration integer is outside its allowed range: " + utf8_to_wstring(key);
        return false;
    }
    output = static_cast<int>(parsed);
    return true;
}

bool ReadBool(const nlohmann::json& value, const char* key, bool& output, wstring& error) {
    if (!value.contains(key) || !value[key].is_boolean()) {
        error = L"Portable configuration boolean is missing or invalid: " + utf8_to_wstring(key);
        return false;
    }
    output = value[key].get<bool>();
    return true;
}

nlohmann::json SerializeEntry(const PortableConfigEntry& entry) {
    nlohmann::json value;
    value["name"] = entry.name;
    value["worlds"] = nlohmann::json::array();
    for (const auto& world : entry.worlds) {
        value["worlds"].push_back({
            {"name", wstring_to_utf8(world.name)},
            {"description", wstring_to_utf8(world.description)}});
    }
    value["backup"] = {
        {"mode", entry.backupMode}, {"beforeBackup", entry.backupBefore},
        {"skipIfUnchanged", entry.skipIfUnchanged}, {"maxSmartBackupsPerFull", entry.maxSmartBackupsPerFull}};
    value["compression"] = {
        {"format", wstring_to_utf8(entry.zipFormat)}, {"method", wstring_to_utf8(entry.zipMethod)},
        {"level", entry.zipLevel}, {"cpuThreads", entry.cpuThreads}, {"useLowPriority", entry.useLowPriority}};
    value["retention"] = {{"keepCount", entry.keepCount}};
    value["blacklist"] = nlohmann::json::array();
    for (const auto& item : entry.blacklist) value["blacklist"].push_back(wstring_to_utf8(item));
    value["cloudPolicy"] = {
        {"enabled", entry.cloudSyncEnabled}, {"mode", entry.cloudSyncMode},
        {"timeoutSeconds", entry.cloudTimeoutSeconds}, {"retryCount", entry.cloudRetryCount},
        {"syncHistoryAfterUpload", entry.cloudSyncHistoryAfterUpload},
        {"autoDownloadBeforeRestore", entry.cloudAutoDownloadBeforeRestore}};
    return value;
}

bool ParseEntry(const wstring& configId, const nlohmann::json& value, PortableConfigEntry& entry, wstring& error) {
    if (!value.is_object()) {
        error = L"Each portable configuration must be an object.";
        return false;
    }
    entry.configId = configId;
    if (!ReadString(value, "name", entry.name, 512, error)) return false;
    if (!value.contains("worlds") || !value["worlds"].is_array() || value["worlds"].size() > 10000) {
        error = L"Portable configuration worlds must be a bounded array.";
        return false;
    }
    set<wstring> worldNames;
    for (const auto& worldValue : value["worlds"]) {
        if (!worldValue.is_object()) {
            error = L"Portable world entry must be an object.";
            return false;
        }
        PortableWorldDefinition world;
        if (!ReadWide(worldValue, "name", world.name, 255, error)
            || !ReadWide(worldValue, "description", world.description, 4096, error)) return false;
        if (!FolderRewindFormat::IsSafeSinglePathSegment(world.name) || !worldNames.insert(world.name).second) {
            error = L"Portable world names must be unique safe logical names.";
            return false;
        }
        entry.worlds.push_back(std::move(world));
    }
    if (!value.contains("backup") || !value["backup"].is_object()
        || !value.contains("compression") || !value["compression"].is_object()
        || !value.contains("retention") || !value["retention"].is_object()
        || !value.contains("cloudPolicy") || !value["cloudPolicy"].is_object()) {
        error = L"Portable configuration policy groups are incomplete.";
        return false;
    }
    const auto& backup = value["backup"];
    const auto& compression = value["compression"];
    const auto& retention = value["retention"];
    const auto& cloud = value["cloudPolicy"];
    if (!ReadInt(backup, "mode", 0, 2, entry.backupMode, error)
        || !ReadBool(backup, "beforeBackup", entry.backupBefore, error)
        || !ReadBool(backup, "skipIfUnchanged", entry.skipIfUnchanged, error)
        || !ReadInt(backup, "maxSmartBackupsPerFull", 1, 100000, entry.maxSmartBackupsPerFull, error)
        || !ReadWide(compression, "format", entry.zipFormat, 32, error)
        || !ReadWide(compression, "method", entry.zipMethod, 32, error)
        || !ReadInt(compression, "level", 0, 22, entry.zipLevel, error)
        || !ReadInt(compression, "cpuThreads", 0, 1024, entry.cpuThreads, error)
        || !ReadBool(compression, "useLowPriority", entry.useLowPriority, error)
        || !ReadInt(retention, "keepCount", 0, 1000000, entry.keepCount, error)
        || !ReadBool(cloud, "enabled", entry.cloudSyncEnabled, error)
        || !ReadInt(cloud, "mode", 0, 1, entry.cloudSyncMode, error)
        || !ReadInt(cloud, "timeoutSeconds", 1, 86400, entry.cloudTimeoutSeconds, error)
        || !ReadInt(cloud, "retryCount", 0, 100, entry.cloudRetryCount, error)
        || !ReadBool(cloud, "syncHistoryAfterUpload", entry.cloudSyncHistoryAfterUpload, error)
        || !ReadBool(cloud, "autoDownloadBeforeRestore", entry.cloudAutoDownloadBeforeRestore, error)) return false;
    if (entry.zipFormat != L"7z" && entry.zipFormat != L"zip") {
        error = L"Portable configuration contains an unsupported archive format.";
        return false;
    }
    if (!value.contains("blacklist") || !value["blacklist"].is_array() || value["blacklist"].size() > 10000) {
        error = L"Portable configuration blacklist must be a bounded array.";
        return false;
    }
    for (const auto& item : value["blacklist"]) {
        if (!item.is_string() || item.get_ref<const string&>().size() > 4096) {
            error = L"Portable configuration blacklist item is invalid.";
            return false;
        }
        entry.blacklist.push_back(utf8_to_wstring(item.get<string>()));
    }
    return true;
}

PortableConfigEntry FromConfig(const Config& config) {
    PortableConfigEntry entry;
    entry.configId = config.configId;
    entry.name = config.name;
    for (const auto& world : config.worlds) entry.worlds.push_back({world.first, world.second});
    entry.zipFormat = config.zipFormat;
    entry.zipMethod = config.zipMethod;
    entry.backupMode = config.backupMode;
    entry.zipLevel = config.zipLevel;
    entry.keepCount = config.keepCount;
    entry.backupBefore = config.backupBefore;
    entry.cpuThreads = config.cpuThreads;
    entry.useLowPriority = config.useLowPriority;
    entry.skipIfUnchanged = config.skipIfUnchanged;
    entry.maxSmartBackupsPerFull = config.maxSmartBackupsPerFull;
    entry.blacklist = config.blacklist;
    entry.cloudSyncEnabled = config.cloudSyncEnabled;
    entry.cloudSyncMode = config.cloudSyncMode;
    entry.cloudTimeoutSeconds = config.cloudTimeoutSeconds;
    entry.cloudRetryCount = config.cloudRetryCount;
    entry.cloudSyncHistoryAfterUpload = config.cloudSyncHistoryAfterUpload;
    entry.cloudAutoDownloadBeforeRestore = config.cloudAutoDownloadBeforeRestore;
    return entry;
}

void ApplyPortableFields(const PortableConfigEntry& entry, Config& config) {
    config.configId = entry.configId;
    config.name = entry.name;
    config.worlds.clear();
    for (const auto& world : entry.worlds) config.worlds.push_back({world.name, world.description});
    config.zipFormat = entry.zipFormat;
    config.zipMethod = entry.zipMethod;
    config.backupMode = entry.backupMode;
    config.zipLevel = entry.zipLevel;
    config.keepCount = entry.keepCount;
    config.backupBefore = entry.backupBefore;
    config.cpuThreads = entry.cpuThreads;
    config.useLowPriority = entry.useLowPriority;
    config.skipIfUnchanged = entry.skipIfUnchanged;
    config.maxSmartBackupsPerFull = entry.maxSmartBackupsPerFull;
    config.blacklist = entry.blacklist;
    config.cloudSyncEnabled = entry.cloudSyncEnabled;
    config.cloudSyncMode = entry.cloudSyncMode;
    config.cloudTimeoutSeconds = entry.cloudTimeoutSeconds;
    config.cloudRetryCount = entry.cloudRetryCount;
    config.cloudSyncHistoryAfterUpload = entry.cloudSyncHistoryAfterUpload;
    config.cloudAutoDownloadBeforeRestore = entry.cloudAutoDownloadBeforeRestore;
}

map<wstring, int> LocalIndicesById(const map<int, Config>& local) {
    map<wstring, int> result;
    for (const auto& [index, config] : local) {
        if (!config.configId.empty()) result.emplace(config.configId, index);
    }
    return result;
}

} // namespace

string PortableConfigDocument::Serialize() const {
    nlohmann::json root;
    root["schemaVersion"] = SchemaVersion;
    root["configs"] = nlohmann::json::object();
    for (const auto& [configId, entry] : configs) {
        root["configs"][wstring_to_utf8(configId)] = SerializeEntry(entry);
    }
    return root.dump(2) + "\n";
}

bool PortableConfigDocument::Parse(const string& json, PortableConfigDocument& document, wstring& error) {
    document.configs.clear();
    error.clear();
    if (json.size() > MaximumBytes) {
        error = L"portable-config.json exceeds the 1 MiB safety limit.";
        return false;
    }
    const auto root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = L"portable-config.json is not valid JSON.";
        return false;
    }
    if (!root.contains("schemaVersion") || !root["schemaVersion"].is_number_integer()
        || root["schemaVersion"].get<int>() != SchemaVersion) {
        error = L"portable-config.json uses an unsupported schema version.";
        return false;
    }
    if (!root.contains("configs") || !root["configs"].is_object() || root["configs"].size() > 10000) {
        error = L"portable-config.json configs must be a bounded object keyed by ConfigId.";
        return false;
    }
    for (auto it = root["configs"].begin(); it != root["configs"].end(); ++it) {
        const wstring configId = utf8_to_wstring(it.key());
        if (!IsCanonicalConfigId(configId)) {
            error = L"portable-config.json contains an invalid ConfigId.";
            return false;
        }
        PortableConfigEntry entry;
        if (!ParseEntry(configId, it.value(), entry, error)) return false;
        if (!document.configs.emplace(configId, std::move(entry)).second) {
            error = L"portable-config.json contains duplicate ConfigId values.";
            return false;
        }
    }
    return true;
}

PortableConfigDocument PortableConfigDocument::FromLocalConfigs(const map<int, Config>& local) {
    PortableConfigDocument document;
    for (const auto& [index, config] : local) {
        (void)index;
        if (config.configId.empty()) continue;
        document.configs[config.configId] = FromConfig(config);
    }
    return document;
}

PortableConfigDocument PortableConfigDocument::MergeForUpload(
    const map<int, Config>& local,
    const PortableConfigDocument& remote,
    PortableConfigMergePreview& preview) {
    PortableConfigDocument merged = remote;
    preview = {};
    preview.excludedFields = kExcludedFields;
    const auto localDocument = FromLocalConfigs(local);
    for (const auto& [configId, entry] : localDocument.configs) {
        if (merged.configs.count(configId)) preview.updated.push_back(configId);
        else preview.added.push_back(configId);
        merged.configs[configId] = entry;
    }
    for (const auto& [configId, entry] : remote.configs) {
        (void)entry;
        if (!localDocument.configs.count(configId)) preview.preserved.push_back(configId);
    }
    return merged;
}

PortableConfigMergePreview PortableConfigDocument::PreviewImport(
    const map<int, Config>& local,
    const PortableConfigDocument& remote) {
    PortableConfigMergePreview preview;
    preview.excludedFields = kExcludedFields;
    const auto localIds = LocalIndicesById(local);
    for (const auto& [configId, entry] : remote.configs) {
        (void)entry;
        if (localIds.count(configId)) preview.updated.push_back(configId);
        else preview.added.push_back(configId);
    }
    for (const auto& [configId, index] : localIds) {
        (void)index;
        if (!remote.configs.count(configId)) preview.preserved.push_back(configId);
    }
    return preview;
}

bool PortableConfigDocument::ApplyImport(
    map<int, Config>& local,
    const PortableConfigDocument& remote,
    PortableConfigMergePreview& preview,
    wstring& error) {
    preview = PreviewImport(local, remote);
    error.clear();
    auto localIds = LocalIndicesById(local);
    int nextIndex = local.empty() ? 0 : local.rbegin()->first + 1;
    for (const auto& [configId, entry] : remote.configs) {
        auto existing = localIds.find(configId);
        if (existing != localIds.end()) {
            Config& target = local[existing->second];
            const bool wasPending = target.pendingLocalBinding;
            ApplyPortableFields(entry, target);
            target.pendingLocalBinding = wasPending;
            continue;
        }
        Config created;
        ApplyPortableFields(entry, created);
        created.saveRoot.clear();
        created.backupPath.clear();
        created.zipPath.clear();
        created.rclonePath.clear();
        created.rcloneRemotePath.clear();
        created.cloudWorkingDirectory.clear();
        created.cloudSyncEnabled = false;
        created.backupOnGameStart = false;
        created.pendingLocalBinding = true;
        while (local.count(nextIndex)) ++nextIndex;
        local[nextIndex++] = std::move(created);
    }
    return true;
}

bool IsConfigPendingLocalBinding(const Config& config) {
    return config.pendingLocalBinding;
}
