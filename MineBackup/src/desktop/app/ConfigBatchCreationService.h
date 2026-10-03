#pragma once

#include "ConfigFactory.h"
#include "ConfigManager.h"

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
	// 非阻塞持久化告警（如配置已提交但目录同步未确认）；
	// 此时 success 仍为 true，Wizard 不得按失败处理。
	std::string warningCode;
};

struct ConfigBatchCreationDependencies {
	std::function<Config(const ConfigDraft&, const ConfigFactoryContext&)> buildConfig;
	// 返回详细持久化状态：NotCommitted 才允许内存回滚；
	// CommittedNotDurable / CommittedDurably 均视为逻辑已提交。
	std::function<ConfigSaveResult()> saveConfigs;
	std::function<void(const std::vector<int>&)> onCommitted;
};

class ConfigBatchCreationService {
public:
	explicit ConfigBatchCreationService(ConfigBatchCreationDependencies dependencies = {});

	ConfigBatchCreationResult Commit(const ConfigBatchCreationRequest& request) const;

private:
	ConfigBatchCreationDependencies dependencies_;
};
