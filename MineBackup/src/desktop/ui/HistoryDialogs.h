#pragma once

#include "HistoryViewModel.h"
#include "UIHelpers.h"

#include <vector>

void DrawHistoryDialogs(
	const UiMetrics& metrics,
	const Config& config,
	int configIndex,
	HistoryWindowController& controller,
	const std::vector<HistoryEntryView>& frameViews,
	const std::vector<HistoryEntry>& entries);
