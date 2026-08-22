#pragma once

#include "MinecraftDiscovery.h"

class MinecraftInstanceInspector {
public:
	// 兼容计划中的简洁接口；需要错误诊断时使用 InspectDetailed。
	std::vector<InspectedMinecraftInstance> Inspect(
		const DiscoveryLocation& location,
		std::stop_token stopToken = {}) const;

	MinecraftInspectionResult InspectDetailed(
		const DiscoveryLocation& location,
		std::stop_token stopToken = {}) const;
};
