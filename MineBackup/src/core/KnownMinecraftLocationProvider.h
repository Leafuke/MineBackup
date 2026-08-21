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
	std::function<std::optional<std::string>(std::string_view)> readEnvironment;
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
