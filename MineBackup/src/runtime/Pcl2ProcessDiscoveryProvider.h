#pragma once

#include "MinecraftDiscovery.h"
#include "ProcessInspectionService.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

enum class BoundedTextFileStatus {
	Loaded,
	Missing,
	NotRegular,
	TooLarge,
	ReadFailed
};

struct BoundedTextFileResult {
	BoundedTextFileStatus status = BoundedTextFileStatus::Missing;
	std::string contents;
	std::error_code filesystemError;
};

struct Pcl2DiscoveryDependencies {
	std::function<BoundedTextFileResult(
		const std::filesystem::path&, std::uintmax_t)> readTextFile;
	std::function<bool(const std::filesystem::path&, std::error_code&)> isDirectory;
	std::function<std::vector<std::filesystem::path>(
		const std::filesystem::path&, std::error_code&)> listChildDirectories;
};

class Pcl2ProcessDiscoveryProvider final : public IMinecraftDiscoveryProvider {
public:
	explicit Pcl2ProcessDiscoveryProvider(
		std::shared_ptr<IProcessInspectionService> processInspection,
		Pcl2DiscoveryDependencies dependencies = {});

	std::string Id() const override;
	std::vector<DiscoveryLocation> DiscoverLocations(
		const MinecraftDiscoveryContext& context,
		std::stop_token stopToken) override;

private:
	std::shared_ptr<IProcessInspectionService> processInspection_;
	Pcl2DiscoveryDependencies dependencies_;
};
