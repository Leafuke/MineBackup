#pragma once

#include "DataModels.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

struct DisplayWorld {
	std::wstring name;
	std::wstring desc;
	int baseConfigIndex = -1;
	int baseWorldIndex = -1;
	Config effectiveConfig;
};

struct WorldSelectionKey {
	int configIndex = -1;
	int worldIndex = -1;

	friend bool operator==(const WorldSelectionKey&, const WorldSelectionKey&) = default;
};

std::vector<DisplayWorld> BuildDisplayWorlds(
	const std::map<int, Config>& configs,
	const std::map<int, SpecialConfig>& specialConfigs,
	int selectedConfigIndex,
	bool specialSelection);

bool IsNarrowWorldListLayout(float availableWidth, float em);
