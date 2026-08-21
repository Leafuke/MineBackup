#pragma once

#include "AppPaths.h"
#include "ArchiveRunner.h"
#include "DataModels.h"

#include <filesystem>
#include <functional>
#include <string>
#include <stop_token>
#include <vector>

namespace ChainSafeRetention {

struct Request {
	Config config;
	HistoryEntry entry;
	std::vector<HistoryEntry> history;
	std::filesystem::path backupDirectory;
	std::filesystem::path metadataDirectory;
	AppPaths paths;
	ArchiveRunner* archiveRunner = nullptr;
	std::stop_token stopToken;
	std::function<bool(std::vector<HistoryEntry>)> commitHistory;
};

struct Result {
	bool changed = false;
	bool warning = false;
	std::string detail;
};

Result Remove(Request request);

} // namespace ChainSafeRetention
