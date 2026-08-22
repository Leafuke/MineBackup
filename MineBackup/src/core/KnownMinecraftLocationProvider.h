#pragma once

#include "MinecraftDiscovery.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

enum class MinecraftHostPlatform {
	Current,
	Windows,
	Linux,
	MacOS
};

struct KnownMinecraftLocationDependencies {
	MinecraftHostPlatform platform = MinecraftHostPlatform::Current;
	// 直接返回文件系统路径，避免 Windows 原生 UTF-16 环境路径
	// 被降级为当前 ANSI 代码页的窄字符串。
	std::function<std::optional<std::filesystem::path>(
		std::string_view)> readEnvironmentPath;
	std::function<bool(const std::filesystem::path&, std::error_code&)> isDirectory;
	std::function<std::vector<std::filesystem::path>(
		const std::filesystem::path&, std::error_code&)> listChildDirectories;
};

class KnownMinecraftLocationProvider final : public IMinecraftDiscoveryProvider {
public:
	explicit KnownMinecraftLocationProvider(
		KnownMinecraftLocationDependencies dependencies = {});

	std::string Id() const override;
	std::vector<DiscoveryLocation> DiscoverLocations(
		const MinecraftDiscoveryContext& context,
		std::stop_token stopToken) override;

private:
	KnownMinecraftLocationDependencies dependencies_;
};
