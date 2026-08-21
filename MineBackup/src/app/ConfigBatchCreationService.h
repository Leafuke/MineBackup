#pragma once

#include "ConfigFactory.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct ConfigBatchCreationRequest {
	std::vector<ConfigDraft> drafts;
	ConfigFactoryContext factoryContext;
	std::filesystem::path defaultBackupRoot;
	bool markCoreValidationPending = true;
};

struct ConfigBatchCreationResult {
	bool success = false;
	std::vector<int> configIndices;
	std::string errorCode;
};

struct ConfigBatchCreationDependencies {
	std::function<Config(const ConfigDraft&, const ConfigFactoryContext&)> buildConfig;
	std::function<bool()> saveConfigs;
	std::function<void(const std::vector<int>&)> onCommitted;
};

class ConfigBatchCreationService {
public:
	explicit ConfigBatchCreationService(ConfigBatchCreationDependencies dependencies = {});

	ConfigBatchCreationResult Commit(const ConfigBatchCreationRequest& request) const;

private:
	ConfigBatchCreationDependencies dependencies_;
};
