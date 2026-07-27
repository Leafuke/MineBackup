#pragma once

#include "DataModels.h"

#include <map>
#include <string>
#include <vector>

struct PortableWorldDefinition {
    std::wstring name;
    std::wstring description;
};

struct PortableConfigEntry {
    std::wstring configId;
    std::string name;
    std::vector<PortableWorldDefinition> worlds;
    std::wstring zipFormat = L"7z";
    std::wstring zipMethod = L"LZMA2";
    int backupMode = 1;
    int zipLevel = 5;
    int keepCount = 0;
    bool backupBefore = false;
    int cpuThreads = 0;
    bool useLowPriority = false;
    bool skipIfUnchanged = true;
    int maxSmartBackupsPerFull = 5;
    std::vector<std::wstring> blacklist;
    bool cloudSyncEnabled = false;
    int cloudSyncMode = 0;
    int cloudTimeoutSeconds = 600;
    int cloudRetryCount = 0;
    bool cloudSyncHistoryAfterUpload = true;
    bool cloudAutoDownloadBeforeRestore = true;
};

struct PortableConfigMergePreview {
    std::vector<std::wstring> added;
    std::vector<std::wstring> updated;
    std::vector<std::wstring> preserved;
    std::vector<std::wstring> excludedFields;
};

struct PortableConfigDocument {
    static constexpr int SchemaVersion = 1;
    static constexpr std::size_t MaximumBytes = 1u * 1024u * 1024u;

    std::map<std::wstring, PortableConfigEntry> configs;

    std::string Serialize() const;
    static bool Parse(const std::string& json, PortableConfigDocument& document, std::wstring& error);
    static PortableConfigDocument FromLocalConfigs(const std::map<int, Config>& configs);
    static PortableConfigDocument MergeForUpload(
        const std::map<int, Config>& local,
        const PortableConfigDocument& remote,
        PortableConfigMergePreview& preview);
    static PortableConfigMergePreview PreviewImport(
        const std::map<int, Config>& local,
        const PortableConfigDocument& remote);
    static bool ApplyImport(
        std::map<int, Config>& local,
        const PortableConfigDocument& remote,
        PortableConfigMergePreview& preview,
        std::wstring& error);
};

bool IsConfigPendingLocalBinding(const Config& config);
