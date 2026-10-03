#pragma once

#include "MinecraftDiscovery.h"

#include <memory>
#include <string>
#include <vector>

// Modrinth App 默认实例自动发现提供程序
// 通过文件系统方式枚举 profiles 目录下的实例根
class ModrinthDiscoveryProvider final : public IMinecraftDiscoveryProvider {
public:
	ModrinthDiscoveryProvider() = default;
	~ModrinthDiscoveryProvider() override = default;

	std::string Id() const override { return "modrinth"; }

	std::vector<DiscoveryLocation> DiscoverLocations(
		const MinecraftDiscoveryContext& context,
		std::stop_token stopToken) override;
};
