#pragma once

#include "AppPaths.h"
#include "AtomicFileWriter.h"
#include "ArchiveRunner.h"
#include "DataModels.h"

#include <filesystem>
#include <functional>
#include <string>
#include <stop_token>
#include <vector>

namespace ChainSafeRetention {

struct HistoryRename {
    HistoryEntry expected;
    std::wstring backupFile;
    std::wstring backupType;
};
struct HistoryChanges {
    std::vector<HistoryEntry> deletions;
    std::vector<HistoryRename> renames;
};
// Applies atomically to the latest entries. False means stale/missing identity,
// a newly important deletion, or a colliding archive name; no entries changed.
bool ApplyHistoryChanges(const Config& config, std::vector<HistoryEntry>& latest, const HistoryChanges& changes);

struct Request {
	Config config;
	HistoryEntry entry;
	std::vector<HistoryEntry> history;
	std::filesystem::path backupDirectory;
	std::filesystem::path metadataDirectory;
	AppPaths paths;
	ArchiveRunner* archiveRunner = nullptr;
	std::stop_token stopToken;
	std::function<bool(const HistoryChanges&)> commitHistory;
	// Optional file-operation/phase injection for deterministic transaction tests.
	std::function<AtomicFileWriter::WriteResult(const std::filesystem::path&, const std::filesystem::path&)> replacePrepared;
	std::function<void()> beforeMetadataCommit;
};

struct Result {
	bool changed = false;
	bool warning = false;
	std::string detail;
	std::filesystem::path recoveryPath;
};

Result Remove(Request request);

} // namespace ChainSafeRetention
