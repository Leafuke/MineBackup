#include "Broadcast.h"
#include "BackupManager.h"
#include "AppState.h"
#include "Globals.h"
#include "text_to_text.h"
#include "i18n.h"
#include "Console.h"
#include "HistoryManager.h"
#include "CloudSyncService.h"
#include "ConfigManager.h"
#include "json.hpp"
#include "PlatformCompat.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <cwctype>
#include <set>
#include <regex>
using namespace std;

enum class FolderState {
	BACKUP,
	RESTORE,
};

static inline const char* FolderStateToI18nKey(FolderState state) {
	switch (state) {
	case FolderState::BACKUP: return "OP_BACKUP";
	case FolderState::RESTORE: return "OP_RESTORE";
	default: return "OP_BACKUP";
	}
}

static inline void ToLowerInPlace(wstring& s) {
#ifdef _WIN32
	for (wchar_t& ch : s) ch = (wchar_t)towlower(ch);
#else
	(void)s;
#endif
}

static inline int NormalizeCompressionLevel(const wstring& method, int level) {
	int minLevel = 1;
	int maxLevel = 9;
	if (_wcsicmp(method.c_str(), L"zstd") == 0) {
		maxLevel = 22;
	}
	if (level < minLevel) return minLevel;
	if (level > maxLevel) return maxLevel;
	return level;
}

static inline wstring MakeWorldOperationKey(const filesystem::path& worldPath) {
	error_code ec;
	filesystem::path p = worldPath;

	// Normalize to an absolute, lexically-normal path so the same folder maps to one key.
	auto abs = filesystem::absolute(p, ec);
	if (!ec) p = abs;
	p = p.lexically_normal();

	wstring key = p.wstring();

#ifdef _WIN32
	// Windows paths are case-insensitive; unify casing and separators.
	for (wchar_t& ch : key) {
		if (ch == L'/') ch = L'\\';
	}
	ToLowerInPlace(key);
#else
	for (wchar_t& ch : key) {
		if (ch == L'\\') ch = L'/';
	}
#endif

	return key;
}

static mutex g_worldOpMutex;
static unordered_map<wstring, FolderState> g_worldOpInProgress;

class WorldOperationGuard {
public:
	WorldOperationGuard(const filesystem::path& worldPath, FolderState requested)
		: key_(MakeWorldOperationKey(worldPath)), requested_(requested) {
		lock_guard<mutex> lock(g_worldOpMutex);
		auto it = g_worldOpInProgress.find(key_);
		if (it == g_worldOpInProgress.end()) {
			g_worldOpInProgress.emplace(key_, requested_);
			acquired_ = true;
		}
		else {
			existing_ = it->second;
			acquired_ = false;
		}
	}

	WorldOperationGuard(const WorldOperationGuard&) = delete;
	WorldOperationGuard& operator=(const WorldOperationGuard&) = delete;

	WorldOperationGuard(WorldOperationGuard&& other) noexcept {
		*this = std::move(other);
	}
	WorldOperationGuard& operator=(WorldOperationGuard&& other) noexcept {
		if (this == &other) return *this;
		Release();
		key_ = std::move(other.key_);
		requested_ = other.requested_;
		existing_ = other.existing_;
		acquired_ = other.acquired_;
		other.acquired_ = false;
		return *this;
	}

	~WorldOperationGuard() { Release(); }

	bool Acquired() const { return acquired_; }
	FolderState Requested() const { return requested_; }
	FolderState Existing() const { return existing_; }

private:
	void Release() {
		if (!acquired_) return;
		lock_guard<mutex> lock(g_worldOpMutex);
		g_worldOpInProgress.erase(key_);
		acquired_ = false;
	}

	wstring key_;
	FolderState requested_ = FolderState::BACKUP;
	FolderState existing_ = FolderState::BACKUP;
	bool acquired_ = false;
};

wstring SanitizeFileName(const wstring& input);
vector<filesystem::path> GetChangedFiles(const filesystem::path& worldPath, const filesystem::path& metadataPath, const filesystem::path& backupPath, BackupCheckResult& out_result, map<wstring, BackupFileState>& out_currentState, BackupChangeSet& out_changeSet);
bool is_blacklisted(const filesystem::path& file_to_check, const filesystem::path& backup_source_root, const filesystem::path& original_world_root, const vector<wstring>& blacklist);

namespace {
	constexpr const wchar_t* kDeletedOnlyMarkerDir = L"__MineBackup_Internal";
	constexpr const wchar_t* kDeletedOnlyMarkerFile = L"__DeletedOnly.marker";
	const vector<wstring> kForcedBackupBlacklistRules = {
		L"regex:(^|[\\\\/])session\\.lock$",
		L"regex:(^|[\\\\/])lock$",
		L"regex:(^|[\\\\/]).*\\.lock$"
	};

	static vector<wstring> BuildEffectiveBackupBlacklist(const vector<wstring>& userBlacklist) {
		vector<wstring> effective = userBlacklist;
		for (const auto& forcedRule : kForcedBackupBlacklistRules) {
			const bool exists = any_of(effective.begin(), effective.end(), [&](const wstring& item) {
				return _wcsicmp(item.c_str(), forcedRule.c_str()) == 0;
				});
			if (!exists) {
				effective.push_back(forcedRule);
			}
		}
		return effective;
	}

	struct BackupMetadataRecordIndex {
		wstring archiveFileName;
		wstring backupType;
		wstring basedOnFullBackup;
		wstring previousBackupFileName;
		wstring createdAtUtc;
	};

	struct BackupMetadataSummary {
		int version = 2;
		wstring lastBackupFileName;
		wstring basedOnFullBackup;
		map<wstring, BackupFileState> fileStates;
		vector<BackupMetadataRecordIndex> records;
	};

	struct BackupChangeRecord {
		wstring archiveFileName;
		wstring backupType;
		wstring basedOnFullBackup;
		wstring previousBackupFileName;
		wstring createdAtUtc;
		vector<wstring> addedFiles;
		vector<wstring> modifiedFiles;
		vector<wstring> deletedFiles;
		vector<wstring> fullFileList;
	};

	enum class RestoreChainStatus {
		OK,
		METADATA_UNAVAILABLE,
		MISSING_BASE_FULL,
		INVALID
	};

	struct RestoreChainResult {
		RestoreChainStatus status = RestoreChainStatus::INVALID;
		vector<filesystem::path> chain;
		bool usedMetadata = false;
	};

	struct SmartRestoreArchiveGroup {
		filesystem::path archive;
		vector<wstring> files;
	};

	struct SmartRestorePlan {
		vector<filesystem::path> chain;
		vector<SmartRestoreArchiveGroup> archiveGroups;
	};

	static wstring GetMetadataRecordFileName(const wstring& backupFileName) {
		return backupFileName + L".json";
	}

	static filesystem::path GetMetadataRecordPath(const filesystem::path& metadataDir, const wstring& backupFileName) {
		return metadataDir / GetMetadataRecordFileName(backupFileName);
	}

	static filesystem::path GetMetadataDirectory(const Config& config, const wstring& worldName) {
		filesystem::path metadataFolder = JoinPath(config.backupPath, L"_metadata");
		metadataFolder /= worldName;
		return metadataFolder;
	}

	static string MakeUtcTimestampString() {
		auto now = chrono::system_clock::now();
		time_t nowTime = chrono::system_clock::to_time_t(now);
		tm utcTime{};
#ifdef _WIN32
		gmtime_s(&utcTime, &nowTime);
#else
		gmtime_r(&nowTime, &utcTime);
#endif
		char buf[32];
		strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
		return buf;
	}

	static bool IsIncrementalBackupType(const wstring& typeOrFileName) {
		return _wcsicmp(typeOrFileName.c_str(), L"Smart") == 0
			|| typeOrFileName.find(L"[Smart]") != wstring::npos;
	}

	static bool IsFullLikeBackupType(const wstring& typeOrFileName) {
		return _wcsicmp(typeOrFileName.c_str(), L"Full") == 0
			|| _wcsicmp(typeOrFileName.c_str(), L"Overwrite") == 0
			|| typeOrFileName.find(L"[Full]") != wstring::npos;
	}

	static nlohmann::json SerializeFileState(const BackupFileState& state) {
		nlohmann::json item;
		item["size"] = state.size;
		item["lastWriteTimeTicks"] = state.lastWriteTimeTicks;
		return item;
	}

	static bool TryDeserializeFileState(const nlohmann::json& item, BackupFileState& outState) {
		if (!item.is_object()) return false;
		outState.size = item.value("size", static_cast<uintmax_t>(0));
		outState.lastWriteTimeTicks = item.value("lastWriteTimeTicks", static_cast<long long>(0));
		return true;
	}

	static bool LoadBackupMetadataSummary(const filesystem::path& metadataDir, BackupMetadataSummary& outSummary) {
		filesystem::path metadataFile = metadataDir / L"metadata.json";
		if (!filesystem::exists(metadataFile)) return false;

		try {
			ifstream in(metadataFile, ios::binary);
			if (!in.is_open()) return false;

			nlohmann::json root = nlohmann::json::parse(in);
			outSummary = BackupMetadataSummary{};
			outSummary.version = root.value("version", 2);
			outSummary.lastBackupFileName = utf8_to_wstring(root.value("lastBackupFileName", string{}));
			if (outSummary.lastBackupFileName.empty()) {
				outSummary.lastBackupFileName = utf8_to_wstring(root.value("lastBackupFile", string{}));
			}
			outSummary.basedOnFullBackup = utf8_to_wstring(root.value("basedOnFullBackup", string{}));
			if (outSummary.basedOnFullBackup.empty()) {
				outSummary.basedOnFullBackup = utf8_to_wstring(root.value("basedOnBackupFile", string{}));
			}

			if (!root.contains("fileStates") || !root.at("fileStates").is_object()) {
				return false;
			}
			for (auto& [key, value] : root.at("fileStates").items()) {
				BackupFileState state;
				if (!TryDeserializeFileState(value, state)) {
					return false;
				}
				outSummary.fileStates.emplace(utf8_to_wstring(key), state);
			}

			if (root.contains("records") && root.at("records").is_array()) {
				for (const auto& recordJson : root.at("records")) {
					if (!recordJson.is_object()) continue;
					BackupMetadataRecordIndex record;
					record.archiveFileName = utf8_to_wstring(recordJson.value("archiveFileName", string{}));
					record.backupType = utf8_to_wstring(recordJson.value("backupType", string{}));
					record.basedOnFullBackup = utf8_to_wstring(recordJson.value("basedOnFullBackup", string{}));
					record.previousBackupFileName = utf8_to_wstring(recordJson.value("previousBackupFileName", string{}));
					record.createdAtUtc = utf8_to_wstring(recordJson.value("createdAtUtc", string{}));
					if (!record.archiveFileName.empty()) {
						outSummary.records.push_back(std::move(record));
					}
				}
			}

			return true;
		}
		catch (...) {
			return false;
		}
	}

	static bool LoadBackupChangeRecord(const filesystem::path& metadataDir, const wstring& archiveFileName, BackupChangeRecord& outRecord) {
		filesystem::path recordPath = GetMetadataRecordPath(metadataDir, archiveFileName);
		if (!filesystem::exists(recordPath)) return false;

		try {
			ifstream in(recordPath, ios::binary);
			if (!in.is_open()) return false;

			nlohmann::json root = nlohmann::json::parse(in);
			outRecord = BackupChangeRecord{};
			outRecord.archiveFileName = utf8_to_wstring(root.value("archiveFileName", string{}));
			outRecord.backupType = utf8_to_wstring(root.value("backupType", string{}));
			outRecord.basedOnFullBackup = utf8_to_wstring(root.value("basedOnFullBackup", string{}));
			outRecord.previousBackupFileName = utf8_to_wstring(root.value("previousBackupFileName", string{}));
			outRecord.createdAtUtc = utf8_to_wstring(root.value("createdAtUtc", string{}));

			auto loadStringArray = [](const nlohmann::json& parent, const char* key, vector<wstring>& out) {
				out.clear();
				if (!parent.contains(key) || !parent.at(key).is_array()) return;
				for (const auto& item : parent.at(key)) {
					if (item.is_string()) out.push_back(utf8_to_wstring(item.get<string>()));
				}
				sort(out.begin(), out.end());
			};

			loadStringArray(root, "addedFiles", outRecord.addedFiles);
			loadStringArray(root, "modifiedFiles", outRecord.modifiedFiles);
			loadStringArray(root, "deletedFiles", outRecord.deletedFiles);
			loadStringArray(root, "fullFileList", outRecord.fullFileList);
			if (outRecord.archiveFileName.empty()) {
				outRecord.archiveFileName = archiveFileName;
			}
			return true;
		}
		catch (...) {
			return false;
		}
	}

	static void SaveBackupChangeRecord(const filesystem::path& metadataDir, const BackupChangeRecord& record) {
		filesystem::create_directories(metadataDir);
		nlohmann::json root;
		root["archiveFileName"] = wstring_to_utf8(record.archiveFileName);
		root["backupType"] = wstring_to_utf8(record.backupType);
		root["basedOnFullBackup"] = wstring_to_utf8(record.basedOnFullBackup);
		root["previousBackupFileName"] = wstring_to_utf8(record.previousBackupFileName);
		root["createdAtUtc"] = wstring_to_utf8(record.createdAtUtc);

		auto writeStringArray = [](nlohmann::json& parent, const char* key, const vector<wstring>& values) {
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& value : values) {
				arr.push_back(wstring_to_utf8(value));
			}
			parent[key] = std::move(arr);
		};

		writeStringArray(root, "addedFiles", record.addedFiles);
		writeStringArray(root, "modifiedFiles", record.modifiedFiles);
		writeStringArray(root, "deletedFiles", record.deletedFiles);
		writeStringArray(root, "fullFileList", record.fullFileList);

		ofstream out(GetMetadataRecordPath(metadataDir, record.archiveFileName), ios::binary | ios::trunc);
		out << root.dump(2);
	}

	static void SaveBackupMetadataSummary(const filesystem::path& metadataDir, const BackupMetadataSummary& summary) {
		filesystem::create_directories(metadataDir);
		nlohmann::json root;
		root["version"] = summary.version;
		root["lastBackupFileName"] = wstring_to_utf8(summary.lastBackupFileName);
		root["basedOnFullBackup"] = wstring_to_utf8(summary.basedOnFullBackup);

		nlohmann::json fileStates = nlohmann::json::object();
		for (const auto& pair : summary.fileStates) {
			fileStates[wstring_to_utf8(pair.first)] = SerializeFileState(pair.second);
		}
		root["fileStates"] = std::move(fileStates);

		nlohmann::json records = nlohmann::json::array();
		for (const auto& record : summary.records) {
			nlohmann::json recordJson;
			recordJson["archiveFileName"] = wstring_to_utf8(record.archiveFileName);
			recordJson["backupType"] = wstring_to_utf8(record.backupType);
			recordJson["basedOnFullBackup"] = wstring_to_utf8(record.basedOnFullBackup);
			recordJson["previousBackupFileName"] = wstring_to_utf8(record.previousBackupFileName);
			recordJson["createdAtUtc"] = wstring_to_utf8(record.createdAtUtc);
			records.push_back(std::move(recordJson));
		}
		root["records"] = std::move(records);

		ofstream out(metadataDir / L"metadata.json", ios::binary | ios::trunc);
		out << root.dump(2);
	}

	static void UpdateMetadataFiles(const filesystem::path& metadataDir, const wstring& currentBackupFile, const wstring& baseBackupFile, const wstring& backupType, const map<wstring, BackupFileState>& currentState, const BackupChangeSet& changeSet) {
		BackupMetadataSummary summary;
		LoadBackupMetadataSummary(metadataDir, summary);

		const wstring previousLastBackupFile = summary.lastBackupFileName;
		const wstring normalizedBase = IsIncrementalBackupType(backupType)
			? (baseBackupFile.empty() ? currentBackupFile : baseBackupFile)
			: currentBackupFile;

		BackupChangeRecord record;
		record.archiveFileName = currentBackupFile;
		record.backupType = backupType;
		record.basedOnFullBackup = normalizedBase;
		record.previousBackupFileName = IsIncrementalBackupType(backupType) ? previousLastBackupFile : L"";
		record.createdAtUtc = utf8_to_wstring(MakeUtcTimestampString());
		record.addedFiles = changeSet.addedFiles;
		record.modifiedFiles = changeSet.modifiedFiles;
		record.deletedFiles = changeSet.deletedFiles;
		for (const auto& pair : currentState) {
			record.fullFileList.push_back(pair.first);
		}
		sort(record.fullFileList.begin(), record.fullFileList.end());
		SaveBackupChangeRecord(metadataDir, record);

		summary.version = 2;
		summary.lastBackupFileName = currentBackupFile;
		summary.basedOnFullBackup = normalizedBase;
		summary.fileStates = currentState;
		summary.records.erase(
			remove_if(summary.records.begin(), summary.records.end(), [&](const BackupMetadataRecordIndex& item) {
				return _wcsicmp(item.archiveFileName.c_str(), currentBackupFile.c_str()) == 0;
			}),
			summary.records.end()
		);

		BackupMetadataRecordIndex indexRecord;
		indexRecord.archiveFileName = currentBackupFile;
		indexRecord.backupType = backupType;
		indexRecord.basedOnFullBackup = normalizedBase;
		indexRecord.previousBackupFileName = record.previousBackupFileName;
		indexRecord.createdAtUtc = record.createdAtUtc;
		summary.records.push_back(std::move(indexRecord));
		sort(summary.records.begin(), summary.records.end(), [](const BackupMetadataRecordIndex& a, const BackupMetadataRecordIndex& b) {
			if (a.createdAtUtc != b.createdAtUtc) return a.createdAtUtc < b.createdAtUtc;
			return a.archiveFileName < b.archiveFileName;
		});

		SaveBackupMetadataSummary(metadataDir, summary);
	}

	static void InvalidateBackupMetadata(const Config& config, const wstring& worldName, const wstring& deletedBackupFile, const wstring& renamedOldFile = L"", const wstring& renamedNewFile = L"") {
		filesystem::path metadataDir = GetMetadataDirectory(config, worldName);
		error_code ec;
		if (!deletedBackupFile.empty()) {
			filesystem::remove(GetMetadataRecordPath(metadataDir, deletedBackupFile), ec);
		}
		if (!renamedOldFile.empty() && !renamedNewFile.empty()) {
			filesystem::path oldRecord = GetMetadataRecordPath(metadataDir, renamedOldFile);
			filesystem::path newRecord = GetMetadataRecordPath(metadataDir, renamedNewFile);
			filesystem::rename(oldRecord, newRecord, ec);
		}
		filesystem::remove(metadataDir / L"metadata.json", ec);
	}

	static vector<wstring> ToSortedVector(const set<wstring>& values) {
		return vector<wstring>(values.begin(), values.end());
	}

	static bool TryRepairMetadataAfterSafeDelete(
		const Config& config,
		const wstring& worldName,
		const wstring& deletedBackupFile,
		const wstring& mergedOldFile,
		const wstring& mergedFinalFile,
		const wstring& mergedBackupType,
		string& errorMessage
	) {
		errorMessage.clear();
		filesystem::path metadataDir = GetMetadataDirectory(config, worldName);

		BackupMetadataSummary summary;
		if (!LoadBackupMetadataSummary(metadataDir, summary)) {
			errorMessage = "Cannot load metadata summary.";
			return false;
		}

		BackupChangeRecord deletedRecord;
		if (!LoadBackupChangeRecord(metadataDir, deletedBackupFile, deletedRecord)) {
			errorMessage = "Cannot load deleted backup metadata record.";
			return false;
		}

		BackupChangeRecord mergedRecord;
		if (!LoadBackupChangeRecord(metadataDir, mergedOldFile, mergedRecord)) {
			errorMessage = "Cannot load merged target metadata record.";
			return false;
		}

		set<wstring> fullAfterDeleted(deletedRecord.fullFileList.begin(), deletedRecord.fullFileList.end());
		set<wstring> fullBeforeDeleted = fullAfterDeleted;
		for (const auto& path : deletedRecord.addedFiles) {
			fullBeforeDeleted.erase(path);
		}
		for (const auto& path : deletedRecord.deletedFiles) {
			fullBeforeDeleted.insert(path);
		}

		set<wstring> fullAfterMerged(mergedRecord.fullFileList.begin(), mergedRecord.fullFileList.end());

		set<wstring> mergedAdded;
		set<wstring> mergedDeleted;
		set_difference(
			fullAfterMerged.begin(), fullAfterMerged.end(),
			fullBeforeDeleted.begin(), fullBeforeDeleted.end(),
			inserter(mergedAdded, mergedAdded.end())
		);
		set_difference(
			fullBeforeDeleted.begin(), fullBeforeDeleted.end(),
			fullAfterMerged.begin(), fullAfterMerged.end(),
			inserter(mergedDeleted, mergedDeleted.end())
		);

		set<wstring> modifiedCandidates;
		modifiedCandidates.insert(deletedRecord.modifiedFiles.begin(), deletedRecord.modifiedFiles.end());
		modifiedCandidates.insert(mergedRecord.modifiedFiles.begin(), mergedRecord.modifiedFiles.end());
		for (const auto& path : mergedRecord.addedFiles) {
			if (fullBeforeDeleted.count(path) > 0 && fullAfterMerged.count(path) > 0) {
				modifiedCandidates.insert(path);
			}
		}

		set<wstring> mergedModified;
		for (const auto& path : modifiedCandidates) {
			if (fullBeforeDeleted.count(path) == 0 || fullAfterMerged.count(path) == 0) continue;
			if (mergedAdded.count(path) > 0 || mergedDeleted.count(path) > 0) continue;
			mergedModified.insert(path);
		}

		BackupChangeRecord repaired = mergedRecord;
		repaired.archiveFileName = mergedFinalFile;
		repaired.backupType = mergedBackupType;
		repaired.createdAtUtc = mergedRecord.createdAtUtc.empty() ? utf8_to_wstring(MakeUtcTimestampString()) : mergedRecord.createdAtUtc;

		if (IsIncrementalBackupType(mergedBackupType)) {
			repaired.previousBackupFileName = deletedRecord.previousBackupFileName;
			repaired.basedOnFullBackup = !deletedRecord.basedOnFullBackup.empty() ? deletedRecord.basedOnFullBackup : mergedRecord.basedOnFullBackup;
			repaired.addedFiles = ToSortedVector(mergedAdded);
			repaired.deletedFiles = ToSortedVector(mergedDeleted);
			repaired.modifiedFiles = ToSortedVector(mergedModified);
		}
		else {
			repaired.previousBackupFileName.clear();
			repaired.basedOnFullBackup = mergedFinalFile;
			repaired.addedFiles = ToSortedVector(fullAfterMerged);
			repaired.deletedFiles.clear();
			repaired.modifiedFiles.clear();
		}
		repaired.fullFileList = ToSortedVector(fullAfterMerged);

		SaveBackupChangeRecord(metadataDir, repaired);

		error_code ec;
		filesystem::remove(GetMetadataRecordPath(metadataDir, deletedBackupFile), ec);
		if (mergedOldFile != mergedFinalFile) {
			ec.clear();
			filesystem::remove(GetMetadataRecordPath(metadataDir, mergedOldFile), ec);
		}

		vector<BackupMetadataRecordIndex> repairedRecords;
		repairedRecords.reserve(summary.records.size());
		for (auto record : summary.records) {
			if (_wcsicmp(record.archiveFileName.c_str(), deletedBackupFile.c_str()) == 0) {
				continue;
			}

			if (_wcsicmp(record.archiveFileName.c_str(), mergedOldFile.c_str()) == 0) {
				record.archiveFileName = mergedFinalFile;
				record.backupType = repaired.backupType;
				record.basedOnFullBackup = repaired.basedOnFullBackup;
				record.previousBackupFileName = repaired.previousBackupFileName;
				record.createdAtUtc = repaired.createdAtUtc;
			}

			if (_wcsicmp(record.previousBackupFileName.c_str(), deletedBackupFile.c_str()) == 0) {
				record.previousBackupFileName = mergedFinalFile;
			}
			if (mergedOldFile != mergedFinalFile && _wcsicmp(record.previousBackupFileName.c_str(), mergedOldFile.c_str()) == 0) {
				record.previousBackupFileName = mergedFinalFile;
			}

			if (_wcsicmp(record.basedOnFullBackup.c_str(), deletedBackupFile.c_str()) == 0) {
				record.basedOnFullBackup = mergedFinalFile;
			}
			if (mergedOldFile != mergedFinalFile && _wcsicmp(record.basedOnFullBackup.c_str(), mergedOldFile.c_str()) == 0) {
				record.basedOnFullBackup = mergedFinalFile;
			}

			repairedRecords.push_back(std::move(record));
		}

		summary.records = std::move(repairedRecords);
		sort(summary.records.begin(), summary.records.end(), [](const BackupMetadataRecordIndex& a, const BackupMetadataRecordIndex& b) {
			if (a.createdAtUtc != b.createdAtUtc) return a.createdAtUtc < b.createdAtUtc;
			return a.archiveFileName < b.archiveFileName;
		});

		if (_wcsicmp(summary.lastBackupFileName.c_str(), deletedBackupFile.c_str()) == 0) {
			summary.lastBackupFileName = mergedFinalFile;
		}
		if (_wcsicmp(summary.lastBackupFileName.c_str(), mergedOldFile.c_str()) == 0) {
			summary.lastBackupFileName = mergedFinalFile;
		}

		if (_wcsicmp(summary.basedOnFullBackup.c_str(), deletedBackupFile.c_str()) == 0) {
			summary.basedOnFullBackup = mergedFinalFile;
		}
		if (mergedOldFile != mergedFinalFile && _wcsicmp(summary.basedOnFullBackup.c_str(), mergedOldFile.c_str()) == 0) {
			summary.basedOnFullBackup = mergedFinalFile;
		}

		SaveBackupMetadataSummary(metadataDir, summary);
		return true;
	}

	static void ClearReadonlyAttributesRecursively(const filesystem::path& dir) {
		error_code ec;
		if (!filesystem::exists(dir, ec) || ec) return;
		for (const auto& entry : filesystem::recursive_directory_iterator(dir, filesystem::directory_options::skip_permission_denied, ec)) {
			if (ec) break;
			filesystem::permissions(entry.path(), filesystem::perms::owner_all, filesystem::perm_options::add, ec);
		}
		filesystem::permissions(dir, filesystem::perms::owner_all, filesystem::perm_options::add, ec);
	}

	static filesystem::path CreateSafeRestoreTempDirectoryPath(const filesystem::path& targetDir) {
		filesystem::path normalized = targetDir.lexically_normal();
		filesystem::path parent = normalized.parent_path();
		if (parent.empty()) {
			throw runtime_error("Restore target has no parent directory.");
		}

		filesystem::path base = parent / (normalized.filename().wstring() + L"-Temp");
		filesystem::path candidate = base;
		int suffix = 1;
		error_code ec;
		while (filesystem::exists(candidate, ec)) {
			candidate = filesystem::path(base.wstring() + L"-" + to_wstring(suffix++));
		}
		return candidate;
	}

	static bool TryPrepareSafeRestoreWorkspace(const filesystem::path& targetDir, filesystem::path& tempDir, string& errorMessage) {
		tempDir.clear();
		errorMessage.clear();
		error_code ec;
		const bool targetExists = filesystem::exists(targetDir, ec);
		if (ec) {
			errorMessage = "Failed to inspect restore target: " + ec.message();
			return false;
		}

		if (!targetExists) {
			filesystem::create_directories(targetDir, ec);
			if (ec) {
				errorMessage = "Failed to create restore target: " + ec.message();
				return false;
			}
			return true;
		}

		try {
			tempDir = CreateSafeRestoreTempDirectoryPath(targetDir);
		}
		catch (const exception& ex) {
			errorMessage = ex.what();
			return false;
		}

		filesystem::rename(targetDir, tempDir, ec);
		if (ec) {
			errorMessage = "Failed to move restore target to snapshot: " + ec.message();
			tempDir.clear();
			return false;
		}

		filesystem::create_directories(targetDir, ec);
		if (!ec) {
			return true;
		}

		const string createError = ec.message();

		// Best-effort rollback when workspace preparation fails after snapshot move.
		error_code cleanupEc;
		if (filesystem::exists(targetDir, cleanupEc) && !cleanupEc) {
			ClearReadonlyAttributesRecursively(targetDir);
			filesystem::remove_all(targetDir, cleanupEc);
		}

		error_code rollbackEc;
		filesystem::rename(tempDir, targetDir, rollbackEc);
		if (rollbackEc) {
			errorMessage = "Failed to create clean workspace (" + createError + "), rollback also failed: " + rollbackEc.message();
			return false;
		}

		tempDir.clear();
		errorMessage = "Failed to create clean workspace: " + createError;
		return false;
	}

	static wstring ToLowerSlash(wstring value) {
		transform(value.begin(), value.end(), value.begin(), ::towlower);
		replace(value.begin(), value.end(), L'\\', L'/');
		return value;
	}

	static void TrimTrailingSlash(wstring& value) {
		while (!value.empty() && (value.back() == L'/' || value.back() == L'\\')) {
			value.pop_back();
		}
	}

	static bool PathEqualsOrUnder(const wstring& path, const wstring& rule) {
		if (rule.empty()) return false;
		if (path == rule) return true;
		return path.size() > rule.size()
			&& path.rfind(rule, 0) == 0
			&& path[rule.size()] == L'/';
	}

	static bool PathHasSegment(const wstring& path, const wstring& segment) {
		if (segment.empty() || segment.find(L'/') != wstring::npos) return false;
		size_t start = 0;
		while (start <= path.size()) {
			size_t end = path.find(L'/', start);
			wstring current = path.substr(start, end == wstring::npos ? wstring::npos : end - start);
			if (current == segment) return true;
			if (end == wstring::npos) break;
			start = end + 1;
		}
		return false;
	}

	static bool IsInRestoreWhitelist(const filesystem::path& entryPath, const filesystem::path& rootDir, const vector<wstring>& whitelist) {
		wstring entryNameLower = ToLowerSlash(entryPath.filename().wstring());
		wstring entryPathLower = ToLowerSlash(entryPath.wstring());

		wstring relativePathLower;
		error_code ec;
		filesystem::path relativePath = filesystem::relative(entryPath, rootDir, ec);
		if (!ec) {
			relativePathLower = ToLowerSlash(relativePath.wstring());
		}

		for (const auto& ruleOrig : whitelist) {
			if (ruleOrig.empty()) continue;
			if (ruleOrig.rfind(L"regex:", 0) == 0) {
				try {
					wregex pattern(ruleOrig.substr(6), regex_constants::icase | regex_constants::ECMAScript);
					if (regex_search(entryPath.wstring(), pattern) ||
						(!relativePath.empty() && regex_search(relativePath.wstring(), pattern))) {
						return true;
					}
				}
				catch (const regex_error&) {
				}
				continue;
			}

			wstring rule = ToLowerSlash(ruleOrig);
			TrimTrailingSlash(rule);

			if (entryNameLower == rule || PathEqualsOrUnder(relativePathLower, rule) || PathHasSegment(relativePathLower, rule)) {
				return true;
			}

			filesystem::path rulePath(ruleOrig);
			if (rulePath.is_absolute()) {
				wstring rulePathLower = ToLowerSlash(rulePath.wstring());
				TrimTrailingSlash(rulePathLower);
				if (PathEqualsOrUnder(entryPathLower, rulePathLower)) {
					return true;
				}
			}
		}
		return false;
	}

	static bool IsPathOrAncestorInRestoreWhitelist(const filesystem::path& entryPath, const filesystem::path& rootDir, const vector<wstring>& whitelist) {
		if (IsInRestoreWhitelist(entryPath, rootDir, whitelist)) {
			return true;
		}

		error_code ec;
		filesystem::path rootFull = filesystem::absolute(rootDir, ec).lexically_normal();
		filesystem::path current = filesystem::is_directory(entryPath, ec) ? entryPath : entryPath.parent_path();
		while (!current.empty()) {
			filesystem::path currentFull = filesystem::absolute(current, ec).lexically_normal();
			if (currentFull == rootFull) {
				break;
			}
			if (IsInRestoreWhitelist(currentFull, rootDir, whitelist)) {
				return true;
			}
			current = currentFull.parent_path();
		}
		return false;
	}

	static void CleanupInternalRestoreMarkers(const filesystem::path& targetDir) {
		error_code ec;
		filesystem::path internalDir = targetDir / kDeletedOnlyMarkerDir;
		if (filesystem::exists(internalDir, ec) && !ec) {
			ClearReadonlyAttributesRecursively(internalDir);
			filesystem::remove_all(internalDir, ec);
		}
	}

	static void CopyRestoreWhitelistEntries(const filesystem::path& sourceDir, const filesystem::path& targetDir, const vector<wstring>& whitelist) {
		if (whitelist.empty()) return;
		error_code ec;
		if (!filesystem::exists(sourceDir, ec) || ec) return;

		for (const auto& dirEntry : filesystem::recursive_directory_iterator(sourceDir, filesystem::directory_options::skip_permission_denied, ec)) {
			if (ec) break;
			if (!dirEntry.is_directory()) continue;
			if (!IsPathOrAncestorInRestoreWhitelist(dirEntry.path(), sourceDir, whitelist)) continue;

			filesystem::path relPath = filesystem::relative(dirEntry.path(), sourceDir, ec);
			if (ec) continue;
			filesystem::create_directories(targetDir / relPath, ec);
		}

		for (const auto& fileEntry : filesystem::recursive_directory_iterator(sourceDir, filesystem::directory_options::skip_permission_denied, ec)) {
			if (ec) break;
			if (!fileEntry.is_regular_file()) continue;
			if (!IsPathOrAncestorInRestoreWhitelist(fileEntry.path(), sourceDir, whitelist)) continue;

			filesystem::path relPath = filesystem::relative(fileEntry.path(), sourceDir, ec);
			if (ec) continue;
			filesystem::path destPath = targetDir / relPath;
			filesystem::create_directories(destPath.parent_path(), ec);
			if (filesystem::exists(destPath, ec) && !ec) {
				continue;
			}
			filesystem::copy_file(fileEntry.path(), destPath, filesystem::copy_options::overwrite_existing, ec);
		}
	}

	static bool TryCommitSafeRestoreWorkspace(const filesystem::path& targetDir, const filesystem::path& tempDir, const vector<wstring>& whitelist, string& errorMessage) {
		errorMessage.clear();
		try {
			CleanupInternalRestoreMarkers(targetDir);
			CopyRestoreWhitelistEntries(tempDir, targetDir, whitelist);
			if (!tempDir.empty() && filesystem::exists(tempDir)) {
				ClearReadonlyAttributesRecursively(tempDir);
				filesystem::remove_all(tempDir);
			}
			return true;
		}
		catch (const exception& ex) {
			errorMessage = ex.what();
			return false;
		}
	}

	static bool TryRollbackSafeRestoreWorkspace(const filesystem::path& targetDir, const filesystem::path& tempDir, string& errorMessage) {
		errorMessage.clear();

		error_code ec;
		if (tempDir.empty()) {
			errorMessage = "Snapshot directory path is empty.";
			return false;
		}

		if (!filesystem::exists(tempDir, ec) || ec) {
			errorMessage = "Snapshot directory is missing.";
			return false;
		}

		if (filesystem::exists(targetDir, ec) && !ec) {
			ClearReadonlyAttributesRecursively(targetDir);
			filesystem::remove_all(targetDir, ec);
			if (ec) {
				errorMessage = "Failed to clean restore target before rollback: " + ec.message();
				return false;
			}
		}

		filesystem::rename(tempDir, targetDir, ec);
		if (ec) {
			errorMessage = "Failed to restore snapshot: " + ec.message();
			return false;
		}

		return true;
	}

	static bool CreateDeletionOnlyArchive(const Config& config, const filesystem::path& archivePath, Console& console) {
		wstringstream nameBuilder;
		nameBuilder << L"MineBackup_DeleteOnly_" << chrono::steady_clock::now().time_since_epoch().count();
		filesystem::path tempDir = filesystem::temp_directory_path() / nameBuilder.str();
		bool success = false;
		try {
			filesystem::path internalDir = tempDir / kDeletedOnlyMarkerDir;
			filesystem::create_directories(internalDir);
			ofstream marker(internalDir / kDeletedOnlyMarkerFile, ios::binary | ios::trunc);
			marker << MakeUtcTimestampString();
			marker.close();

			const int normalizedZipLevel = NormalizeCompressionLevel(config.zipMethod, config.zipLevel);
			wstring command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod + L" -mx=" + to_wstring(normalizedZipLevel) +
				L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" -ssw \"" + archivePath.wstring() + L"\" \"*\"";
			success = RunCommandInBackground(command, console, config.useLowPriority, tempDir.wstring());
		}
		catch (const exception& ex) {
			console.AddLog("[Error] Failed to create deletion-only archive: %s", ex.what());
		}

		error_code ec;
		if (filesystem::exists(tempDir, ec) && !ec) {
			ClearReadonlyAttributesRecursively(tempDir);
			filesystem::remove_all(tempDir, ec);
		}
		return success;
	}
}

void AddBackupToWESnapshots(const Config& config, const wstring& worldName, const wstring& backupFile, Console& console) {
	console.AddLog(L("LOG_WE_INTEGRATION_START"), wstring_to_utf8(worldName).c_str());

	// 创建快照路径
	filesystem::path we_base_path = config.weSnapshotPath;
	if (we_base_path.empty()) {
		we_base_path = GetDocumentsPath();
		if (we_base_path.empty()) {
			console.AddLog("[Error] Could not determine Documents folder path.");
			console.AddLog(L("LOG_WE_INTEGRATION_FAILED"));
			return;
		}
		we_base_path /= "MineBackup-WE-Snap";
	}

	auto now = chrono::system_clock::now();
	auto in_time_t = chrono::system_clock::to_time_t(now);
	wstringstream ss;
	tm t;
	localtime_s(&t, &in_time_t);
	ss << put_time(&t, L"%Y-%m-%d-%H-%M-%S");

	filesystem::path final_snapshot_path = we_base_path / worldName / ss.str();

	error_code ec;
	filesystem::create_directories(final_snapshot_path, ec);
	if (ec) {
		console.AddLog("[Error] Failed to create snapshot directory: %s", ec.message().c_str());
		console.AddLog(L("LOG_WE_INTEGRATION_FAILED"));
		return;
	}
	console.AddLog(L("LOG_WE_INTEGRATION_PATH_OK"), wstring_to_utf8(final_snapshot_path.wstring()).c_str());

	// WorldEdit 快照需要的核心文件/文件夹
	const vector<wstring> essential_parts = { L"region", L"poi", L"entities", L"level.dat" };

	// 还原链处理
	filesystem::path sourceDir = JoinPath(config.backupPath, worldName);
	filesystem::path targetBackupPath = sourceDir / backupFile;

	if ((backupFile.find(L"[Smart]") == wstring::npos && backupFile.find(L"[Full]") == wstring::npos) || !filesystem::exists(targetBackupPath)) {
		console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(backupFile).c_str());
		return;
	}

	// 收集所有相关的备份文件
	vector<filesystem::path> backupsToApply;

	if (backupFile.find(L"[Smart]") != wstring::npos) {
		// 寻找基础的完整备份
		filesystem::path baseFullBackup;
		auto baseFullTime = filesystem::file_time_type{};

		for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
			if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Full]") != wstring::npos) {
				if (entry.last_write_time() < filesystem::last_write_time(targetBackupPath) && entry.last_write_time() > baseFullTime) {
					baseFullTime = entry.last_write_time();
					baseFullBackup = entry.path();
				}
			}
		}

		if (baseFullBackup.empty()) {
			console.AddLog(L("LOG_BACKUP_SMART_NO_FOUND"));
			return;
		}

		console.AddLog(L("LOG_BACKUP_SMART_FOUND"), wstring_to_utf8(baseFullBackup.filename().wstring()).c_str());
		backupsToApply.push_back(baseFullBackup);

		// 收集从基础备份到目标备份之间的所有增量备份
		for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
			if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Smart]") != wstring::npos) {
				if (entry.last_write_time() > baseFullTime && entry.last_write_time() <= filesystem::last_write_time(targetBackupPath)) {
					backupsToApply.push_back(entry.path());
				}
			}
		}
		// 按时间顺序排序
		sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
			return filesystem::last_write_time(a) < filesystem::last_write_time(b);
			});
	}
	else {
		backupsToApply.push_back(targetBackupPath);
	}

	// 依次解压核心文件/文件夹
	wstring files_to_extract_str;
	for (const auto& part : essential_parts) {
		files_to_extract_str += L" \"" + part + L"\"";
	}

	for (size_t i = 0; i < backupsToApply.size(); ++i) {
		const auto& backup = backupsToApply[i];
		console.AddLog(L("RESTORE_STEPS"), i + 1, backupsToApply.size(), wstring_to_utf8(backup.filename().wstring()).c_str());
		wstring command = L"\"" + config.zipPath + L"\" x \"" + backup.wstring() + L"\" -o\"" + final_snapshot_path.wstring() + L"\"" + files_to_extract_str + L" -r -y";
		if (!RunCommandInBackground(command, console, config.useLowPriority)) {
			console.AddLog(L("LOG_WE_INTEGRATION_FAILED"));
			return;
		}
	}

	// 修改 WorldEdit 配置文件（与原有实现一致）
	console.AddLog(L("LOG_WE_INTEGRATION_CONFIG_UPDATE_START"));
	filesystem::path save_root(config.saveRoot);
	filesystem::path we_config_path;
	if (filesystem::exists(save_root.parent_path() / "config" / "worldedit" / "worldedit.properties")) {
		we_config_path = save_root.parent_path() / "config" / "worldedit" / "worldedit.properties";
	}
	else if (filesystem::exists(save_root / "config" / "worldedit" / "worldedit.properties")) {
		we_config_path = save_root / "config" / "worldedit" / "worldedit.properties";
	}
	else if (filesystem::exists(save_root / "worldedit.conf")) {
		we_config_path = save_root / "worldedit.conf";
	}

	if (!filesystem::exists(we_config_path)) {
		console.AddLog(L("LOG_WE_INTEGRATION_CONFIG_NOT_FOUND"), wstring_to_utf8(we_config_path.wstring()).c_str());
		console.AddLog(L("LOG_WE_INTEGRATION_SUCCESS"), wstring_to_utf8(worldName).c_str());
		return;
	}

	ifstream infile(we_config_path);
	vector<string> lines;
	string line;
	bool key_found = false;
	string new_line = "snapshots-dir=" + wstring_to_utf8(we_base_path.wstring());
	replace(new_line.begin(), new_line.end(), '\\', '/');

	while (getline(infile, line)) {
		if (line.rfind("snapshots-dir=", 0) == 0) {
			lines.push_back(new_line);
			key_found = true;
		}
		else {
			lines.push_back(line);
		}
	}
	infile.close();

	if (!key_found) {
		lines.push_back(new_line);
	}

	ofstream outfile(we_config_path);
	if (outfile.is_open()) {
		for (const auto& l : lines) {
			outfile << l << endl;
		}
		outfile.close();
		console.AddLog(L("LOG_WE_INTEGRATION_CONFIG_UPDATE_SUCCESS"));
	}
	else {
		console.AddLog(L("LOG_WE_INTEGRATION_CONFIG_UPDATE_FAIL"));
		console.AddLog(L("LOG_WE_INTEGRATION_FAILED"));
		return;
	}

	console.AddLog(L("LOG_WE_INTEGRATION_SUCCESS"), wstring_to_utf8(worldName).c_str());
}

void UpdateMetadataFile(const filesystem::path& metadataPath, const wstring& newBackupFile, const wstring& basedOnBackupFile, const wstring& backupType, const map<wstring, BackupFileState>& currentState, const BackupChangeSet& changeSet) {
	UpdateMetadataFiles(metadataPath, newBackupFile, basedOnBackupFile, backupType, currentState, changeSet);
}


// 限制备份文件数量，超出则自动删除最旧的
void LimitBackupFiles(const Config& config, const int& configIndex, const wstring& folderPath, int limit, Console* console)
{
	if (limit <= 0) return;
	namespace fs = filesystem;
	vector<fs::directory_entry> files;

	// 收集所有常规文件
	try {
		if (!fs::exists(folderPath) || !fs::is_directory(folderPath))
			return;
		for (const auto& entry : fs::directory_iterator(folderPath)) {
			if (entry.is_regular_file())
				files.push_back(entry);
		}
	}
	catch (const fs::filesystem_error& e) {
		if (console) console->AddLog(L("LOG_ERROR_SCAN_BACKUP_DIR"), e.what());
		return;
	}

	// 如果未超出限制，无需处理
	if ((int)files.size() <= limit) return;

	const auto& history_it = g_appState.g_history.find(configIndex);
	bool history_available = (history_it != g_appState.g_history.end());

	// 按最后写入时间升序排序（最旧的在前）
	sort(files.begin(), files.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
		return fs::last_write_time(a) < fs::last_write_time(b);
		});

	vector<fs::directory_entry> deletable_files;
	for (const auto& file : files) {
		bool is_important = false;
		if (history_available) {
			for (const auto& entry : history_it->second) {
				if (entry.worldName == file.path().parent_path().filename().wstring() && entry.backupFile == file.path().filename().wstring()) {
					if (entry.isImportant) {
						is_important = true;
						console->AddLog(L("LOG_INFO_BACKUP_MARKED_IMPORTANT"), wstring_to_utf8(file.path().filename().wstring()).c_str());
					}
					break;
				}
			}
		}

		if (!is_important) {
			deletable_files.push_back(file);
		}
	}

	// 如果可删除的文件数量不足，就不进行删除
	if ((int)files.size() - (int)deletable_files.size() >= limit) {
		if (console) console->AddLog("[Info] Cannot delete more files; remaining backups are marked as important.");
		return;
	}

	int to_delete_count = (int)files.size() - limit;
	size_t safe_delete_count = static_cast<size_t>(max(0, to_delete_count));
	for (size_t i = 0; i < safe_delete_count && i < deletable_files.size(); ++i) {
		const auto& file_to_delete = deletable_files[i];
		try {
			if (file_to_delete.path().filename().wstring().find(L"[Smart]") == 0)
			{
				if (console) console->AddLog(L("LOG_WARNING_DELETE_SMART_BACKUP"), wstring_to_utf8(files[i].path().filename().wstring()).c_str());
			}

			if (isSafeDelete) {
				// 在 history 中找到这一项并安全删除
				if (history_available) {
					for (const auto& entry : history_it->second) {
						if (entry.worldName == file_to_delete.path().parent_path().filename().wstring() && entry.backupFile == file_to_delete.path().filename().wstring()) {
							DoSafeDeleteBackup(config, entry, configIndex, *console);
							break;
						}
					}
				}
			}
			else {
				bool deletedThroughHistory = false;
				if (history_available && console) {
					for (const auto& entry : history_it->second) {
						if (entry.worldName == file_to_delete.path().parent_path().filename().wstring() && entry.backupFile == file_to_delete.path().filename().wstring()) {
							int mutableConfigIndex = configIndex;
							DoDeleteBackup(config, entry, mutableConfigIndex, *console);
							deletedThroughHistory = true;
							break;
						}
					}
				}

				if (!deletedThroughHistory) {
					fs::remove(file_to_delete);
					InvalidateBackupMetadata(config, file_to_delete.path().parent_path().filename().wstring(), file_to_delete.path().filename().wstring());
					RemoveHistoryEntry(configIndex, file_to_delete.path().filename().wstring());
					SaveHistory();
				}
			}
			if (console) console->AddLog(L("LOG_DELETE_OLD_BACKUP"), wstring_to_utf8(file_to_delete.path().filename().wstring()).c_str());
		}
		catch (const fs::filesystem_error& e) {
			if (console) console->AddLog(L("LOG_ERROR_DELETE_BACKUP"), e.what());
		}
	}
}


// 执行单个世界的备份操作。
// 参数: folder: 世界信息结构体, console: 日志输出对象, comment: 用户注释
void DoBackup(const MyFolder& folder, Console& console, const wstring& comment) {
    const Config& config = folder.config;

	WorldOperationGuard opGuard(filesystem::path(folder.path), FolderState::BACKUP);
	if (!opGuard.Acquired()) {
		console.AddLog(
			L("LOG_OP_REJECTED_BUSY"),
			wstring_to_utf8(folder.name).c_str(),
			L(FolderStateToI18nKey(opGuard.Existing())),
			L(FolderStateToI18nKey(opGuard.Requested()))
		);
		return;
	}

	console.AddLog(L("LOG_BACKUP_START_HEADER"));
	console.AddLog(L("LOG_BACKUP_PREPARE"), wstring_to_utf8(folder.name).c_str());

    if (!filesystem::exists(config.zipPath)) {
        console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
        console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
        return;
    }

	wstring originalSourcePath = folder.path;
	wstring sourcePath = NormalizeSeparators(originalSourcePath);
	const vector<wstring> effectiveBlacklist = BuildEffectiveBackupBlacklist(config.blacklist);
	filesystem::path destinationFolder = JoinPath(config.backupPath, folder.name);
	filesystem::path metadataFolder = JoinPath(config.backupPath, L"_metadata");
	metadataFolder /= folder.name;
    wstring command;
	wstring archivePath;
    wstring archiveNameBase = folder.desc.empty() ? folder.name : folder.desc;

    if (!comment.empty()) {
        archiveNameBase += L" [" + SanitizeFileName(comment) + L"]";
    }

    // 生成带时间戳的文件名
    time_t now = time(0);
    tm ltm;
    localtime_s(&ltm, &now);
    wchar_t timeBuf[160];
    wcsftime(timeBuf, std::size(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);
	archivePath = (destinationFolder / (L"[" + wstring(timeBuf) + L"]" + archiveNameBase + L"." + config.zipFormat)).wstring();

	try {
		filesystem::create_directories(destinationFolder);
		filesystem::create_directories(metadataFolder);
		console.AddLog(L("LOG_BACKUP_DIR_IS"), wstring_to_utf8(destinationFolder.wstring()).c_str());
    } catch (const filesystem::filesystem_error& e) {
        console.AddLog(L("LOG_ERROR_CREATE_BACKUP_DIR"), e.what());
        return;
    }

	// 检测到 level.dat 被锁定，启用热备份握手并依赖 7z -ssw 直接从原世界路径压缩

    if (IsFileLocked(sourcePath + L"/level.dat") || IsFileLocked(sourcePath + L"/session.lock")) {
        // 在热备份前，先检查联动模组是否存在
        bool modAvailable = PerformModHandshake("backup", wstring_to_utf8(folder.name));

        if (modAvailable) {
            console.AddLog(L("KNOTLINK_MOD_DETECTED_BACKUP"),
                g_appState.knotLinkMod.modVersion.c_str());
        } else {
            if (g_appState.knotLinkMod.modDetected.load() && !g_appState.knotLinkMod.versionCompatible.load()) {
                console.AddLog(L("KNOTLINK_MOD_VERSION_TOO_OLD"),
                    g_appState.knotLinkMod.modVersion.c_str(),
                    KnotLinkModInfo::MIN_MOD_VERSION);
            } else {
                console.AddLog(L("KNOTLINK_MOD_NOT_DETECTED_BACKUP"));
            }
        }

        if (modAvailable) {
            // 联动模组存在且版本兼容: 等待模组完成世界保存
            console.AddLog(L("KNOTLINK_WAITING_WORLD_SAVE"));
			std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 握手和下一个广播之间必须有短暂延时
			// 通知模组准备热备份
			BroadcastEvent("event=pre_hot_backup;config=" + to_string(folder.configIndex) +
				";world=" + wstring_to_utf8(folder.name));
			bool saved = g_appState.knotLinkMod.waitForFlag(
				&KnotLinkModInfo::worldSaveComplete,
                std::chrono::milliseconds(10000)); // 最多等待10秒

            if (saved) {
                console.AddLog(L("KNOTLINK_WORLD_SAVE_CONFIRMED"));
            } else {
                // 超时：模组未在规定时间内完成保存，停止
                console.AddLog(L("KNOTLINK_WORLD_SAVE_TIMEOUT"));
				return;
            }
        }
		console.AddLog("[Info] Snapshot copy is disabled. Using 7-Zip -ssw to back up from live world files.");
	}

    bool forceFullBackup = true;
    if (filesystem::exists(destinationFolder)) {
        for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
            if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Full]") != wstring::npos) {
                forceFullBackup = false;
                break;
            }
        }
    }
    if (forceFullBackup)
        console.AddLog(L("LOG_FORCE_FULL_BACKUP"));

    bool forceFullBackupDueToLimit = false;
    if (config.backupMode == 2 && config.maxSmartBackupsPerFull > 0 && !forceFullBackup) {
        vector<filesystem::path> worldBackups;
        try {
            for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
                if (entry.is_regular_file()) {
                    worldBackups.push_back(entry.path());
                }
            }
        } catch (const filesystem::filesystem_error& e) {
            console.AddLog(L("LOG_ERROR_SCAN_BACKUP_DIR"), e.what());
        }

        if (!worldBackups.empty()) {
            sort(worldBackups.begin(), worldBackups.end(), [](const auto& a, const auto& b) {
                return filesystem::last_write_time(a) < filesystem::last_write_time(b);
            });

            int smartCount = 0;
            bool fullFound = false;
            for (auto it = worldBackups.rbegin(); it != worldBackups.rend(); ++it) {
                wstring filename = it->filename().wstring();
                if (filename.find(L"[Full]") != wstring::npos) {
                    fullFound = true;
                    break;
                }
                if (filename.find(L"[Smart]") != wstring::npos) {
                    ++smartCount;
                }
            }

            if (fullFound && smartCount >= config.maxSmartBackupsPerFull) {
                forceFullBackupDueToLimit = true;
                console.AddLog(L("LOG_FORCE_FULL_BACKUP_LIMIT_REACHED"), config.maxSmartBackupsPerFull);
            }
        }
    }

	vector<filesystem::path> candidate_files;
	BackupCheckResult checkResult;
	map<wstring, BackupFileState> currentState;
	BackupChangeSet changeSet;
	candidate_files = GetChangedFiles(sourcePath, metadataFolder, destinationFolder, checkResult, currentState, changeSet);
    if (checkResult == BackupCheckResult::NO_CHANGE && config.skipIfUnchanged) {
        console.AddLog(L("LOG_NO_CHANGE_FOUND"));
        return;
    } else if (checkResult == BackupCheckResult::FORCE_FULL_BACKUP_METADATA_INVALID) {
        console.AddLog(L("LOG_METADATA_INVALID"));
    } else if (checkResult == BackupCheckResult::FORCE_FULL_BACKUP_BASE_MISSING && config.backupMode == 2) {
        console.AddLog(L("LOG_BASE_BACKUP_NOT_FOUND"));
    }

    forceFullBackup = (checkResult == BackupCheckResult::FORCE_FULL_BACKUP_METADATA_INVALID ||
        checkResult == BackupCheckResult::FORCE_FULL_BACKUP_BASE_MISSING ||
        forceFullBackupDueToLimit) || forceFullBackup;

	if (!(config.backupMode == 2 && !forceFullBackup)) {
        try {
            candidate_files.clear();
            for (const auto& entry : filesystem::recursive_directory_iterator(sourcePath)) {
                if (entry.is_regular_file()) {
                    candidate_files.push_back(entry.path());
                }
            }
        } catch (const filesystem::filesystem_error& e) {
            console.AddLog("[Error] Failed to scan source directory %s: %s", wstring_to_utf8(sourcePath).c_str(), e.what());
            return;
        }
    }

    auto is_relative_blacklisted = [&](const wstring& relativePath) {
		filesystem::path absolutePath = filesystem::path(sourcePath) / relativePath;
		return is_blacklisted(absolutePath, sourcePath, originalSourcePath, effectiveBlacklist);
	};

	for (auto it = currentState.begin(); it != currentState.end(); ) {
		if (is_relative_blacklisted(it->first)) {
			it = currentState.erase(it);
		}
		else {
			++it;
		}
	}

	auto filter_relative_changes = [&](vector<wstring>& paths) {
		paths.erase(remove_if(paths.begin(), paths.end(), [&](const wstring& relativePath) {
			return is_relative_blacklisted(relativePath);
		}), paths.end());
	};
	filter_relative_changes(changeSet.addedFiles);
	filter_relative_changes(changeSet.modifiedFiles);
	filter_relative_changes(changeSet.deletedFiles);

    vector<filesystem::path> files_to_backup;
    for (const auto& file : candidate_files) {
		if (!is_blacklisted(file, sourcePath, originalSourcePath, effectiveBlacklist)) {
            files_to_backup.push_back(file);
        }
    }

	if (!forceFullBackup && !changeSet.HasChanges() && (config.skipIfUnchanged || config.backupMode == 2)) {
        console.AddLog(L("LOG_NO_CHANGE_FOUND"));
        return;
    }

	const bool deletionOnlyChange = changeSet.deletedFiles.size() > 0 && files_to_backup.empty();
	if (files_to_backup.empty() && !(config.backupMode == 2 && deletionOnlyChange && !forceFullBackup)) {
		console.AddLog(L("LOG_NO_CHANGE_FOUND"));
		return;
	}

    filesystem::path tempDir = filesystem::temp_directory_path() / L"MineBackup_Filelist";
	wstring filelist_path;
	if (!files_to_backup.empty()) {
		filesystem::create_directories(tempDir);
		filelist_path = (tempDir / (L"_filelist.txt")).wstring();

		ofstream ofs{std::filesystem::path(filelist_path), ios::binary};
		if (ofs.is_open()) {
			for (const auto& file : files_to_backup) {
				string utf8Path = wstring_to_utf8(filesystem::relative(file, sourcePath).wstring());
				ofs.write(utf8Path.data(), static_cast<std::streamsize>(utf8Path.size()));
				ofs.put('\n');
			}
			ofs.close();
#ifdef _WIN32
			{
				HANDLE h = CreateFileW(filelist_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				if (h != INVALID_HANDLE_VALUE) {
					FlushFileBuffers(h);
					CloseHandle(h);
				}
			}
#endif
		} else {
			console.AddLog("[Error] Failed to create temporary file list for 7-Zip.");
			return;
		}
	}

	const int normalizedZipLevel = NormalizeCompressionLevel(config.zipMethod, config.zipLevel);

    wstring backupTypeStr;
    wstring basedOnBackupFile;
    filesystem::path latestBackupPath;

	if (config.backupMode == 1 || forceFullBackup) {
		backupTypeStr = L"Full";
		archivePath = (destinationFolder / (L"[Full][" + wstring(timeBuf) + L"]" + archiveNameBase + L"." + config.zipFormat)).wstring();
		command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod + L" -mx=" + to_wstring(normalizedZipLevel) +
			L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" -ssw \"" + archivePath + L"\"" + L" @" + filelist_path;
		basedOnBackupFile = filesystem::path(archivePath).filename().wstring();
    } else if (config.backupMode == 2) {
        backupTypeStr = L"Smart";

		console.AddLog(L("LOG_BACKUP_SMART_INFO"), files_to_backup.size() + changeSet.deletedFiles.size());

        // 智能备份需要找到它所基于的文件
        // 这可以通过再次读取元数据获得，GetChangedFiles 内部已经验证过它存在
		try {
			BackupMetadataSummary summary;
			if (!LoadBackupMetadataSummary(metadataFolder, summary)) {
				throw runtime_error("Cannot load metadata summary");
			}
			basedOnBackupFile = summary.basedOnFullBackup.empty() ? summary.lastBackupFileName : summary.basedOnFullBackup;
		} catch (const exception& e) {
			console.AddLog("[Warning] Failed to read metadata for smart backup, forcing full backup: %s", e.what());
			// 回退到完整备份
			backupTypeStr = L"Full";
			archivePath = (destinationFolder / (L"[Full][" + wstring(timeBuf) + L"]" + archiveNameBase + L"." + config.zipFormat)).wstring();
			command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod + L" -mx=" + to_wstring(normalizedZipLevel) +
				L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" -ssw \"" + archivePath + L"\"" + L" @" + filelist_path;
			basedOnBackupFile = filesystem::path(archivePath).filename().wstring();
			goto execute_backup;
		}

        // 7z 支持用 @文件名 的方式批量指定要压缩的文件。把所有要备份的文件路径写到一个文本文件避免超过cmd 8191限长
		archivePath = (destinationFolder / (L"[Smart][" + wstring(timeBuf) + L"]" + archiveNameBase + L"." + config.zipFormat)).wstring();

		if (!deletionOnlyChange) {
			command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod + L" -mx=" + to_wstring(normalizedZipLevel) +
				L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" -ssw \"" + archivePath + L"\"" + L" @" + filelist_path;
		}
    } else if (config.backupMode == 3) {
        backupTypeStr = L"Overwrite";
        console.AddLog(L("LOG_OVERWRITE"));
        auto latest_time = filesystem::file_time_type{}; // 默认构造就是最小时间点，不需要::min()
        bool found = false;

		for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
            if (entry.is_regular_file() && entry.path().extension().wstring() == L"." + config.zipFormat) {
                if (entry.last_write_time() > latest_time) {
                    latest_time = entry.last_write_time();
                    latestBackupPath = entry.path();
                    found = true;
                }
            }
        }
        if (found) {
            console.AddLog(L("LOG_FOUND_LATEST"), wstring_to_utf8(latestBackupPath.filename().wstring()).c_str());
			command = L"\"" + config.zipPath + L"\" u -ssw \"" + latestBackupPath.wstring() + L"\" \"" + NormalizeSeparators(sourcePath) + L"/*\" -mx=" + to_wstring(normalizedZipLevel);
            archivePath = latestBackupPath.wstring(); // 记录被更新的文件
        }
        else {
            console.AddLog(L("LOG_NO_BACKUP_FOUND"));
			archivePath = (destinationFolder / (L"[Full][" + wstring(timeBuf) + L"]" + archiveNameBase + L"." + config.zipFormat)).wstring();
			command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod + L" -mx=" + to_wstring(normalizedZipLevel) + L" -m0=" + config.zipMethod +
				L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" -ssw -spf \"" + archivePath + L"\"" + L" \"" + NormalizeSeparators(sourcePath) + L"/*\"";
            // -spf 强制使用完整路径，-spf2 使用相对路径
        }
    }

execute_backup:
    {
        // 在后台线程中执行命令
        bool backupSucceeded = false;
		if (backupTypeStr == L"Smart" && deletionOnlyChange) {
			backupSucceeded = CreateDeletionOnlyArchive(config, archivePath, console);
		}
		else {
			backupSucceeded = RunCommandInBackground(command, console, config.useLowPriority, sourcePath); // 工作目录不能丢！
		}

        if (backupSucceeded)
        {
            console.AddLog(L("LOG_BACKUP_END_HEADER"));

        // 备份文件大小检查 - 根据备份类型调整阈值
        try {
            if (filesystem::exists(archivePath)) {
                uintmax_t fileSize = filesystem::file_size(archivePath);
                // Full备份至少应该有100KB，Smart备份可以很小
                uintmax_t minThreshold = (backupTypeStr == L"Full") ? 102400 : 10240;
                if (fileSize < minThreshold) {
                    console.AddLog(L("BACKUP_FILE_TOO_SMALL_WARNING"), wstring_to_utf8(filesystem::path(archivePath).filename().wstring()).c_str());
                    // 广播一个警告
                    BroadcastEvent("event=backup_warning;type=file_too_small;");
                }
            }
        }
        catch (const filesystem::filesystem_error& e) {
            console.AddLog("[Error] Could not check backup file size: %s", e.what());
        }

		if (folder.configIndex != -1)
			LimitBackupFiles(config, folder.configIndex, destinationFolder.wstring(), config.keepCount, &console);
		else
			LimitBackupFiles(config, g_appState.currentConfigIndex, destinationFolder.wstring(), config.keepCount, &console);

		wstring completedBackupFile = filesystem::path(archivePath).filename().wstring();

        g_appState.realConfigIndex = -1;

        if (config.backupMode == 3) { // 如果是覆写模式，修改一下文件名
            wstring oldName = latestBackupPath.filename().wstring();
            size_t leftBracket = oldName.find(L"["); // 第一个对应Full Smart
            leftBracket = oldName.find(L"[", leftBracket + 1);
            size_t rightBracket = oldName.find(L"]");
            rightBracket = oldName.find(L"]", rightBracket + 1);
            wstring newName = oldName;
            if (leftBracket != wstring::npos && rightBracket != wstring::npos && rightBracket > leftBracket) {
                // 构造新的时间戳
                wchar_t timeBuf[160];
                time_t now = time(0);
                tm ltm;
                localtime_s(&ltm, &now);
                wcsftime(timeBuf, std::size(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);
                // 替换时间戳部分
                newName.replace(leftBracket + 1, rightBracket - leftBracket - 1, timeBuf);
                filesystem::path newPath = latestBackupPath.parent_path() / newName;
                filesystem::rename(latestBackupPath, newPath);
                latestBackupPath = newPath;
				archivePath = latestBackupPath.wstring();
				completedBackupFile = latestBackupPath.filename().wstring();
            }
			if (latestBackupPath.empty()) {
				completedBackupFile = filesystem::path(archivePath).filename().wstring();
			}
			else {
				RemoveHistoryEntry(folder.configIndex, oldName);
				InvalidateBackupMetadata(config, folder.name, oldName, oldName, completedBackupFile);
			}
        }

		UpdateMetadataFile(metadataFolder, completedBackupFile, basedOnBackupFile, backupTypeStr, currentState, changeSet);
		AddHistoryEntry(folder.configIndex, folder.name, completedBackupFile, backupTypeStr, comment, folder.path);

		// 广播一个成功事件
		string payload = "event=backup_success;config=" + to_string(g_appState.currentConfigIndex) + ";world=" + wstring_to_utf8(folder.name) + ";file=" + wstring_to_utf8(completedBackupFile);
		BroadcastEvent(payload);


		// 云存档统一交给 CloudSyncService 处理，避免 UI 和核心逻辑各自拼接 rclone 命令。
		QueueUploadAfterBackup(config, folder.configIndex, folder, completedBackupFile, comment, console);
        }
        else {
            BroadcastEvent("event=backup_failed;config=" + to_string(g_appState.currentConfigIndex) + ";world=" + wstring_to_utf8(folder.name) + ";error=command_failed");
        }
    }
}
void DoOthersBackup(const Config& config, filesystem::path backupWhat, const wstring& comment, Console& console) {
	console.AddLog(L("LOG_BACKUP_OTHERS_START"));

	filesystem::path saveRoot(config.saveRoot);

	filesystem::path othersPath = backupWhat;
	backupWhat = backupWhat.filename().wstring(); // 只保留最后的文件夹名
	const std::wstring backupName = backupWhat.wstring();

	//filesystem::path modsPath = saveRoot.parent_path() / "mods";

	if (!filesystem::exists(othersPath) || !filesystem::is_directory(othersPath)) {
		console.AddLog(L("LOG_ERROR_OTHERS_NOT_FOUND"), wstring_to_utf8(othersPath.wstring()).c_str());
		console.AddLog(L("LOG_BACKUP_OTHERS_END"));
		return;
	}

	filesystem::path destinationFolder;
	wstring archiveNameBase;

	destinationFolder = filesystem::path(config.backupPath) / backupWhat;
	archiveNameBase = backupName;

	if (!comment.empty()) {
		archiveNameBase += L" [" + SanitizeFileName(comment) + L"]";
	}

	// Timestamp
	time_t now = time(0);
	tm ltm;
	localtime_s(&ltm, &now);
	wchar_t timeBuf[160];
	wcsftime(timeBuf, std::size(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);
	wstring archivePath = (destinationFolder / (L"[" + wstring(timeBuf) + L"]" + archiveNameBase + L"." + config.zipFormat)).wstring();

	try {
		filesystem::create_directories(destinationFolder);
		console.AddLog(L("LOG_BACKUP_DIR_IS"), wstring_to_utf8(destinationFolder.wstring()).c_str());
	}
	catch (const filesystem::filesystem_error& e) {
		console.AddLog(L("LOG_ERROR_CREATE_BACKUP_DIR"), e.what());
		console.AddLog(L("LOG_BACKUP_OTHERS_END"));
		return;
	}

	const int normalizedZipLevel = NormalizeCompressionLevel(config.zipMethod, config.zipLevel);
	wstring command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod + L" -mx=" + to_wstring(normalizedZipLevel) +
		L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" -ssw \"" + archivePath + L"\"" + L" \"" + othersPath.wstring() + L"\\*\"";

	if (RunCommandInBackground(command, console, config.useLowPriority)) {
		LimitBackupFiles(config, g_appState.realConfigIndex, destinationFolder.wstring(), config.keepCount, &console);
		// 用特殊名字添加到历史
		AddHistoryEntry(g_appState.currentConfigIndex, backupName, filesystem::path(archivePath).filename().wstring(), backupName, comment, othersPath.wstring());
	}

	console.AddLog(L("LOG_BACKUP_OTHERS_END"));
}

#include "BackupManagerRestore.inl"

static bool DeleteLocalArchiveOnly(const Config& config, const HistoryEntry& entryToDelete, Console& console) {
	filesystem::path pathToDelete = JoinPath(config.backupPath, entryToDelete.worldName) / entryToDelete.backupFile;
	try {
		if (filesystem::exists(pathToDelete)) {
			filesystem::remove(pathToDelete);
			console.AddLog("  - %s OK", wstring_to_utf8(pathToDelete.filename().wstring()).c_str());
			return true;
		}
		console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(entryToDelete.backupFile).c_str());
		return false;
	}
	catch (const filesystem::filesystem_error& e) {
		console.AddLog(L("LOG_ERROR_DELETE_BACKUP"), wstring_to_utf8(pathToDelete.filename().wstring()).c_str(), e.what());
		return false;
	}
}

void DeleteBackupWithMode(const Config& config, const HistoryEntry& entryToDelete, int configIndex, BackupDeleteMode mode, bool useSafeDelete, Console& console) {
	if (mode == BackupDeleteMode::HistoryOnly) {
		// 仅删除历史：保留本地文件，常用于清理误导入或不再需要展示的云历史。
		RemoveHistoryEntry(configIndex, entryToDelete.worldName, entryToDelete.backupFile);
		SaveHistory();
		QueueConfigurationHistorySyncAfterLocalChange(config, configIndex, "history deletion", console);
		return;
	}

	if (mode == BackupDeleteMode::LocalArchiveOnly) {
		DeleteLocalArchiveOnly(config, entryToDelete, console);
		return;
	}

	if (useSafeDelete && entryToDelete.backupType.find(L"Smart") != wstring::npos) {
		DoSafeDeleteBackup(config, entryToDelete, configIndex, console);
	}
	else {
		int mutableConfigIndex = configIndex;
		DoDeleteBackup(config, entryToDelete, mutableConfigIndex, console);
	}
}

void DoDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, int& configIndex, Console& console) {
	console.AddLog(L("LOG_PRE_TO_DELETE"), wstring_to_utf8(entryToDelete.backupFile).c_str());

	filesystem::path backupDir = JoinPath(config.backupPath, entryToDelete.worldName);
	vector<filesystem::path> filesToDelete;
	filesToDelete.push_back(backupDir / entryToDelete.backupFile);

	// 执行删除操作
	for (const auto& path : filesToDelete) {
		try {
			if (filesystem::exists(path)) {
				filesystem::remove(path);
				console.AddLog("  - %s OK", wstring_to_utf8(path.filename().wstring()).c_str());
				InvalidateBackupMetadata(config, entryToDelete.worldName, path.filename().wstring());
				// 从历史记录中移除对应条目
				RemoveHistoryEntry(configIndex, entryToDelete.worldName, path.filename().wstring());
			}
			else {
				console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(entryToDelete.backupFile).c_str());
				InvalidateBackupMetadata(config, entryToDelete.worldName, path.filename().wstring());
				RemoveHistoryEntry(configIndex, entryToDelete.worldName, path.filename().wstring());
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog(L("LOG_ERROR_DELETE_BACKUP"), wstring_to_utf8(path.filename().wstring()).c_str(), e.what());
		}
	}
	SaveHistory(); // 保存历史记录的更改
	QueueConfigurationHistorySyncAfterLocalChange(config, configIndex, "backup deletion", console);
}

void DoSafeDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, int configIndex, Console& console) {
	console.AddLog(L("LOG_SAFE_DELETE_START"), wstring_to_utf8(entryToDelete.backupFile).c_str());

	if (entryToDelete.isImportant) {
		console.AddLog(L("LOG_SAFE_DELETE_ABORT_IMPORTANT"), wstring_to_utf8(entryToDelete.backupFile).c_str());
		return;
	}

	filesystem::path backupDir = JoinPath(config.backupPath, entryToDelete.worldName);
	filesystem::path pathToDelete = backupDir / entryToDelete.backupFile;
	const HistoryEntry* nextEntryRaw = nullptr;

	// Create a sorted list of history entries for this world to reliably find the next one
	vector<const HistoryEntry*> worldHistory;
	for (const auto& entry : g_appState.g_history[configIndex]) {
		if (entry.worldName == entryToDelete.worldName) {
			worldHistory.push_back(&entry);
		}
	}
	sort(worldHistory.begin(), worldHistory.end(), [](const auto* a, const auto* b) {
		return a->timestamp_str < b->timestamp_str;
		});

	for (size_t i = 0; i < worldHistory.size(); ++i) {
		if (worldHistory[i]->backupFile == entryToDelete.backupFile) {
			if (i + 1 < worldHistory.size()) {
				nextEntryRaw = worldHistory[i + 1];
			}
			break;
		}
	}

	if (!nextEntryRaw || nextEntryRaw->backupType == L"Full") {
		console.AddLog(L("LOG_SAFE_DELETE_END_OF_CHAIN"));
		DoDeleteBackup(config, entryToDelete, configIndex, console);
		return;
	}

	if (nextEntryRaw->isImportant) {
		console.AddLog(L("LOG_SAFE_DELETE_ABORT_IMPORTANT_TARGET"), wstring_to_utf8(nextEntryRaw->backupFile).c_str());
		return;
	}

	const HistoryEntry nextEntry = *nextEntryRaw;
	filesystem::path pathToMergeInto = backupDir / nextEntry.backupFile;
	console.AddLog(L("LOG_SAFE_DELETE_MERGE_INFO"), wstring_to_utf8(entryToDelete.backupFile).c_str(), wstring_to_utf8(nextEntry.backupFile).c_str());

	if (!filesystem::exists(pathToDelete)) {
		console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(entryToDelete.backupFile).c_str());
		DoDeleteBackup(config, entryToDelete, configIndex, console);
		return;
	}

	if (!filesystem::exists(pathToMergeInto)) {
		console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(nextEntry.backupFile).c_str());
		DoDeleteBackup(config, entryToDelete, configIndex, console);
		return;
	}

	const auto original_mod_time = filesystem::last_write_time(pathToMergeInto);
	const int normalizedZipLevel = NormalizeCompressionLevel(config.zipMethod, config.zipLevel);

	filesystem::path tempRootBase;
	if (config.snapshotPath.size() >= 2 && filesystem::exists(config.snapshotPath)) {
		tempRootBase = filesystem::path(NormalizeSeparators(config.snapshotPath));
	}
	else {
		tempRootBase = filesystem::temp_directory_path();
	}

	wstringstream suffixBuilder;
	suffixBuilder << chrono::steady_clock::now().time_since_epoch().count();
	const filesystem::path tempRoot = tempRootBase / (L"MineBackup_Merge_" + suffixBuilder.str());
	const filesystem::path mergeWorkspace = tempRoot / L"merge_workspace";
	const filesystem::path rebuiltArchive = tempRoot / (L"rebuilt." + config.zipFormat);
	const filesystem::path originalTargetBackup = tempRoot / (L"target_backup." + config.zipFormat);

	bool replacedTargetArchive = false;
	filesystem::path finalArchivePath = pathToMergeInto;
	wstring finalBackupType = nextEntry.backupType;
	wstring finalBackupFile = nextEntry.backupFile;

	try {
		filesystem::create_directories(mergeWorkspace);
		filesystem::copy_file(pathToMergeInto, originalTargetBackup, filesystem::copy_options::overwrite_existing);

		console.AddLog(L("LOG_SAFE_DELETE_STEP_1"));
		wstring cmdExtractDeleted = L"\"" + config.zipPath + L"\" x \"" + pathToDelete.wstring() + L"\" -o\"" + mergeWorkspace.wstring() + L"\" -y";
		if (!RunCommandInBackground(cmdExtractDeleted, console, config.useLowPriority)) {
			throw runtime_error("Failed to extract deleted archive.");
		}

		console.AddLog(L("LOG_SAFE_DELETE_STEP_2"));
		wstring cmdExtractNext = L"\"" + config.zipPath + L"\" x \"" + pathToMergeInto.wstring() + L"\" -o\"" + mergeWorkspace.wstring() + L"\" -y";
		if (!RunCommandInBackground(cmdExtractNext, console, config.useLowPriority)) {
			throw runtime_error("Failed to extract target archive.");
		}

		error_code markerEc;
		filesystem::path markerDir = mergeWorkspace / kDeletedOnlyMarkerDir;
		if (filesystem::exists(markerDir, markerEc) && !markerEc) {
			filesystem::remove_all(markerDir, markerEc);
		}

		wstring cmdRebuild = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -m0=" + config.zipMethod +
			L" -mx=" + to_wstring(normalizedZipLevel) +
			L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) +
			L" -ssw \"" + rebuiltArchive.wstring() + L"\" \"*\"";
		if (!RunCommandInBackground(cmdRebuild, console, config.useLowPriority, mergeWorkspace.wstring())) {
			throw runtime_error("Failed to rebuild merged archive.");
		}

		error_code ec;
		if (filesystem::exists(pathToMergeInto, ec) && !ec) {
			filesystem::remove(pathToMergeInto, ec);
			if (ec) {
				throw runtime_error("Failed to replace original target archive.");
			}
		}

		filesystem::rename(rebuiltArchive, pathToMergeInto, ec);
		if (ec) {
			ec.clear();
			filesystem::copy_file(rebuiltArchive, pathToMergeInto, filesystem::copy_options::overwrite_existing, ec);
			if (ec) {
				throw runtime_error("Failed to deploy rebuilt target archive.");
			}
			error_code cleanupEc;
			filesystem::remove(rebuiltArchive, cleanupEc);
		}
		replacedTargetArchive = true;

		if (entryToDelete.backupType == L"Full") {
			console.AddLog(L("LOG_SAFE_DELETE_STEP_3"));
			finalBackupType = L"Full";

			wstring newFilename = nextEntry.backupFile;
			size_t pos = newFilename.find(L"[Smart]");
			if (pos != wstring::npos) {
				newFilename.replace(pos, 7, L"[Full]");
				finalBackupFile = newFilename;
				filesystem::path newPath = backupDir / newFilename;
				if (newPath != pathToMergeInto && filesystem::exists(newPath)) {
					throw runtime_error("Cannot rename merged archive because destination filename already exists.");
				}
				filesystem::rename(pathToMergeInto, newPath);
				finalArchivePath = newPath;
				console.AddLog(L("LOG_SAFE_DELETE_RENAMED"), wstring_to_utf8(newFilename).c_str());
			}
		}
		else {
			console.AddLog(L("LOG_SAFE_DELETE_STEP_3_SKIP"));
		}

		error_code timeEc;
		filesystem::last_write_time(finalArchivePath, original_mod_time, timeEc);
		if (timeEc) {
			console.AddLog("[Warning] Failed to preserve merged archive timestamp: %s", timeEc.message().c_str());
		}

		console.AddLog(L("LOG_SAFE_DELETE_STEP_4"));
		filesystem::remove(pathToDelete);
		RemoveHistoryEntry(configIndex, entryToDelete.worldName, entryToDelete.backupFile);

		for (auto& entry : g_appState.g_history[configIndex]) {
			if (entry.worldName == nextEntry.worldName && entry.backupFile == nextEntry.backupFile) {
				entry.backupFile = finalBackupFile;
				entry.backupType = finalBackupType;
				break;
			}
		}

		SaveHistory();
		QueueConfigurationHistorySyncAfterLocalChange(config, configIndex, "safe delete", console);

		string metadataError;
		if (!TryRepairMetadataAfterSafeDelete(
			config,
			entryToDelete.worldName,
			entryToDelete.backupFile,
			nextEntry.backupFile,
			finalBackupFile,
			finalBackupType,
			metadataError
		)) {
			console.AddLog("[Warning] Failed to repair metadata after safe-delete (%s). Falling back to metadata invalidation.", metadataError.c_str());
			InvalidateBackupMetadata(config, entryToDelete.worldName, entryToDelete.backupFile, nextEntry.backupFile, finalBackupFile);
		}

		error_code cleanupEc;
		filesystem::remove_all(tempRoot, cleanupEc);
		console.AddLog(L("LOG_SAFE_DELETE_SUCCESS"));
	}
	catch (const exception& e) {
		if (replacedTargetArchive) {
			error_code restoreEc;
			if (filesystem::exists(finalArchivePath, restoreEc) && !restoreEc) {
				filesystem::remove(finalArchivePath, restoreEc);
			}
			if (finalArchivePath != pathToMergeInto) {
				error_code extraRemoveEc;
				if (filesystem::exists(pathToMergeInto, extraRemoveEc) && !extraRemoveEc) {
					filesystem::remove(pathToMergeInto, extraRemoveEc);
				}
			}

			restoreEc.clear();
			filesystem::copy_file(originalTargetBackup, pathToMergeInto, filesystem::copy_options::overwrite_existing, restoreEc);
			if (restoreEc) {
				console.AddLog("[Warning] Failed to restore original archive after safe-delete failure: %s", restoreEc.message().c_str());
			}
			else {
				error_code timeEc;
				filesystem::last_write_time(pathToMergeInto, original_mod_time, timeEc);
			}
		}

		console.AddLog(L("LOG_SAFE_DELETE_FATAL_ERROR"), e.what());
		error_code ec;
		filesystem::remove_all(tempRoot, ec);
	}
}

// 避免仅以 worldIdx 作为 key 导致的冲突，使用{ configIdx, worldIdx }
void AutoBackupThreadFunction(int configIdx, int worldIdx, int intervalMinutes, Console* console, atomic<bool>& stop_flag) {
	auto key = make_pair(configIdx, worldIdx);
	console->AddLog(L("LOG_AUTOBACKUP_START"), worldIdx, intervalMinutes);

	while (true) {
		// 等待指定的时间，但每秒检查一次是否需要停止
		for (int i = 0; i < intervalMinutes * 60; ++i) {
			if (stop_flag) { // 或者 stop_flag.load()
				console->AddLog(L("LOG_AUTOBACKUP_STOPPED"), worldIdx);
				return; // 线程安全地退出
			}
			this_thread::sleep_for(chrono::seconds(1));
		}

		// 如果在长时间的等待后，发现需要停止，则不执行备份直接退出
		if (stop_flag) {
			console->AddLog(L("LOG_AUTOBACKUP_STOPPED"), worldIdx);
			return;
		}

		// 时间到了，开始备份
		console->AddLog(L("LOG_AUTOBACKUP_ROUTINE"), worldIdx);
		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			if (g_appState.configs.count(configIdx) && worldIdx >= 0 && worldIdx < g_appState.configs[configIdx].worlds.size()) {
				MyFolder folder = {
					JoinPath(g_appState.configs[configIdx].saveRoot, g_appState.configs[configIdx].worlds[worldIdx].first).wstring(),
					g_appState.configs[configIdx].worlds[worldIdx].first,
					g_appState.configs[configIdx].worlds[worldIdx].second,
					g_appState.configs[configIdx],
					configIdx,
					worldIdx
				};
				DoBackup(folder, *console);
			}
			else {
				console->AddLog(L("ERROR_INVALID_WORLD_IN_TASK"), configIdx, worldIdx);
				// 任务无效，退出或移除
				lock_guard<mutex> lock2(g_appState.task_mutex);
				if (g_appState.g_active_auto_backups.count(key)) {
					g_appState.g_active_auto_backups.erase(key);
				}
				return;
			}
		}
	}
}

void DoExportForSharing(Config tempConfig, wstring worldName, wstring worldPath, wstring outputPath, wstring description, Console& console) {
	console.AddLog(L("LOG_EXPORT_STARTED"), wstring_to_utf8(worldName).c_str());

	// 准备临时文件和路径
	filesystem::path temp_export_dir = filesystem::temp_directory_path() / L"MineBackup_Export" / worldName;
	filesystem::path readme_path = temp_export_dir / L"readme.txt";

	try {
		// 清理并创建临时工作目录
		if (filesystem::exists(temp_export_dir)) {
			filesystem::remove_all(temp_export_dir);
		}
		filesystem::create_directories(temp_export_dir);

		// 如果有描述，创建 readme.txt
		if (!description.empty()) {
			ofstream readme_file(readme_path, ios::binary);
			if (readme_file.is_open()) {
				auto write_line = [&readme_file](const wstring& line) {
					string utf8 = wstring_to_utf8(line);
					readme_file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
					readme_file.put('\n');
				};

				write_line(L"[Name]");
				write_line(worldName);
				readme_file.put('\n');
				write_line(L"[Description]");
				write_line(description);
				readme_file.put('\n');
				write_line(L"[Exported by MineBackup]");
			}
		}

		// 收集并过滤文件
		vector<filesystem::path> files_to_export;
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (!is_blacklisted(entry.path(), worldPath, worldPath, tempConfig.blacklist)) {
				files_to_export.push_back(entry.path());
			}
		}

		// 将 readme.txt 也加入待压缩列表
		if (!description.empty()) {
			files_to_export.push_back(readme_path);
		}

		if (files_to_export.empty()) {
			console.AddLog("[Error] No files left to export after applying blacklist.");
			filesystem::remove_all(temp_export_dir);
			return;
		}

		// 创建文件列表供 7z 使用
		wstring filelist_path = (temp_export_dir / L"filelist.txt").wstring();
		ofstream ofs{std::filesystem::path(filelist_path), ios::binary};
		for (const auto& file : files_to_export) {
			string utf8Path;
			if (file.wstring().rfind(worldPath, 0) == 0) {
				utf8Path = wstring_to_utf8(filesystem::relative(file, worldPath).wstring());
			}
			else {
				utf8Path = wstring_to_utf8(file.wstring());
			}
			ofs.write(utf8Path.data(), static_cast<std::streamsize>(utf8Path.size()));
			ofs.put('\n');
		}
		ofs.close();

		// 构建并执行 7z 命令
		const int normalizedZipLevel = NormalizeCompressionLevel(tempConfig.zipMethod, tempConfig.zipLevel);
		wstring command = L"\"" + tempConfig.zipPath + L"\" a -t" + tempConfig.zipFormat + L" -m0=" + tempConfig.zipMethod + L" -mx=" + to_wstring(normalizedZipLevel) +
			L" -ssw \"" + outputPath + L"\"" + L" @" + filelist_path;

		// 工作目录应为原始世界路径，以确保压缩包内路径正确
		if (RunCommandInBackground(command, console, tempConfig.useLowPriority, worldPath)) {
			console.AddLog(L("LOG_EXPORT_SUCCESS"), wstring_to_utf8(outputPath).c_str());
			wstring cmd = L"/select,\"" + outputPath + L"\"";
			OpenFolderWithFocus(filesystem::path(outputPath).parent_path().wstring(), cmd);
		}
		else {
			console.AddLog(L("LOG_EXPORT_FAILED"));
		}

	}
	catch (const exception& e) {
		console.AddLog("[Error] An exception occurred during export: %s", e.what());
	}

	// 清理临时目录
	filesystem::remove_all(temp_export_dir);
}


MyFolder GetOccupiedWorld();

void DoHotRestore(const MyFolder& world, Console& console, bool deleteBackup, const std::wstring& backupFile) {
	
	Config cfg = world.config;
	auto& mod = g_appState.knotLinkMod;

	// 确认联动模组状态（在 TriggerHotkeyRestore 中已完成握手） ===
	// 到达这里说明模组已检测到且版本兼容
	console.AddLog(L("KNOTLINK_HOT_RESTORE_START"), wstring_to_utf8(world.name).c_str());

	// 发送预还原消息，请求模组保存世界并退出
	mod.resetForOperation();
	BroadcastEvent("event=pre_hot_restore;config=" + to_string(world.configIndex) +
		";world=" + wstring_to_utf8(world.name));
	console.AddLog(L("KNOTLINK_WAITING_WORLD_SAVE_EXIT"));

	// 等待模组通知世界保存并退出完成
	// 使用 condition_variable 高效等待，取代旧的轮询方式
	bool exitComplete = mod.waitForFlag(
		&KnotLinkModInfo::worldSaveAndExitComplete,
		std::chrono::milliseconds(10000)); // 最多等待10秒
	if (!exitComplete) {
		console.AddLog(L("KNOTLINK_HOT_RESTORE_TIMEOUT"));
		BroadcastEvent("event=restore_cancelled;reason=timeout;world=" + wstring_to_utf8(world.name));
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		g_appState.isRespond = false;
		return;
	}

	console.AddLog(L("KNOTLINK_MOD_EXIT_CONFIRMED"));

	// 等待文件系统确认世界不再被占用
	{
		auto startTime = chrono::steady_clock::now();
		auto checkTimeout = chrono::seconds(15);
		bool worldReleased = false;

		while (chrono::steady_clock::now() - startTime < checkTimeout) {
			MyFolder checkOccupied = GetOccupiedWorld();
			if (checkOccupied.name != world.name) {
				worldReleased = true;
				break;
			}
			this_thread::sleep_for(chrono::milliseconds(500));
		}

		if (!worldReleased) {
			console.AddLog(L("KNOTLINK_HOT_RESTORE_WORLD_OCCUPIED"));
			BroadcastEvent("event=restore_cancelled;reason=world_occupied;world=" + wstring_to_utf8(world.name));
			g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
			g_appState.isRespond = false;
			return;
		}
	}

	// 等待文件锁释放
	{
		filesystem::path levelDatPath = filesystem::path(world.path) / L"level.dat";
		auto waitStart = chrono::steady_clock::now();
		auto waitTimeout = chrono::seconds(10);
		while (chrono::steady_clock::now() - waitStart < waitTimeout) {
			if (!IsFileLocked(levelDatPath.wstring())) {
				break;
			}
			this_thread::sleep_for(chrono::milliseconds(200));
		}
		// 额外等待确保文件系统同步完成
		this_thread::sleep_for(chrono::milliseconds(500));
	}

	g_appState.hotkeyRestoreState = HotRestoreState::RESTORING;
	console.AddLog(L("KNOTLINK_HOT_RESTORE_PROCEEDING"));

	// 查找目标备份文件（指定文件名或最新备份）
	filesystem::path backupDir = JoinPath(cfg.backupPath, world.name);
	filesystem::path targetBackup;
	bool found = false;

	if (!backupFile.empty()) {
		targetBackup = backupDir / backupFile;
		found = filesystem::exists(targetBackup) && filesystem::is_regular_file(targetBackup);
	}
	else {
		filesystem::path latestBackup;
	auto latest_time = filesystem::file_time_type{};

		if (filesystem::exists(backupDir)) {
			for (const auto& entry : filesystem::directory_iterator(backupDir)) {
				if (entry.is_regular_file()) {
					if (entry.last_write_time() > latest_time) {
						latest_time = entry.last_write_time();
						latestBackup = entry.path();
						found = true;
					}
				}
			}
		}

		if (found) {
			targetBackup = latestBackup;
		}
	}

	if (!found) {
		console.AddLog(L("LOG_NO_BACKUP_FOUND"));
		BroadcastEvent("event=restore_finished;status=failure;reason=no_backup_found;world=" + wstring_to_utf8(world.name));
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		g_appState.isRespond = false;
		return;
	}

	console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(targetBackup.filename().wstring()).c_str());

	const bool restoreSucceeded = DoRestore(cfg, world.name, targetBackup.filename().wstring(), ref(console), 0, "");
	if (!restoreSucceeded) {
		BroadcastEvent("event=restore_finished;status=failure;reason=restore_failed;world=" + wstring_to_utf8(world.name));
		g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
		g_appState.isRespond = false;
		return;
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	BroadcastEvent("event=restore_finished;status=success;config=" + to_string(world.configIndex) +
		";world=" + wstring_to_utf8(world.name));
	console.AddLog(L("KNOTLINK_HOT_RESTORE_DONE"));

	std::this_thread::sleep_for(std::chrono::milliseconds(3000));

	// 发送重进世界的指令
	BroadcastEvent("event=rejoin_world;world=" + wstring_to_utf8(world.name));
	console.AddLog(L("KNOTLINK_REJOIN_SENT"));

	// 等待重进世界结果
	bool rejoinReceived = mod.waitForFlag(
		&KnotLinkModInfo::rejoinResponseReceived,
		std::chrono::milliseconds(30000)); // 最多等待30秒

	if (rejoinReceived) {
		bool success = false;
		{
			std::lock_guard<std::mutex> lock(mod.mtx);
			success = mod.rejoinSuccess;
		}
		if (success) {
			console.AddLog(L("KNOTLINK_REJOIN_OK"));
			BroadcastEvent("event=hot_restore_complete;status=full_success;world=" + wstring_to_utf8(world.name));
		}
		else {
			console.AddLog(L("KNOTLINK_REJOIN_FAIL"));
			BroadcastEvent("event=hot_restore_complete;status=restore_ok_rejoin_failed;world=" + wstring_to_utf8(world.name));
		}
	}
	else {
		// 重进世界超时 — 还原已成功但重进结果未知
		console.AddLog(L("KNOTLINK_REJOIN_TIMEOUT"));
		BroadcastEvent("event=hot_restore_complete;status=restore_ok_rejoin_timeout;world=" + wstring_to_utf8(world.name));
	}

	// 重置状态
	g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
	g_appState.isRespond = false;
}
