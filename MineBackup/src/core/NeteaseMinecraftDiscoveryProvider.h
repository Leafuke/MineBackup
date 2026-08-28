#pragma once

#include "MinecraftDiscovery.h"

#include <memory>
#include <string>
#include <vector>

// 网易我的世界中国版（Java版与基岩版）自动发现提供程序
// Java版通过读取注册表 DownloadPath 定位 Game\.minecraft
// 基岩版定位 %APPDATA%\MinecraftPE_Netease\minecraftWorlds
class NeteaseMinecraftDiscoveryProvider final : public IMinecraftDiscoveryProvider {
public:
	NeteaseMinecraftDiscoveryProvider() = default;
	~NeteaseMinecraftDiscoveryProvider() override = default;

	std::string Id() const override { return "netease-minecraft"; }

	std::vector<DiscoveryLocation> DiscoverLocations(
		const MinecraftDiscoveryContext& context,
		std::stop_token stopToken) override;
};
