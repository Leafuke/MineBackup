#include "LegacyMineBackup15Reader.h"

#include "FolderRewindHistoryStore.h"
#include "text_to_text.h"

#include <fstream>

using namespace std;

namespace LegacyMineBackup15Reader {
namespace {

wstring JsonString(const nlohmann::json& item, const char* key) {
	auto it = item.find(key);
	return it != item.end() && it->is_string() ? utf8_to_wstring(it->get<string>()) : L"";
}

void ReadStringArray(const nlohmann::json& root, const char* key, vector<wstring>& values) {
	values.clear();
	auto it = root.find(key);
	if (it == root.end() || !it->is_array()) return;
	for (const auto& item : *it) if (item.is_string()) values.push_back(utf8_to_wstring(item.get<string>()));
}

} // namespace

bool IsLegacyHistoryFile(const filesystem::path& path) {
#if !MINEBACKUP_ENABLE_V15_MIGRATION
	(void)path;
	return false;
#else
	ifstream in(path, ios::binary);
	if (!in.is_open()) return false;
	auto root = nlohmann::json::parse(in, nullptr, false);
	if (!root.is_array()) return false;
	for (const auto& item : root) {
		if (item.is_object() && (item.contains("configIndex") || item.contains("backupFile"))) return true;
	}
	return false;
#endif
}

bool ReadHistory(const filesystem::path& path, const map<int, Config>& configs, HistoryReadResult& result) {
#if !MINEBACKUP_ENABLE_V15_MIGRATION
	(void)path; (void)configs; (void)result;
	return false;
#else
	ifstream in(path, ios::binary);
	if (!in.is_open()) return false;
	auto root = nlohmann::json::parse(in, nullptr, false);
	if (!root.is_array()) return false;

	result = HistoryReadResult{};
	result.sourceItems = static_cast<int>(root.size());
	for (const auto& item : root) {
		HistoryEntry entry;
		wstring configId;
		int configIndex = -1;
		bool isNewItem = false;
		if (FolderRewindHistoryStore::TryParseHistoryItem(item, entry, configId)) {
			configIndex = FolderRewindHistoryStore::ResolveConfigIndexByConfigId(configs, configId);
			isNewItem = true;
			++result.newItems;
		}
		else {
			int legacyIndex = -1;
			if (!FolderRewindHistoryStore::TryParseLegacyHistoryItem(item, entry, legacyIndex)
				|| configs.find(legacyIndex) == configs.end()) {
				result.unmigrated.push_back(item);
				continue;
			}
			configIndex = legacyIndex;
			entry.configId = configs.at(configIndex).configId;
			++result.legacyItems;
		}
		if (configIndex < 0 || configs.find(configIndex) == configs.end()) {
			result.unmigrated.push_back(item);
			continue;
		}
		entry.configId = configs.at(configIndex).configId;
		auto& entries = result.history[configIndex];
		if (isNewItem) entries.insert(entries.begin(), std::move(entry));
		else entries.push_back(std::move(entry));
	}
	return true;
#endif
}

bool ReadMetadataSummary(const filesystem::path& metadataDir, MetadataSummary& summary, wstring& error) {
#if !MINEBACKUP_ENABLE_V15_MIGRATION
	(void)metadataDir; (void)summary; error = L"1.15 migration support is disabled.";
	return false;
#else
	ifstream in(metadataDir / L"metadata.json", ios::binary);
	if (!in.is_open()) { error = L"metadata.json is unavailable."; return false; }
	auto root = nlohmann::json::parse(in, nullptr, false);
	if (!root.is_object()) { error = L"metadata.json is malformed."; return false; }
	MetadataSummary parsed;
	parsed.version = root.value("version", 2);
	parsed.lastBackupFileName = JsonString(root, "lastBackupFileName");
	if (parsed.lastBackupFileName.empty()) parsed.lastBackupFileName = JsonString(root, "lastBackupFile");
	parsed.basedOnFullBackup = JsonString(root, "basedOnFullBackup");
	if (parsed.basedOnFullBackup.empty()) parsed.basedOnFullBackup = JsonString(root, "basedOnBackupFile");
	auto states = root.find("fileStates");
	if (states == root.end() || !states->is_object() || parsed.lastBackupFileName.empty()) {
		error = L"metadata.json is missing required state."; return false;
	}
	for (auto& [key, value] : states->items()) {
		if (!value.is_object()) { error = L"metadata.json contains an invalid file state."; return false; }
		parsed.fileStates[utf8_to_wstring(key)] = {
			value.value("size", static_cast<uintmax_t>(0)),
			value.value("lastWriteTimeTicks", static_cast<long long>(0))
		};
	}
	auto records = root.find("records");
	if (records != root.end() && records->is_array()) {
		for (const auto& item : *records) {
			if (!item.is_object()) continue;
			FolderRewindFormat::ChangeRecord record;
			record.archiveFileName = JsonString(item, "archiveFileName");
			record.backupType = JsonString(item, "backupType");
			record.basedOnFullBackup = JsonString(item, "basedOnFullBackup");
			record.previousBackupFileName = JsonString(item, "previousBackupFileName");
			record.createdAtUtc = JsonString(item, "createdAtUtc");
			if (!record.archiveFileName.empty()) parsed.recordIndex.push_back(std::move(record));
		}
	}
	summary = std::move(parsed);
	return true;
#endif
}

bool ReadChangeRecord(const filesystem::path& metadataDir, const wstring& archiveFileName, FolderRewindFormat::ChangeRecord& record, wstring& error) {
#if !MINEBACKUP_ENABLE_V15_MIGRATION
	(void)metadataDir; (void)archiveFileName; (void)record; error = L"1.15 migration support is disabled.";
	return false;
#else
	ifstream in(metadataDir / (archiveFileName + L".json"), ios::binary);
	if (!in.is_open()) { error = L"Legacy change record is unavailable."; return false; }
	auto root = nlohmann::json::parse(in, nullptr, false);
	if (!root.is_object()) { error = L"Legacy change record is malformed."; return false; }
	FolderRewindFormat::ChangeRecord parsed;
	parsed.archiveFileName = JsonString(root, "archiveFileName");
	if (parsed.archiveFileName.empty()) parsed.archiveFileName = archiveFileName;
	parsed.backupType = JsonString(root, "backupType");
	parsed.basedOnFullBackup = JsonString(root, "basedOnFullBackup");
	parsed.previousBackupFileName = JsonString(root, "previousBackupFileName");
	parsed.createdAtUtc = JsonString(root, "createdAtUtc");
	ReadStringArray(root, "addedFiles", parsed.addedFiles);
	ReadStringArray(root, "modifiedFiles", parsed.modifiedFiles);
	ReadStringArray(root, "deletedFiles", parsed.deletedFiles);
	ReadStringArray(root, "fullFileList", parsed.fullFileList);
	if (parsed.backupType.empty()) { error = L"Legacy change record has no backup type."; return false; }
	record = std::move(parsed);
	return true;
#endif
}

} // namespace LegacyMineBackup15Reader
