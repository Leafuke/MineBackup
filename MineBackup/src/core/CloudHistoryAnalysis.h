#pragma once

#include "DataModels.h"

#include <optional>
#include <functional>
#include <vector>

CloudHistoryAnalysisResult AnalyzeRemoteHistory(
	const Config& config,
	const std::vector<HistoryEntry>& localHistory,
	const std::vector<HistoryEntry>& remoteHistory,
	const std::optional<CloudActiveHistoryManifest>& activeManifest);

CloudSyncResult AggregateCloudDownloads(
    const std::vector<HistoryEntry>& entries,
    CloudSyncMode mode,
    const std::function<bool(const HistoryEntry&)>& alreadyAvailable,
    const std::function<CloudCommandResult(const HistoryEntry&)>& download);
