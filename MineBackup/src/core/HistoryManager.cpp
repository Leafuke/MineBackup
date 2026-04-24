#include <map>
#include <vector>
#include <fstream>
#include <sstream>
#include "HistoryManager.h"
#include "AppState.h"
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

	void SerializeHistoryEntry(nlohmann::json& item, int configIndex, const HistoryEntry& entry) {
		item["configIndex"] = configIndex;
		item["timestamp"] = wstring_to_utf8(entry.timestamp_str);
		item["worldPath"] = wstring_to_utf8(entry.worldPath);
		item["worldName"] = wstring_to_utf8(entry.worldName);
		item["backupFile"] = wstring_to_utf8(entry.backupFile);
		item["backupType"] = wstring_to_utf8(entry.backupType);
		item["comment"] = wstring_to_utf8(entry.comment);
		item["isImportant"] = entry.isImportant;
		item["isCloudArchived"] = entry.isCloudArchived;
		item["cloudArchivedAtUtc"] = wstring_to_utf8(entry.cloudArchivedAtUtc);
		item["cloudArchiveRemotePath"] = wstring_to_utf8(entry.cloudArchiveRemotePath);
		item["cloudMetadataRecordRemotePath"] = wstring_to_utf8(entry.cloudMetadataRecordRemotePath);
		item["cloudMetadataStateRemotePath"] = wstring_to_utf8(entry.cloudMetadataStateRemotePath);
	}

	bool TryDeserializeHistoryEntry(const nlohmann::json& item, int defaultConfigIndex, int& outConfigIndex, HistoryEntry& outEntry) {
		if (!item.is_object()) return false;

		outConfigIndex = item.value("configIndex", defaultConfigIndex);
		if (outConfigIndex < 0) return false;

		outEntry = HistoryEntry{};
		outEntry.timestamp_str = utf8_to_wstring(item.value("timestamp", std::string{}));
		outEntry.worldPath = utf8_to_wstring(item.value("worldPath", std::string{}));
		outEntry.worldName = utf8_to_wstring(item.value("worldName", std::string{}));
		outEntry.backupFile = utf8_to_wstring(item.value("backupFile", std::string{}));
		outEntry.backupType = utf8_to_wstring(item.value("backupType", std::string{}));
		outEntry.comment = utf8_to_wstring(item.value("comment", std::string{}));
		outEntry.isImportant = item.value("isImportant", false);
		outEntry.isCloudArchived = item.value("isCloudArchived", false);
		outEntry.cloudArchivedAtUtc = utf8_to_wstring(item.value("cloudArchivedAtUtc", std::string{}));
		outEntry.cloudArchiveRemotePath = utf8_to_wstring(item.value("cloudArchiveRemotePath", std::string{}));
		outEntry.cloudMetadataRecordRemotePath = utf8_to_wstring(item.value("cloudMetadataRecordRemotePath", std::string{}));
		outEntry.cloudMetadataStateRemotePath = utf8_to_wstring(item.value("cloudMetadataStateRemotePath", std::string{}));

		return !outEntry.worldName.empty() && !outEntry.backupFile.empty();
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
				entry.comment = segments.size() >= 5 ? segments[4] : L"";
				entry.isImportant = (segments.size() >= 6 && segments[5] == L"important");
				g_appState.g_history[current_config_id].push_back(entry);
			}
		}
	}

	return true;
}

void SaveHistory() {
	const filesystem::path filename = L"history.json";
#ifdef _WIN32
	SetFileAttributesWin(filename.wstring(), 0);
#endif
	ofstream out(filename, ios::binary | ios::trunc);
	if (!out.is_open()) return;

	nlohmann::json root = nlohmann::json::array();

	for (const auto& config_pair : g_appState.g_history) {
		for (const auto& entry : config_pair.second) {
			nlohmann::json item;
			SerializeHistoryEntry(item, config_pair.first, entry);
			root.push_back(std::move(item));
		}
	}

	out << root.dump(2);
	out.close();
#ifdef _WIN32
	SetFileAttributesWin(filename.wstring(), 1);
#endif
}

void LoadHistory() {
	const filesystem::path jsonFilename = L"history.json";
	const filesystem::path legacyFilename = L"history.dat";
	g_appState.g_history.clear();
	if (filesystem::exists(jsonFilename)) {
		try {
			ifstream in(jsonFilename, ios::binary);
			if (!in.is_open()) return;

			nlohmann::json root = nlohmann::json::parse(in);
			if (!root.is_array()) return;

			for (const auto& item : root) {
				int configIndex = -1;
				HistoryEntry entry;
				if (!TryDeserializeHistoryEntry(item, -1, configIndex, entry)) {
					continue;
				}
				g_appState.g_history[configIndex].push_back(entry);
			}
			return;
		}
		catch (...) {
			g_appState.g_history.clear();
		}
	}

	if (filesystem::exists(legacyFilename) && LoadLegacyHistoryFile(legacyFilename)) {
		SaveHistory();
	}
}

void AddHistoryEntry(int configIndex, const wstring& worldName, const wstring& backupFile, const wstring& backupType, const wstring& comment, const wstring& worldPath) {
	time_t now = time(0);
	tm ltm;
	localtime_s(&ltm, &now);
	wchar_t timeBuf[80];
	wcsftime(timeBuf, std::size(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);

	HistoryEntry entry;
	entry.timestamp_str = timeBuf;
	entry.worldPath = worldPath;
	entry.worldName = worldName;
	entry.backupFile = backupFile;
	entry.backupType = backupType;
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
	ofstream out{ filesystem::path(destinationPath), ios::binary | ios::trunc };
	if (!out.is_open()) return false;

	nlohmann::json root = nlohmann::json::array();
	if (configIndex < 0) {
		for (const auto& pair : g_appState.g_history) {
			for (const auto& entry : pair.second) {
				nlohmann::json item;
				SerializeHistoryEntry(item, pair.first, entry);
				root.push_back(std::move(item));
			}
		}
	}
	else {
		for (const auto& entry : g_appState.g_history[configIndex]) {
			nlohmann::json item;
			SerializeHistoryEntry(item, configIndex, entry);
			root.push_back(std::move(item));
		}
	}

	out << root.dump(2);
	return true;
}

bool ImportHistoryFromFile(const wstring& sourcePath, int configIndex, bool mergeExisting) {
	ifstream in{ filesystem::path(sourcePath), ios::binary };
	if (!in.is_open()) return false;

	nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
	if (root.is_discarded() || !root.is_array()) return false;

	if (!mergeExisting) {
		g_appState.g_history[configIndex].clear();
	}

	bool changed = false;
	for (const auto& item : root) {
		int importedConfigIndex = configIndex;
		HistoryEntry entry;
		if (!TryDeserializeHistoryEntry(item, configIndex, importedConfigIndex, entry)) {
			continue;
		}
		importedConfigIndex = configIndex;
		changed = UpsertHistoryEntry(importedConfigIndex, entry, false) || changed;
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
