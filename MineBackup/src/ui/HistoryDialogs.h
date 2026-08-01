#pragma once

#include "HistoryViewModel.h"
#include "UIHelpers.h"

#include <vector>

void DrawHistoryDialogs(
	const UiMetrics& metrics,
	Config& config,
	int configIndex,
	HistoryWindowController& controller,
	const std::vector<HistoryEntryView>& frameViews);
