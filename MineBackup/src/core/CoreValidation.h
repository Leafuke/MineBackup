#pragma once
#ifndef CORE_VALIDATION_H
#define CORE_VALIDATION_H

#include "DataModels.h"

#include <cstddef>
#include <map>
#include <vector>

using CoreValidationHistorySnapshot =
	std::map<std::wstring, std::vector<HistoryEntry>>;

bool IsLegacyCoreValidationPollution(const HistoryEntry& entry);
std::size_t RemoveLegacyCoreValidationPollution(
	std::vector<HistoryEntry>& entries);
bool AreCoreValidationHistorySnapshotsEqual(
	const CoreValidationHistorySnapshot& before,
	const CoreValidationHistorySnapshot& after,
	std::size_t* changedConfigCount = nullptr);

bool StartCoreValidationAsync(bool automatic);

#endif // CORE_VALIDATION_H
