#pragma once

#include "MinecraftDiscovery.h"

#include <memory>
#include <string>
#include <vector>

// Prism Launcher 实例自动发现提供程序
// 通过解析 prismlauncher.cfg (InstanceDir) 与各实例的 instance.cfg 发现游戏根目录
class PrismLauncherDiscoveryProvider final : public IMinecraftDiscoveryProvider {
public:
	PrismLauncherDiscoveryProvider() = default;
	~PrismLauncherDiscoveryProvider() override = default;

	std::string Id() const override { return "prism-launcher"; }

	std::vector<DiscoveryLocation> DiscoverLocations(
		const MinecraftDiscoveryContext& context,
		std::stop_token stopToken) override;
};
