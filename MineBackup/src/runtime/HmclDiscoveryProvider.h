#pragma once

#include "MinecraftDiscovery.h"

#include <memory>
#include <string>
#include <vector>

// HMCL (Hello Minecraft! Launcher) 游戏目录发现提供程序
// 通过读取 user-game-directories.json 获取用户登记的绝对游戏目录
class HmclDiscoveryProvider final : public IMinecraftDiscoveryProvider {
public:
	HmclDiscoveryProvider() = default;
	~HmclDiscoveryProvider() override = default;

	std::string Id() const override { return "hmcl"; }

	std::vector<DiscoveryLocation> DiscoverLocations(
		const MinecraftDiscoveryContext& context,
		std::stop_token stopToken) override;
};
