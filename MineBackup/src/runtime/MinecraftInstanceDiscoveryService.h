#pragma once

#include "DataModels.h"
#include "MinecraftDiscovery.h"
#include "MinecraftInstanceInspector.h"

#include <map>
#include <memory>
#include <stop_token>
#include <vector>

class MinecraftInstanceDiscoveryService {
public:
	MinecraftInstanceDiscoveryService(
		std::vector<std::shared_ptr<IMinecraftDiscoveryProvider>> providers,
		MinecraftInstanceInspector inspector = {});

	MinecraftDiscoveryResult Discover(
		const std::map<int, Config>& existingConfigs,
		std::vector<DiscoveryLocation> additionalLocations = {},
		std::stop_token stopToken = {}) const;

private:
	std::vector<std::shared_ptr<IMinecraftDiscoveryProvider>> providers_;
	MinecraftInstanceInspector inspector_;
};

MinecraftInstanceDiscoveryService CreateDefaultMinecraftDiscoveryService();
