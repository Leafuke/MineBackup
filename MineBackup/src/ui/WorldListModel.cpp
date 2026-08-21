#include "WorldListModel.h"

using namespace std;

vector<DisplayWorld> BuildDisplayWorlds(
	const map<int, Config>& configs,
	int selectedConfigIndex) {
	vector<DisplayWorld> result;
	const auto config = configs.find(selectedConfigIndex);
	if (config == configs.end()) return result;
	for (int worldIndex = 0;
		worldIndex < static_cast<int>(config->second.worlds.size());
		++worldIndex) {
		const auto& source = config->second.worlds[worldIndex];
		if (source.second == L"#") continue;
		DisplayWorld world;
		world.name = source.first;
		world.desc = source.second;
		world.baseConfigIndex = selectedConfigIndex;
		world.baseWorldIndex = worldIndex;
		world.effectiveConfig = config->second;
		result.push_back(std::move(world));
	}
	return result;
}

bool IsNarrowWorldListLayout(float availableWidth, float em) {
	return availableWidth < 38.0f * em;
}
