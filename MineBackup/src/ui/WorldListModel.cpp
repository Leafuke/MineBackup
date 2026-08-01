#include "WorldListModel.h"

using namespace std;

namespace {
	void ApplySpecialOverrides(DisplayWorld& world, const SpecialConfig& special) {
		world.effectiveConfig.zipLevel = special.zipLevel;
		if (special.keepCount > 0) world.effectiveConfig.keepCount = special.keepCount;
		if (special.cpuThreads > 0) world.effectiveConfig.cpuThreads = special.cpuThreads;
		world.effectiveConfig.useLowPriority = special.useLowPriority;
		world.effectiveConfig.blacklist = special.blacklist;
	}

	bool TryAppendWorld(
		vector<DisplayWorld>& result,
		const map<int, Config>& configs,
		int configIndex,
		int worldIndex,
		const SpecialConfig* special,
		bool hideMarked) {
		const auto config = configs.find(configIndex);
		if (config == configs.end()
			|| worldIndex < 0
			|| worldIndex >= static_cast<int>(config->second.worlds.size())) {
			return false;
		}
		const auto& source = config->second.worlds[worldIndex];
		if (hideMarked && source.second == L"#") return false;

		DisplayWorld world;
		world.name = source.first;
		world.desc = source.second;
		world.baseConfigIndex = configIndex;
		world.baseWorldIndex = worldIndex;
		world.effectiveConfig = config->second;
		if (special) ApplySpecialOverrides(world, *special);
		result.push_back(std::move(world));
		return true;
	}
}

vector<DisplayWorld> BuildDisplayWorlds(
	const map<int, Config>& configs,
	const map<int, SpecialConfig>& specialConfigs,
	int selectedConfigIndex,
	bool specialSelection) {
	vector<DisplayWorld> result;
	if (!specialSelection) {
		const auto config = configs.find(selectedConfigIndex);
		if (config == configs.end()) return result;
		for (int worldIndex = 0;
			worldIndex < static_cast<int>(config->second.worlds.size());
			++worldIndex) {
			TryAppendWorld(
				result,
				configs,
				selectedConfigIndex,
				worldIndex,
				nullptr,
				true);
		}
		return result;
	}

	const auto special = specialConfigs.find(selectedConfigIndex);
	if (special == specialConfigs.end()) return result;
	if (!special->second.unifiedTasks.empty()) {
		for (const UnifiedTaskV2& task : special->second.unifiedTasks) {
			if (task.type != TaskTypeV2::Backup || !task.enabled) continue;
			TryAppendWorld(
				result,
				configs,
				task.configIndex,
				task.worldIndex,
				&special->second,
				false);
		}
		return result;
	}

	// 旧任务只读兼容：构造结果与统一任务走同一个合并入口。
	for (const AutomatedTask& task : special->second.tasks) {
		TryAppendWorld(
			result,
			configs,
			task.configIndex,
			task.worldIndex,
			&special->second,
			false);
	}
	return result;
}

bool IsNarrowWorldListLayout(float availableWidth, float em) {
	return availableWidth < 38.0f * em;
}
