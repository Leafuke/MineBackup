#pragma once

#include "BackupService.h"
#include "HistoryRepository.h"

#include <map>

class RuntimeRetentionService {
public:
	RuntimeRetentionService(
		HistoryRepository& history,
		std::filesystem::path historyFile,
		std::map<int, Config> configs);

	void Enforce(
		const BackupRequest& request,
		const HistoryEntry& createdEntry);

private:
	HistoryRepository& history_;
	std::filesystem::path historyFile_;
	std::map<int, Config> configs_;
};
