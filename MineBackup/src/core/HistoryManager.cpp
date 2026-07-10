#include <map>
#include <vector>
#include <fstream>
#include <sstream>
#include "HistoryManager.h"
#include "AppState.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "MigrationService.h"
#include "json.hpp"
#include "text_to_text.h"
#include "PlatformCompat.h"
#include <algorithm>
#include <filesystem>

using namespace std;

namespace {
	bool IsSameHistoryEntry(const HistoryEntry& lhs, const HistoryEntry& rhs) {
		return lhs.worldName == rhs.worldName
			&& lhs.backupFile == rhs.backupFile;
	}

	bool IsSameHistoryEntry(const HistoryEntry& lhs, const wstring& worldName, const wstring& backupFile) {
		return lhs.worldName == worldName
			&& lhs.backupFile == backupFile;
	}

	vector<HistoryEntry>* TryGetHistoryVector(int configIndex) {
		auto it = g_appState.g_history.find(configIndex);
		if (it == g_appState.g_history.end()) return nullptr;
		return &it->second;
	}
}

static bool LoadLegacyHistoryFile(const filesystem::path& filename) {
	g_appState.g_history.clear();
	ifstream in(filename, ios::binary);
	if (!in.is_open()) return false;

	string line_utf8;
	int current_config_id = -1;

	while (getline(in, line_utf8)) {
		wstring line = utf8_to_wstring(line_utf8);
		if (line.empty() || line.front() == L'#') continue;

		if (line.front() == L'[' && line.back() == L']') {
			wstring section = line.substr(1, line.size() - 2);
			if (section.find(L"Config") == 0) {
				current_config_id = stoi(section.substr(6));
			}
		}
		else if (current_config_id != -1) {
			auto pos = line.find(L'=');
			if (pos == wstring::npos) continue;

			wstring key = line.substr(0, pos);
			wstring val = line.substr(pos + 1);
			if (key != L"Entry") continue;

			wstringstream ss(val);
			wstring segment;
			vector<wstring> segments;
			while (getline(ss, segment, L'|')) {
				segments.push_back(segment);
			}

			if (segments.size() >= 4) {
				HistoryEntry entry;
				entry.timestamp_str = segments[0];
				entry.worldName = segments[1];
				entry.backupFile = segments[2];
				entry.backupType = segments[3];
				entry.isPartialBackup = FolderRewindFormat::IsSmartBackupType(entry.backupType);
				entry.comment = segments.size() >= 5 ? segments[4] : L"";
				entry.isImportant = (segments.size() >= 6 && segments[5] == L"important");
				g_appState.g_history[current_config_id].push_back(entry);
			}
		}
	}

	return true;
}

void SaveHistory() {
	if (MigrationService::IsHistoryPersistenceBlocked()) return;
	const filesystem::path filename = L"history.json";
#ifdef _WIN32
	SetFileAttributesWin(filename.wstring(), 0);
#endif
	const bool saved = FolderRewindHistoryStore::SaveHistoryFile(filename, g_appState.configs, g_appState.g_history);
#ifdef _WIN32
	if (saved) SetFileAttributesWin(filename.wstring(), 1);
#endif
}

void LoadHistory() {
	const filesystem::path jsonFilename = L"history.json";
	const filesystem::path legacyFilename = L"history.dat";
	g_appState.g_history.clear();
	if (filesystem::exists(jsonFilename)) {
		map<int, vector<HistoryEntry>> loadedHistory;
		if (FolderRewindHistoryStore::LoadHistoryFile(jsonFilename, g_appState.configs, loadedHistory)) {
			g_appState.g_history = std::move(loadedHistory);
			return;
		}
		g_appState.g_history.clear();
	}

	if (filesystem::exists(legacyFilename) && LoadLegacyHistoryFile(legacyFilename)) {
		SaveHistory();
	}
}

void AddHistoryEntry(int configIndex, const wstring& worldName, const wstring& backupFile, const wstring& backupType, const wstring& comment, const wstring& worldPath) {
	HistoryEntry entry;
	auto configIt = g_appState.configs.find(configIndex);
	if (configIt != g_appState.configs.end()) {
		configIt->second.configId = FolderRewindFormat::EnsureConfigId(configIt->second.configId);
		entry.configId = configIt->second.configId;
	}
	entry.timestamp_str = FolderRewindFormat::MakeLocalHistoryTimestampString();
	entry.worldPath = worldPath;
	entry.worldName = worldName;
	entry.backupFile = backupFile;
	entry.backupType = backupType;
	entry.isPartialBackup = FolderRewindFormat::IsSmartBackupType(backupType);
	entry.comment = comment;

	g_appState.g_history[configIndex].push_back(entry);
	SaveHistory();
}

void RemoveHistoryEntry(int configIndex, const wstring& backupFileToRemove) {
	if (g_appState.g_history.count(configIndex)) {
		auto& history_vec = g_appState.g_history[configIndex];
		history_vec.erase(
			remove_if(history_vec.begin(), history_vec.end(),
				[&](const HistoryEntry& entry) {
					return entry.backupFile == backupFileToRemove;
				}),
			history_vec.end()
		);
	}
}

void RemoveHistoryEntry(int configIndex, const wstring& worldName, const wstring& backupFileToRemove) {
	if (g_appState.g_history.count(configIndex)) {
		auto& history_vec = g_appState.g_history[configIndex];
		history_vec.erase(
			remove_if(history_vec.begin(), history_vec.end(),
				[&](const HistoryEntry& entry) {
					return entry.worldName == worldName && entry.backupFile == backupFileToRemove;
				}),
			history_vec.end()
		);
	}
}

bool ExportHistoryToFile(const wstring& destinationPath, int configIndex) {
	if (configIndex < 0) {
		return FolderRewindHistoryStore::SaveHistoryFile(filesystem::path(destinationPath), g_appState.configs, g_appState.g_history);
	}

	map<int, Config> selectedConfigs;
	auto configIt = g_appState.configs.find(configIndex);
	if (configIt != g_appState.configs.end()) {
		selectedConfigs.emplace(configIndex, configIt->second);
	}

	map<int, vector<HistoryEntry>> selectedHistory;
	auto historyIt = g_appState.g_history.find(configIndex);
	if (historyIt != g_appState.g_history.end()) {
		selectedHistory.emplace(configIndex, historyIt->second);
	}

	return FolderRewindHistoryStore::SaveHistoryFile(filesystem::path(destinationPath), selectedConfigs, selectedHistory);
}

bool ImportHistoryFromFile(const wstring& sourcePath, int configIndex, bool mergeExisting) {
	auto configIt = g_appState.configs.find(configIndex);
	if (configIt == g_appState.configs.end()) return false;
	configIt->second.configId = FolderRewindFormat::EnsureConfigId(configIt->second.configId);
	const wstring targetConfigId = configIt->second.configId;

	ifstream in{ filesystem::path(sourcePath), ios::binary };
	if (!in.is_open()) return false;

	nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
	if (root.is_discarded() || !root.is_array()) return false;

	vector<HistoryEntry> parsedEntries;
	for (const auto& item : root) {
		HistoryEntry entry;
		wstring importedConfigId;
		int importedConfigIndex = -1;
		if (!FolderRewindHistoryStore::TryParseHistoryItem(item, entry, importedConfigId)
			&& !FolderRewindHistoryStore::TryParseLegacyHistoryItem(item, entry, importedConfigIndex)) {
			continue;
		}
		entry.configId = targetConfigId;
		parsedEntries.push_back(std::move(entry));
	}

	if (!root.empty() && parsedEntries.empty()) {
		return false;
	}

	if (!mergeExisting) {
		g_appState.g_history[configIndex].clear();
	}

	bool changed = false;
	for (const auto& entry : parsedEntries) {
		changed = UpsertHistoryEntry(configIndex, entry, false) || changed;
	}

	if (changed || !mergeExisting) {
		SaveHistory();
	}
	return true;
}

vector<HistoryEntry> GetHistoryEntriesForConfig(int configIndex) {
	auto* vec = TryGetHistoryVector(configIndex);
	return vec ? *vec : vector<HistoryEntry>{};
}

vector<HistoryEntry> GetHistoryEntriesForWorld(int configIndex, const wstring& worldName) {
	vector<HistoryEntry> result;
	auto* vec = TryGetHistoryVector(configIndex);
	if (!vec) return result;

	for (const auto& entry : *vec) {
		if (entry.worldName == worldName) {
			result.push_back(entry);
		}
	}
	sort(result.begin(), result.end(), [](const HistoryEntry& lhs, const HistoryEntry& rhs) {
		return lhs.timestamp_str < rhs.timestamp_str;
	});
	return result;
}

HistoryEntry* FindHistoryEntry(int configIndex, const wstring& worldName, const wstring& backupFile) {
	auto* vec = TryGetHistoryVector(configIndex);
	if (!vec) return nullptr;

	for (auto& entry : *vec) {
		if (IsSameHistoryEntry(entry, worldName, backupFile)) {
			return &entry;
		}
	}
	return nullptr;
}

bool TryGetHistoryEntry(int configIndex, const wstring& worldName, const wstring& backupFile, HistoryEntry& outEntry) {
	HistoryEntry* found = FindHistoryEntry(configIndex, worldName, backupFile);
	if (!found) return false;
	outEntry = *found;
	return true;
}

bool UpsertHistoryEntry(int configIndex, const HistoryEntry& entry, bool overwriteExisting) {
	if (entry.worldName.empty() || entry.backupFile.empty()) return false;

	auto& entries = g_appState.g_history[configIndex];
	for (auto& existing : entries) {
		if (!IsSameHistoryEntry(existing, entry)) {
			continue;
		}

		bool changed = false;
		if (overwriteExisting) {
			existing = entry;
			changed = true;
		}
		else {
			if (existing.configId.empty() && !entry.configId.empty()) {
				existing.configId = entry.configId;
				changed = true;
			}
			if (existing.timestamp_str.empty() && !entry.timestamp_str.empty()) {
				existing.timestamp_str = entry.timestamp_str;
				changed = true;
			}
			if (existing.worldPath.empty() && !entry.worldPath.empty()) {
				existing.worldPath = entry.worldPath;
				changed = true;
			}
			if (existing.comment.empty() && !entry.comment.empty()) {
				existing.comment = entry.comment;
				changed = true;
			}
			if (existing.backupType.empty() && !entry.backupType.empty()) {
				existing.backupType = entry.backupType;
				changed = true;
			}
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
			if (existing.cloudArchivedAtUtc.empty() && !entry.cloudArchivedAtUtc.empty()) {
				existing.cloudArchivedAtUtc = entry.cloudArchivedAtUtc;
				changed = true;
			}
			if (existing.cloudArchiveRemotePath.empty() && !entry.cloudArchiveRemotePath.empty()) {
				existing.cloudArchiveRemotePath = entry.cloudArchiveRemotePath;
				changed = true;
			}
			if (existing.cloudMetadataRecordRemotePath.empty() && !entry.cloudMetadataRecordRemotePath.empty()) {
				existing.cloudMetadataRecordRemotePath = entry.cloudMetadataRecordRemotePath;
				changed = true;
			}
			if (existing.cloudMetadataStateRemotePath.empty() && !entry.cloudMetadataStateRemotePath.empty()) {
				existing.cloudMetadataStateRemotePath = entry.cloudMetadataStateRemotePath;
				changed = true;
			}
		}
		return changed;
	}

	entries.push_back(entry);
	return true;
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
	HistoryEntry* entry = FindHistoryEntry(configIndex, worldName, backupFile);
	if (!entry) {
		return false;
	}

	entry->isCloudArchived = isCloudArchived;
	if (isCloudArchived) {
		if (!archivedAtUtc.empty()) entry->cloudArchivedAtUtc = archivedAtUtc;
		if (!archiveRemotePath.empty()) entry->cloudArchiveRemotePath = archiveRemotePath;
		if (!metadataRecordRemotePath.empty()) entry->cloudMetadataRecordRemotePath = metadataRecordRemotePath;
		if (!metadataStateRemotePath.empty()) entry->cloudMetadataStateRemotePath = metadataStateRemotePath;
	}
	else {
		// 取消云归档时同步清空远端路径，避免 UI 继续显示“可从云下载”。
		entry->cloudArchivedAtUtc.clear();
		entry->cloudArchiveRemotePath.clear();
		entry->cloudMetadataRecordRemotePath.clear();
		entry->cloudMetadataStateRemotePath.clear();
	}
	SaveHistory();
	return true;
}
