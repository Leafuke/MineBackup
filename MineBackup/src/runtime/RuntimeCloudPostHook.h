#pragma once

#include "ExternalToolManager.h"
#include "HistoryRepository.h"
#include "RcloneClient.h"
#include "RuntimeIntegration.h"

#include <functional>
#include <map>

class SynchronousRcloneCloudPostHook final : public ICloudPostHook {
public:
	using ConfigSnapshot = std::function<std::map<int, Config>()>;
	using ToolResolver = std::function<ExternalToolResolution(
		const std::filesystem::path&,
		const AppPaths&,
		std::stop_token)>;

	SynchronousRcloneCloudPostHook(
		AppPaths paths,
		HistoryRepository& history,
		ConfigSnapshot configSnapshot,
		RcloneClient::ProcessExecutor processExecutor = {},
		ToolResolver toolResolver = {});

	CloudPostResult Run(
		const BackupRequest& request,
		const HistoryEntry& historyEntry,
		std::stop_token stopToken) override;

private:
	AppPaths paths_;
	HistoryRepository& history_;
	ConfigSnapshot configSnapshot_;
	RcloneClient::ProcessExecutor processExecutor_;
	ToolResolver toolResolver_;
};

