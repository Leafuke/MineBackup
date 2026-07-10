#pragma once

#ifndef MINEBACKUP_ENABLE_V15_MIGRATION
#define MINEBACKUP_ENABLE_V15_MIGRATION 1
#endif

#include "AppState.h"
#include "FolderRewindFormat.h"
#include "json.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace LegacyMineBackup15Reader {

struct MetadataSummary {
	int version = 2;
	std::wstring lastBackupFileName;
	std::wstring basedOnFullBackup;
	std::map<std::wstring, std::pair<uintmax_t, long long>> fileStates;
	std::vector<FolderRewindFormat::ChangeRecord> recordIndex;
};

struct HistoryReadResult {
	std::map<int, std::vector<HistoryEntry>> history;
	nlohmann::json unmigrated = nlohmann::json::array();
	int sourceItems = 0;
	int legacyItems = 0;
	int newItems = 0;
};

bool IsLegacyHistoryFile(const std::filesystem::path& path);
bool ReadHistory(const std::filesystem::path& path, const std::map<int, Config>& configs, HistoryReadResult& result);
bool ReadMetadataSummary(const std::filesystem::path& metadataDir, MetadataSummary& summary, std::wstring& error);
bool ReadChangeRecord(const std::filesystem::path& metadataDir, const std::wstring& archiveFileName, FolderRewindFormat::ChangeRecord& record, std::wstring& error);

} // namespace LegacyMineBackup15Reader
