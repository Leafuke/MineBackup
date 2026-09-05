#pragma once

#include "MinecraftDiscovery.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// 依赖注入结构，便于跨平台和单元测试
struct NeteaseDiscoveryDependencies {
	std::function<std::optional<std::filesystem::path>(
		const MinecraftDiscoveryContext&, const std::string&)> readDownloadPath;
	std::function<std::optional<std::filesystem::path>(
		std::string_view)> readEnvironmentPath;
	std::function<bool(const std::filesystem::path&, std::error_code&)> isDirectory;
};

// 网易我的世界中国版（Java版与基岩版）自动发现提供程序
// Java版通过读取注册表 DownloadPath 定位 Game\.minecraft
// 基岩版定位 %APPDATA%\MinecraftPE_Netease\minecraftWorlds
class NeteaseMinecraftDiscoveryProvider final : public IMinecraftDiscoveryProvider {
public:
	explicit NeteaseMinecraftDiscoveryProvider(
		NeteaseDiscoveryDependencies dependencies = {});
	~NeteaseMinecraftDiscoveryProvider() override = default;

	std::string Id() const override { return "netease-minecraft"; }

	std::vector<DiscoveryLocation> DiscoverLocations(
		const MinecraftDiscoveryContext& context,
		std::stop_token stopToken) override;

private:
	NeteaseDiscoveryDependencies dependencies_;
};
