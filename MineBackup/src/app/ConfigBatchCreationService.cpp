#include "ConfigBatchCreationService.h"

#include "AppState.h"
#include "ConfigManager.h"
#include "FolderRewindFormat.h"
#include "Globals.h"
#include "Logging.h"
#include "MainUiController.h"

#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

using namespace std;

ConfigBatchCreationService::ConfigBatchCreationService(
	ConfigBatchCreationDependencies dependencies)
	: dependencies_(std::move(dependencies)) {
	if (!dependencies_.buildConfig) dependencies_.buildConfig = BuildRecommendedConfig;
	if (!dependencies_.saveConfigs) dependencies_.saveConfigs = [] { return SaveConfigs(); };
	if (!dependencies_.onCommitted) {
		dependencies_.onCommitted = [](const vector<int>&) {
			GetMainUiController().worldList.Invalidate();
		};
	}
}

ConfigBatchCreationResult ConfigBatchCreationService::Commit(
	const ConfigBatchCreationRequest& request) const {
	ConfigBatchCreationResult result;
	if (request.drafts.empty()) {
		result.errorCode = "minecraft.config_batch.empty";
		return result;
	}

	vector<Config> builtConfigs;
	builtConfigs.reserve(request.drafts.size());
	try {
		for (const auto& draft : request.drafts) {
			builtConfigs.push_back(
				dependencies_.buildConfig(draft, request.factoryContext));
		}
	}
	catch (const exception& error) {
		MB_LOG_ERROR(minebackup::logging::LogCategory::Application,
			"minecraft.config_batch.build_failed", "{}", error.what());
		result.errorCode = "minecraft.config_batch.build_failed";
		return result;
	}
	catch (...) {
		MB_LOG_ERROR(minebackup::logging::LogCategory::Application,
			"minecraft.config_batch.build_failed", "Unknown config build failure.");
		result.errorCode = "minecraft.config_batch.build_failed";
		return result;
	}

	map<int, Config> configsSnapshot;
	int currentConfigSnapshot = 1;
	NormalConfigIndexAllocatorState allocatorSnapshot;
	wstring backupRootSnapshot;
	bool validationPendingSnapshot = false;
	bool validationPassedSnapshot = false;
	{
		lock_guard<mutex> lock(g_appState.configsMutex);
		configsSnapshot = g_appState.configs;
		currentConfigSnapshot = g_appState.currentConfigIndex;
		allocatorSnapshot = SnapshotNormalConfigIndexAllocator();
		backupRootSnapshot = g_defaultBackupRootPath;
		validationPendingSnapshot = g_CoreValidationPending.load();
		validationPassedSnapshot = g_CoreValidationPassed.load();

		try {
			for (auto& config : builtConfigs) {
				const int index = AllocateNormalConfigIndex();
				config.configId = FolderRewindFormat::GenerateGuidString();
				const auto [position, inserted] =
					g_appState.configs.emplace(index, std::move(config));
				(void)position;
				if (!inserted) throw runtime_error("Configuration index collision");
				result.configIndices.push_back(index);
			}
		}
		catch (...) {
			g_appState.configs = std::move(configsSnapshot);
			g_appState.currentConfigIndex = currentConfigSnapshot;
			RestoreNormalConfigIndexAllocator(allocatorSnapshot);
			result.configIndices.clear();
			result.errorCode = "minecraft.config_batch.allocate_failed";
			return result;
		}

		g_appState.currentConfigIndex = result.configIndices.front();
		g_defaultBackupRootPath = request.defaultBackupRoot.wstring();
		if (request.markCoreValidationPending) {
			g_CoreValidationPending.store(true);
			g_CoreValidationPassed.store(false);
		}
	}

	MB_LOG_INFO(minebackup::logging::LogCategory::Application,
		"minecraft.config_batch.commit_started", "selected={}", request.drafts.size());
	bool saved = false;
	try {
		saved = dependencies_.saveConfigs();
	}
	catch (...) {
		saved = false;
	}
	if (!saved) {
		// 配置文件原子写失败时恢复所有内存状态，使用户重试得到相同名称和目录。
		lock_guard<mutex> lock(g_appState.configsMutex);
		g_appState.configs = std::move(configsSnapshot);
		g_appState.currentConfigIndex = currentConfigSnapshot;
		RestoreNormalConfigIndexAllocator(allocatorSnapshot);
		g_defaultBackupRootPath = std::move(backupRootSnapshot);
		g_CoreValidationPending.store(validationPendingSnapshot);
		g_CoreValidationPassed.store(validationPassedSnapshot);
		result.configIndices.clear();
		result.errorCode = "minecraft.config_batch.commit_failed";
		MB_LOG_ERROR(minebackup::logging::LogCategory::Application,
			"minecraft.config_batch.commit_failed", "Config persistence failed.");
		return result;
	}

	result.success = true;
	try {
		dependencies_.onCommitted(result.configIndices);
	}
	catch (...) {
		MB_LOG_WARNING(minebackup::logging::LogCategory::Application,
			"minecraft.config_batch.refresh_failed",
			"Configuration commit succeeded, but the world-list refresh callback failed.");
	}
	MB_LOG_INFO(minebackup::logging::LogCategory::Application,
		"minecraft.config_batch.commit_completed", "created={}", result.configIndices.size());
	return result;
}
