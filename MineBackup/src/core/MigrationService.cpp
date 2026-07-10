#include "MigrationService.h"

#include "ConfigManager.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "FolderRewindMetadataStore.h"
#include "LegacyMineBackup15Reader.h"
#include "PlatformCompat.h"
#ifdef _WIN32
#include "Platform_win.h"
#elif defined(__APPLE__)
#include "Platform_macos.h"
#else
#include "Platform_linux.h"
#endif
#include "imgui-all.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;

namespace MigrationService {
namespace {

mutex g_mutex;
recursive_mutex g_executionMutex;
MigrationReport g_report;
bool g_showSummary = false;
bool g_historyPersistenceBlocked = false;

struct Sha1 {
	array<uint32_t, 5> h{ 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
	vector<uint8_t> bytes;

	static uint32_t Rol(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }
	array<uint8_t, 20> Finish() {
		uint64_t bitLength = static_cast<uint64_t>(bytes.size()) * 8;
		bytes.push_back(0x80);
		while ((bytes.size() % 64) != 56) bytes.push_back(0);
		for (int i = 7; i >= 0; --i) bytes.push_back(static_cast<uint8_t>(bitLength >> (i * 8)));
		for (size_t offset = 0; offset < bytes.size(); offset += 64) {
			uint32_t w[80]{};
			for (int i = 0; i < 16; ++i) {
				const size_t p = offset + static_cast<size_t>(i) * 4;
				w[i] = (static_cast<uint32_t>(bytes[p]) << 24) | (static_cast<uint32_t>(bytes[p + 1]) << 16)
					| (static_cast<uint32_t>(bytes[p + 2]) << 8) | bytes[p + 3];
			}
			for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
			uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
			for (int i = 0; i < 80; ++i) {
				uint32_t f = 0, k = 0;
				if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
				else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
				else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
				else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
				uint32_t temp = Rol(a, 5) + f + e + k + w[i];
				e = d; d = c; c = Rol(b, 30); b = a; a = temp;
			}
			h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
		}
		array<uint8_t, 20> out{};
		for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - j * 8));
		return out;
	}
};

wstring LowerNormalized(wstring value) {
	replace(value.begin(), value.end(), L'\\', L'/');
	while (!value.empty() && value.back() == L'/') value.pop_back();
	transform(value.begin(), value.end(), value.begin(), ::towlower);
	return value;
}

wstring UuidV5(const string& name) {
	// Fixed MineBackup migration namespace: b87cf833-c18f-5c85-8bb5-68a723455b1f.
	const uint8_t ns[16] = { 0xb8,0x7c,0xf8,0x33,0xc1,0x8f,0x5c,0x85,0x8b,0xb5,0x68,0xa7,0x23,0x45,0x5b,0x1f };
	Sha1 sha;
	sha.bytes.insert(sha.bytes.end(), begin(ns), end(ns));
	sha.bytes.insert(sha.bytes.end(), name.begin(), name.end());
	auto digest = sha.Finish();
	digest[6] = static_cast<uint8_t>((digest[6] & 0x0f) | 0x50);
	digest[8] = static_cast<uint8_t>((digest[8] & 0x3f) | 0x80);
	wstringstream out;
	out << hex << setfill(L'0');
	for (int i = 0; i < 16; ++i) {
		if (i == 4 || i == 6 || i == 8 || i == 10) out << L'-';
		out << setw(2) << static_cast<unsigned>(digest[i]);
	}
	return out.str();
}

wstring SafeStamp() {
	wstring value = FolderRewindFormat::MakeUtcTimestampString();
	for (wchar_t& ch : value) if (ch == L':' || ch == L'T' || ch == L'Z') ch = L'-';
	return value;
}

filesystem::path SnapshotPath(const filesystem::path& source, const wstring& label) {
	return source.parent_path() / (source.stem().wstring() + L".v1.15." + SafeStamp() + L"." + label + source.extension().wstring());
}

bool CopySnapshot(const filesystem::path& source, filesystem::path& snapshot) {
	error_code ec;
	if (!filesystem::exists(source, ec) || ec) return true;
	snapshot = SnapshotPath(source, L"bak");
	filesystem::copy_file(source, snapshot, filesystem::copy_options::overwrite_existing, ec);
	return !ec;
}

void PreserveInvalidFile(const filesystem::path& source) {
	error_code ec;
	if (!filesystem::exists(source, ec) || ec) return;
	filesystem::copy_file(source, source.wstring() + L".invalid." + SafeStamp(), filesystem::copy_options::overwrite_existing, ec);
}

bool CreateWorldMetadataSnapshot(
	const Config& config,
	const FolderRewindFormat::StoragePaths& paths,
	filesystem::path& snapshotRoot,
	wstring& error) {
	error.clear();
	snapshotRoot = filesystem::path(config.backupPath) / L"_migration-backups" / L"1.15" / SafeStamp()
		/ FolderRewindFormat::kMetadataRootDirName / paths.folderName;
	error_code ec;
	filesystem::create_directories(snapshotRoot, ec);
	if (ec) { error = L"Could not create the world metadata recovery snapshot directory."; return false; }

	int copied = 0;
	for (const auto& entry : filesystem::directory_iterator(paths.metadataDir, ec)) {
		if (ec) { error = L"Could not enumerate legacy metadata for recovery snapshot."; return false; }
		if (!entry.is_regular_file(ec) || ec || entry.path().extension() != L".json") { ec.clear(); continue; }
		const wstring fileName = entry.path().filename().wstring();
		// state.json belongs to the new format. Legacy metadata.json and every adjacent record are preserved.
		if (_wcsicmp(fileName.c_str(), FolderRewindFormat::kMetadataStateFileName) == 0) continue;
		filesystem::copy_file(entry.path(), snapshotRoot / entry.path().filename(), filesystem::copy_options::overwrite_existing, ec);
		if (ec) { error = L"Could not copy a legacy metadata file into the recovery snapshot."; return false; }
		++copied;
	}
	if (copied == 0 || !filesystem::exists(snapshotRoot / L"metadata.json")) {
		error = L"The world metadata recovery snapshot is incomplete.";
		return false;
	}
	return true;
}

bool ReplaceFileAtomically(const filesystem::path& temp, const filesystem::path& target) {
#ifdef _WIN32
	SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
	return MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
	error_code ec;
	filesystem::rename(temp, target, ec);
	return !ec;
#endif
}

void AddOrReplaceUnit(const MigrationUnitResult& unit) {
	lock_guard<mutex> lock(g_mutex);
	MigrationUnitResult mergedUnit = unit;
	auto it = find_if(g_report.units.begin(), g_report.units.end(), [&](const MigrationUnitResult& value) { return value.unitId == unit.unitId; });
	if (it == g_report.units.end()) g_report.units.push_back(mergedUnit);
	else {
		if (mergedUnit.snapshotPath.empty()) mergedUnit.snapshotPath = it->snapshotPath;
		*it = std::move(mergedUnit);
	}
	g_report.updatedAtUtc = FolderRewindFormat::MakeUtcTimestampString();
	g_report.status = MigrationStatus::NotNeeded;
	for (const auto& item : g_report.units)
		g_report.status = HigherPriorityStatus(g_report.status, item.status);
	try {
		filesystem::create_directories(L".migration");
		nlohmann::json root;
		root["Version"] = "1.15-to-1.16";
		root["UpdatedAtUtc"] = wstring_to_utf8(g_report.updatedAtUtc);
		root["Status"] = static_cast<int>(g_report.status);
		root["Units"] = nlohmann::json::array();
		for (const auto& item : g_report.units) {
			nlohmann::json value;
			value["UnitId"] = wstring_to_utf8(item.unitId);
			value["Status"] = static_cast<int>(item.status);
			value["Message"] = wstring_to_utf8(item.message);
			value["SnapshotPath"] = wstring_to_utf8(item.snapshotPath);
			value["MigratedItems"] = item.migratedItems;
			value["SkippedItems"] = item.skippedItems;
			root["Units"].push_back(std::move(value));
		}
		const filesystem::path target = filesystem::path(L".migration") / L"1.15-to-1.16.json";
		const filesystem::path temp = target.wstring() + L".tmp";
		ofstream out(temp, ios::binary | ios::trunc);
		out << root.dump(2); out.close();
		ReplaceFileAtomically(temp, target);
	}
	catch (...) {}
}

bool SameHistoryIdentity(const HistoryEntry& a, const HistoryEntry& b) {
	return _wcsicmp(a.configId.c_str(), b.configId.c_str()) == 0
		&& a.worldPath == b.worldPath && a.worldName == b.worldName
		&& a.backupFile == b.backupFile && a.timestamp_str == b.timestamp_str;
}

void MergeHistory(map<int, vector<HistoryEntry>>& history) {
	for (auto& [configIndex, entries] : history) {
		vector<HistoryEntry> merged;
		for (const auto& entry : entries) {
			auto it = find_if(merged.begin(), merged.end(), [&](const HistoryEntry& current) { return SameHistoryIdentity(current, entry); });
			if (it == merged.end()) { merged.push_back(entry); continue; }
			// The first valid new item wins; legacy data only fills empty fields.
			if (it->worldPath.empty()) it->worldPath = entry.worldPath;
			if (it->backupType.empty()) it->backupType = entry.backupType;
			if (it->comment.empty()) it->comment = entry.comment;
			if (it->cloudArchiveRemotePath.empty()) it->cloudArchiveRemotePath = entry.cloudArchiveRemotePath;
			if (it->cloudMetadataRecordRemotePath.empty()) it->cloudMetadataRecordRemotePath = entry.cloudMetadataRecordRemotePath;
			if (it->cloudMetadataStateRemotePath.empty()) it->cloudMetadataStateRemotePath = entry.cloudMetadataStateRemotePath;
		}
		entries = std::move(merged);
	}
}

MigrationUnitResult MigrateHistory() {
	MigrationUnitResult unit;
	unit.unitId = L"startup:history";
	const filesystem::path path = L"history.json";
	if (!filesystem::exists(path) || !LegacyMineBackup15Reader::IsLegacyHistoryFile(path)) {
		unit.status = MigrationStatus::NotNeeded;
		unit.message = L"History already uses the FolderRewind schema.";
		return unit;
	}
	LegacyMineBackup15Reader::HistoryReadResult read;
	if (!LegacyMineBackup15Reader::ReadHistory(path, g_appState.configs, read)) {
		unit.status = MigrationStatus::Failed; unit.message = L"Could not parse the 1.15 history file.";
		g_historyPersistenceBlocked = true; return unit;
	}
	size_t migratedCount = 0;
	for (const auto& [configIndex, entries] : read.history) migratedCount += entries.size();
	if (read.sourceItems > 0 && migratedCount == 0) {
		unit.status = MigrationStatus::Failed;
		unit.message = L"No 1.15 history entries could be mapped safely; the original file was not replaced.";
		g_historyPersistenceBlocked = true;
		return unit;
	}
	MergeHistory(read.history);
	filesystem::path snapshot;
	if (!CopySnapshot(path, snapshot)) {
		unit.status = MigrationStatus::Failed; unit.message = L"Could not create the history recovery snapshot.";
		g_historyPersistenceBlocked = true; return unit;
	}
	unit.snapshotPath = snapshot.wstring();
	if (!read.unmigrated.empty()) {
		ofstream lost(SnapshotPath(path, L"unmigrated"), ios::binary | ios::trunc);
		lost << read.unmigrated.dump(2);
	}
	const filesystem::path temp = path.parent_path() / L"history.json.migration.tmp";
	if (!FolderRewindHistoryStore::SaveHistoryFile(temp, g_appState.configs, read.history)) {
		unit.status = MigrationStatus::Failed; unit.message = L"Could not write converted history.";
		g_historyPersistenceBlocked = true; return unit;
	}
	map<int, vector<HistoryEntry>> verify;
	if (!FolderRewindHistoryStore::LoadHistoryFile(temp, g_appState.configs, verify) || !ReplaceFileAtomically(temp, path)) {
		error_code ec; filesystem::remove(temp, ec);
		unit.status = MigrationStatus::Failed; unit.message = L"Converted history failed validation or atomic replacement.";
		g_historyPersistenceBlocked = true; return unit;
	}
	unit.migratedItems = read.sourceItems - static_cast<int>(read.unmigrated.size());
	unit.skippedItems = static_cast<int>(read.unmigrated.size());
	unit.status = read.unmigrated.empty() ? MigrationStatus::Succeeded : MigrationStatus::Degraded;
	unit.message = read.unmigrated.empty() ? L"1.15 history migrated." : L"History migrated; unmapped items were preserved in a recovery file.";
	g_historyPersistenceBlocked = false;
	return unit;
}

wstring NormalizeHistoryTimestamp(wstring value) {
	// 1.15 normally used yyyy-MM-dd_HH-mm-ss; FolderRewind accepts an ISO local DateTime.
	if (value.size() >= 19 && value[10] == L'_') {
		value[10] = L'T';
		if (value[13] == L'-') value[13] = L':';
		if (value[16] == L'-') value[16] = L':';
	}
	return value;
}

wstring InferLastBackupTime(const Config& config, int configIndex, const wstring& folderName, const LegacyMineBackup15Reader::MetadataSummary& summary, const vector<FolderRewindFormat::ChangeRecord>& records) {
	for (auto it = records.rbegin(); it != records.rend(); ++it) if (!it->createdAtUtc.empty()) return it->createdAtUtc;
	error_code ec;
	auto archive = filesystem::path(config.backupPath) / folderName / summary.lastBackupFileName;
	if (filesystem::exists(archive, ec) && !ec) {
		const auto archiveTime = filesystem::last_write_time(archive, ec);
		if (!ec) return FolderRewindFormat::FormatFileTimeUtc(archiveTime);
	}
	auto historyIt = g_appState.g_history.find(configIndex);
	if (historyIt != g_appState.g_history.end()) {
		for (auto it = historyIt->second.rbegin(); it != historyIt->second.rend(); ++it) {
			if (it->worldName == folderName && it->backupFile == summary.lastBackupFileName && !it->timestamp_str.empty())
				return NormalizeHistoryTimestamp(it->timestamp_str);
		}
	}
	return L"";
}

} // namespace

MigrationStatus HigherPriorityStatus(MigrationStatus lhs, MigrationStatus rhs) {
	auto priority = [](MigrationStatus status) {
		switch (status) {
		case MigrationStatus::Failed: return 4;
		case MigrationStatus::Degraded: return 3;
		case MigrationStatus::Pending: return 2;
		case MigrationStatus::Succeeded: return 1;
		case MigrationStatus::NotNeeded: default: return 0;
		}
	};
	return priority(rhs) > priority(lhs) ? rhs : lhs;
}

wstring GenerateLegacyConfigId(const Config& config, int configIndex) {
	wstring identity;
	if (config.cloudSyncEnabled && !config.rcloneRemotePath.empty() && !config.name.empty()) {
		identity = L"cloud|" + LowerNormalized(config.rcloneRemotePath) + L"|" + LowerNormalized(utf8_to_wstring(config.name));
	}
	else {
		identity = L"local|" + to_wstring(configIndex) + L"|" + LowerNormalized(utf8_to_wstring(config.name))
			+ L"|" + LowerNormalized(config.saveRoot) + L"|" + LowerNormalized(config.backupPath);
	}
	return UuidV5(wstring_to_utf8(identity));
}

MigrationReport RunStartupMigration() {
#if !MINEBACKUP_ENABLE_V15_MIGRATION
	return GetMigrationReport();
#else
	MigrationUnitResult configUnit;
	configUnit.unitId = L"startup:config";
	bool changed = false;
	for (auto& [index, config] : g_appState.configs) {
		if (!config.legacyConfigIdGenerated) continue;
		changed = true;
		configUnit.migratedItems++;
	}
	if (changed) {
		filesystem::path snapshot;
		if (CopySnapshot(L"config.ini", snapshot)) {
			configUnit.snapshotPath = snapshot.wstring();
			SaveConfigs();
			configUnit.status = MigrationStatus::Succeeded;
			configUnit.message = L"Stable ConfigId values were persisted for 1.15 configurations.";
			for (auto& [index, config] : g_appState.configs) config.legacyConfigIdGenerated = false;
		}
		else {
			configUnit.status = MigrationStatus::Failed;
			configUnit.message = L"Could not snapshot config.ini; configuration migration was not committed.";
		}
	}
	else {
		configUnit.status = MigrationStatus::NotNeeded;
		configUnit.message = L"Configuration identities are current.";
	}
	AddOrReplaceUnit(configUnit);
	auto history = MigrateHistory();
	AddOrReplaceUnit(history);
	{
		lock_guard<mutex> lock(g_mutex);
		g_showSummary = configUnit.status != MigrationStatus::NotNeeded || history.status != MigrationStatus::NotNeeded;
	}
	return GetMigrationReport();
#endif
}

MigrationUnitResult EnsureWorldMigrated(int configIndex, const wstring& folderName, const wstring& fallbackPath) {
	auto it = g_appState.configs.find(configIndex);
	if (it == g_appState.configs.end()) {
		MigrationUnitResult result; result.unitId = L"world:" + to_wstring(configIndex) + L":" + folderName;
		result.status = MigrationStatus::Failed; result.message = L"Configuration not found."; return result;
	}
	return EnsureWorldMigrated(it->second, configIndex, folderName, fallbackPath);
}

MigrationUnitResult EnsureWorldMigrated(const Config& config, int configIndex, const wstring& folderName, const wstring& fallbackPath) {
	lock_guard<recursive_mutex> migrationLock(g_executionMutex);
	MigrationUnitResult unit;
	unit.unitId = L"world:" + to_wstring(configIndex) + L":" + folderName;
#if !MINEBACKUP_ENABLE_V15_MIGRATION
	unit.status = MigrationStatus::NotNeeded; return unit;
#else
	FolderRewindFormat::StoragePaths paths;
	if (!FolderRewindFormat::TryResolveStoragePaths(config.backupPath, folderName, fallbackPath, paths)) {
		unit.status = MigrationStatus::Failed; unit.message = L"World storage path is invalid."; AddOrReplaceUnit(unit); return unit;
	}
	const auto legacyPath = paths.metadataDir / L"metadata.json";
	if (!filesystem::exists(legacyPath)) {
		unit.status = MigrationStatus::NotNeeded; unit.message = L"No 1.15 metadata found."; AddOrReplaceUnit(unit); return unit;
	}
	FolderRewindFormat::MetadataState current;
	const bool validNewState = FolderRewindMetadataStore::LoadState(paths.metadataDir, current);
	LegacyMineBackup15Reader::MetadataSummary legacy;
	wstring error;
	if (!LegacyMineBackup15Reader::ReadMetadataSummary(paths.metadataDir, legacy, error)) {
		if (validNewState) {
			unit.status = MigrationStatus::NotNeeded;
			unit.message = L"A valid FolderRewind state is authoritative; unreadable legacy metadata was ignored.";
		}
		else {
			unit.status = MigrationStatus::Failed;
			unit.message = error;
		}
		AddOrReplaceUnit(unit); return unit;
	}
	if (validNewState && current.lastBackupFileName != legacy.lastBackupFileName) {
		unit.status = MigrationStatus::NotNeeded;
		unit.message = L"The FolderRewind chain has advanced beyond the retained 1.15 metadata.";
		AddOrReplaceUnit(unit); return unit;
	}
	if (validNewState) {
		bool allRecordsMigrated = true;
		for (const auto& legacyRecord : legacy.recordIndex) {
			FolderRewindFormat::ChangeRecord existing;
			if (!FolderRewindMetadataStore::LoadRecord(paths.metadataDir, legacyRecord.archiveFileName, existing)) {
				allRecordsMigrated = false;
				break;
			}
		}
		if (allRecordsMigrated) {
			unit.status = MigrationStatus::NotNeeded;
			unit.message = L"FolderRewind state and records are already complete; retained 1.15 files were not reprocessed.";
			AddOrReplaceUnit(unit);
			return unit;
		}
	}
	filesystem::path recoverySnapshot;
	if (!CreateWorldMetadataSnapshot(config, paths, recoverySnapshot, error)) {
		unit.status = MigrationStatus::Failed;
		unit.message = error;
		AddOrReplaceUnit(unit);
		return unit;
	}
	unit.snapshotPath = recoverySnapshot.wstring();
	if (!validNewState && filesystem::exists(paths.statePath)) PreserveInvalidFile(paths.statePath);

	vector<FolderRewindFormat::ChangeRecord> convertedRecords;
	bool degraded = false;
	for (const auto& indexRecord : legacy.recordIndex) {
		FolderRewindFormat::ChangeRecord existing;
		if (FolderRewindMetadataStore::LoadRecord(paths.metadataDir, indexRecord.archiveFileName, existing)) {
			convertedRecords.push_back(std::move(existing)); continue;
		}
		if (const auto recordPath = FolderRewindMetadataStore::TryGetRecordPath(paths.metadataDir, indexRecord.archiveFileName);
			recordPath && filesystem::exists(*recordPath)) PreserveInvalidFile(*recordPath);
		FolderRewindFormat::ChangeRecord record;
		if (!LegacyMineBackup15Reader::ReadChangeRecord(paths.metadataDir, indexRecord.archiveFileName, record, error)) {
			degraded = true; ++unit.skippedItems; continue;
		}
		for (auto* values : { &record.addedFiles, &record.modifiedFiles, &record.deletedFiles, &record.fullFileList })
			for (auto& value : *values) value = FolderRewindFormat::NormalizeRelativePath(value);
		convertedRecords.push_back(std::move(record));
	}

	FolderRewindFormat::MetadataState next;
	next.lastBackupFileName = legacy.lastBackupFileName;
	next.basedOnFullBackup = legacy.basedOnFullBackup;
	for (const auto& [name, state] : legacy.fileStates) {
		if (state.second == 0) { degraded = true; continue; }
		FolderRewindFormat::FileState value;
		value.size = state.first;
		try {
			filesystem::file_time_type fileTime{ filesystem::file_time_type::duration(state.second) };
			value.lastWriteTimeUtc = FolderRewindFormat::FormatFileTimeUtc(fileTime);
		}
		catch (...) { degraded = true; continue; }
		if (value.lastWriteTimeUtc.empty()) { degraded = true; continue; }
		next.fileStates[FolderRewindFormat::NormalizeRelativePath(name)] = std::move(value);
	}
	next.lastBackupTime = InferLastBackupTime(config, configIndex, paths.folderName, legacy, convertedRecords);
	if (next.lastBackupTime.empty()) degraded = true;
	error_code ec;
	if (!filesystem::exists(paths.backupSubDir / next.lastBackupFileName, ec) || ec) degraded = true;
	if (!next.basedOnFullBackup.empty() && (!filesystem::exists(paths.backupSubDir / next.basedOnFullBackup, ec) || ec)) degraded = true;
	map<wstring, const FolderRewindFormat::ChangeRecord*> recordMap;
	for (const auto& record : convertedRecords) {
		recordMap[record.archiveFileName] = &record;
		if (!filesystem::exists(paths.backupSubDir / record.archiveFileName, ec) || ec) degraded = true;
	}
	if (FolderRewindFormat::IsSmartBackupType(next.lastBackupFileName)) {
		wstring cursor = next.lastBackupFileName;
		set<wstring> visited;
		bool reachedFull = false;
		while (!cursor.empty() && visited.insert(cursor).second) {
			auto recordIt = recordMap.find(cursor);
			if (recordIt == recordMap.end()) { degraded = true; break; }
			const auto& record = *recordIt->second;
			if (FolderRewindFormat::IsFullLikeBackupType(record.backupType)) { reachedFull = true; break; }
			cursor = record.previousBackupFileName;
		}
		if (!reachedFull) degraded = true;
	}

	for (const auto& record : convertedRecords) {
		FolderRewindFormat::ChangeRecord existingRecord;
		if (!FolderRewindMetadataStore::LoadRecord(paths.metadataDir, record.archiveFileName, existingRecord)) {
			if (!FolderRewindMetadataStore::SaveRecord(paths.metadataDir, record)) {
				unit.status = MigrationStatus::Failed; unit.message = L"Could not write a converted change record."; AddOrReplaceUnit(unit); return unit;
			}
			++unit.migratedItems;
		}
	}
	// A valid new state is authoritative. A degraded conversion deliberately leaves state absent so the next backup is Full.
	if (!validNewState && !degraded && !FolderRewindMetadataStore::SaveState(paths.metadataDir, next)) {
		unit.status = MigrationStatus::Failed; unit.message = L"Could not commit converted state.json."; AddOrReplaceUnit(unit); return unit;
	}
	unit.status = degraded ? MigrationStatus::Degraded : MigrationStatus::Succeeded;
	unit.message = degraded ? L"Available records were migrated; the next backup will be forced Full." : L"1.15 metadata migrated without changing archive files.";
	AddOrReplaceUnit(unit);
	return unit;
#endif
}

MigrationUnitResult EnsureCloudMigrated(int configIndex) {
	MigrationUnitResult unit;
	unit.unitId = L"cloud:" + to_wstring(configIndex);
	auto it = g_appState.configs.find(configIndex);
	if (it == g_appState.configs.end()) { unit.status = MigrationStatus::Failed; unit.message = L"Configuration not found."; return unit; }
	const wstring identity = LowerNormalized(it->second.rcloneRemotePath) + L"|" + LowerNormalized(utf8_to_wstring(it->second.name));
	for (const auto& [otherIndex, other] : g_appState.configs) {
		if (otherIndex == configIndex || !other.cloudSyncEnabled) continue;
		if (identity == LowerNormalized(other.rcloneRemotePath) + L"|" + LowerNormalized(utf8_to_wstring(other.name))) {
			unit.status = MigrationStatus::Failed; unit.message = L"Two configurations share the same legacy cloud identity."; AddOrReplaceUnit(unit); return unit;
		}
	}
	unit.status = MigrationStatus::Pending;
	unit.message = L"Remote JSON migration will be committed by the cloud operation.";
	AddOrReplaceUnit(unit);
	return unit;
}

void RecordCloudMigrationResult(int configIndex, MigrationStatus status, const wstring& message, const wstring& snapshotPath) {
	MigrationUnitResult unit;
	unit.unitId = L"cloud:" + to_wstring(configIndex);
	unit.status = status;
	unit.message = message;
	unit.snapshotPath = snapshotPath;
	AddOrReplaceUnit(unit);
}

bool RetryMigration(const wstring& unitId) {
	if (unitId == L"startup:config" || unitId == L"startup:history") { RunStartupMigration(); return true; }
	if (unitId.rfind(L"world:", 0) == 0) {
		auto split = unitId.find(L':', 6);
		if (split == wstring::npos) return false;
		try { EnsureWorldMigrated(stoi(unitId.substr(6, split - 6)), unitId.substr(split + 1)); return true; } catch (...) { return false; }
	}
	if (unitId.rfind(L"cloud:", 0) == 0) {
		try { EnsureCloudMigrated(stoi(unitId.substr(6))); return true; } catch (...) { return false; }
	}
	return false;
}

MigrationReport GetMigrationReport() { lock_guard<mutex> lock(g_mutex); return g_report; }
bool ShouldShowStartupSummary() { lock_guard<mutex> lock(g_mutex); return g_showSummary; }
void DismissStartupSummary() { lock_guard<mutex> lock(g_mutex); g_showSummary = false; }
bool IsHistoryPersistenceBlocked() { return g_historyPersistenceBlocked; }

void DrawMigrationSettings() {
	const auto report = GetMigrationReport();
	ImGui::SeparatorText("MineBackup 1.15 -> 1.16 migration");
	ImGui::TextWrapped("Migration is one-way. Recovery snapshots are retained and archive files are never renamed.");
	for (const auto& unit : report.units) {
		const char* state = unit.status == MigrationStatus::Succeeded ? "Succeeded" : unit.status == MigrationStatus::Degraded ? "Degraded"
			: unit.status == MigrationStatus::Failed ? "Failed" : unit.status == MigrationStatus::Pending ? "Pending" : "Not needed";
		ImGui::BulletText("%s: %s", wstring_to_utf8(unit.unitId).c_str(), state);
		if (!unit.message.empty()) ImGui::TextWrapped("%s", wstring_to_utf8(unit.message).c_str());
		if (!unit.snapshotPath.empty()) {
			ImGui::TextWrapped("Recovery snapshot: %s", wstring_to_utf8(unit.snapshotPath).c_str());
			const filesystem::path snapshot(unit.snapshotPath);
			if (filesystem::exists(snapshot)) {
				ImGui::SameLine();
				ImGui::PushID((wstring_to_utf8(unit.unitId) + "_snapshot").c_str());
				if (ImGui::SmallButton("Open")) OpenFolder(snapshot.parent_path().wstring());
				ImGui::PopID();
			}
		}
		if (unit.status == MigrationStatus::Failed || unit.status == MigrationStatus::Degraded) {
			ImGui::PushID(wstring_to_utf8(unit.unitId).c_str());
			if (ImGui::Button("Retry")) RetryMigration(unit.unitId);
			ImGui::PopID();
		}
	}
}

} // namespace MigrationService
