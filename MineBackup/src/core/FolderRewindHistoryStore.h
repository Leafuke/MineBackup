#pragma once
#ifndef FOLDER_REWIND_HISTORY_STORE_H
#define FOLDER_REWIND_HISTORY_STORE_H

#include "DataModels.h"
#include "FolderRewindFormat.h"
#include "json.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace FolderRewindHistoryStore {

using HistoryByConfigId = std::map<std::wstring, std::vector<HistoryEntry>>;

nlohmann::json SerializeHistoryItem(const Config& config, const HistoryEntry& entry);
bool TryParseHistoryItem(const nlohmann::json& item, HistoryEntry& outEntry, std::wstring& outConfigId);
bool TryParseLegacyHistoryItem(const nlohmann::json& item, HistoryEntry& outEntry, int& outConfigIndex);
int ResolveConfigIndexByConfigId(const std::map<int, Config>& configs, const std::wstring& configId, int fallbackConfigIndex = -1);
bool LoadHistoryFile(const std::filesystem::path& path, const std::map<int, Config>& configs, std::map<int, std::vector<HistoryEntry>>& outHistory);
bool SaveHistoryFile(const std::filesystem::path& path, const std::map<int, Config>& configs, const std::map<int, std::vector<HistoryEntry>>& history);
bool LoadHistoryFileByConfigId(const std::filesystem::path& path, const std::map<int, Config>& configs, HistoryByConfigId& outHistory);
bool SaveHistoryFileByConfigId(const std::filesystem::path& path, const std::map<int, Config>& configs, const HistoryByConfigId& history);
nlohmann::json SerializeActiveHistoryManifest(const Config& config, const std::vector<HistoryEntry>& entries);
bool TryParseActiveHistoryManifest(const nlohmann::json& root, CloudActiveHistoryManifest& outManifest);
bool ManifestContainsHistoryItem(const CloudActiveHistoryManifest& manifest, const HistoryEntry& entry);

} // namespace FolderRewindHistoryStore

#endif // FOLDER_REWIND_HISTORY_STORE_H
