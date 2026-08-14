#pragma once

#include "BackupService.h"
#include "ExternalToolManager.h"
#include "HistoryRepository.h"

#include <map>
#include <functional>

class RuntimeRetentionService {
public:
	using ToolResolver = std::function<ExternalToolResolution(
		const std::filesystem::path&, const AppPaths&, std::stop_token)>;

	RuntimeRetentionService(
		HistoryRepository& history,
		std::filesystem::path historyFile,
		std::map<int, Config> configs,
		AppPaths paths = {},
		ArchiveRunner::ProcessExecutor processExecutor = {},
		ToolResolver toolResolver = {});

	void Enforce(
		const BackupRequest& request,
		const HistoryEntry& createdEntry);

private:
	HistoryRepository& history_;
	std::filesystem::path historyFile_;
	std::map<int, Config> configs_;
	AppPaths paths_;
	ArchiveRunner::ProcessExecutor processExecutor_;
	ToolResolver toolResolver_;
};
