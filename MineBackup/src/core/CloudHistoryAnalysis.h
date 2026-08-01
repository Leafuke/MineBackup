#pragma once

#include "DataModels.h"

#include <optional>
#include <vector>

CloudHistoryAnalysisResult AnalyzeRemoteHistory(
	const Config& config,
	const std::vector<HistoryEntry>& localHistory,
	const std::vector<HistoryEntry>& remoteHistory,
	const std::optional<CloudActiveHistoryManifest>& activeManifest);
