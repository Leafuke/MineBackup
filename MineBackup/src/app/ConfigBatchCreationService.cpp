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
	if (!dependencies_.saveConfigs) dependencies_.saveConfigs = [] { return SaveConfigsDetailed(); };
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

	FlushUiConfigDraft();
	map<int, Config> configsSnapshot;
	map<int, wstring> insertedIds;
	NormalConfigIndexAllocatorState allocatorAfter;
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
				insertedIds[index] = position->second.configId;
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

		allocatorAfter = SnapshotNormalConfigIndexAllocator();
		g_appState.currentConfigIndex = result.configIndices.front();
		g_defaultBackupRootPath = request.defaultBackupRoot.wstring();
		if (request.markCoreValidationPending) {
			g_CoreValidationPending.store(true);
			g_CoreValidationPassed.store(false);
		}
	}

	MB_LOG_INFO(minebackup::logging::LogCategory::Application,
		"minecraft.config_batch.commit_started", "selected={}", request.drafts.size());
	// 无法确定磁盘 commit point 时（依赖抛出异常），按 NotCommitted 处理，
	// 保持与真正的“未写入”一致的保守回滚。
	ConfigSaveState saveState = ConfigSaveState::NotCommitted;
	try {
		saveState = dependencies_.saveConfigs().state;
	}
	catch (...) {
		saveState = ConfigSaveState::NotCommitted;
	}
	if (saveState == ConfigSaveState::NotCommitted) {
		// config.ini 从未被替换：这次提交逻辑上什么都没有发生，
		// 恢复所有内存状态，使用户重试得到相同名称和目录。
		lock_guard<mutex> lock(g_appState.configsMutex);
		for (const auto& [index, id] : insertedIds) {
            auto found = g_appState.configs.find(index);
            if (found != g_appState.configs.end() && found->second.configId == id)
                g_appState.configs.erase(found);
        }
        if (!g_appState.configs.contains(g_appState.currentConfigIndex))
            g_appState.currentConfigIndex = g_appState.configs.contains(currentConfigSnapshot)
                ? currentConfigSnapshot : (g_appState.configs.empty() ? 1 : g_appState.configs.begin()->first);
        if (SnapshotNormalConfigIndexAllocator().nextIndex == allocatorAfter.nextIndex)
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
	if (saveState == ConfigSaveState::CommittedNotDurable) {
		// config.ini 已被替换：真实 commit point 已越过，
		// 内存必须向磁盘提交状态收敛，绝不允许回滚成旧快照。
		// 也不尝试用 .bak 反向覆盖磁盘——那是第二次写事务，同样可能失败。
		result.warningCode = "minecraft.config_batch.commit_not_durable";
		MB_LOG_WARNING(minebackup::logging::LogCategory::Application,
			"minecraft.config_batch.commit_not_durable",
			"Configuration committed but directory sync was not confirmed.");
	}

    const auto committed = SnapshotConfigState();
    for (int index : result.configIndices) {
        if (auto it = committed.configs.find(index); it != committed.configs.end())
            ObserveUiConfigInserted(index, it->second);
    }
    SelectConfigIndex(result.configIndices.front());
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
