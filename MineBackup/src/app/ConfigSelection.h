#pragma once

#include "DataModels.h"

#include <map>
#include <string>

int FindConfigByStableId(
	const std::map<int, Config>& configs,
	const std::wstring& stableId);
int FindSpecialConfigByStableId(
	const std::map<int, SpecialConfig>& configs,
	const std::wstring& stableId);
