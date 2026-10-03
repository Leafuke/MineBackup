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
		auto configAccess = g_appState.configuration.Write();
		configsSnapshot = configAccess.Configs();
		currentConfigSnapshot = configAccess.Selection();
		allocatorSnapshot = SnapshotNormalConfigIndexAllocator();
		backupRootSnapshot = SettingsState().defaultBackupRootPath;
		validationPendingSnapshot = SettingsState().coreValidationPending.load();
		validationPassedSnapshot = SettingsState().coreValidationPassed.load();

		try {
			for (auto& config : builtConfigs) {
				const int index = AllocateNormalConfigIndex();
				config.configId = FolderRewindFormat::GenerateGuidString();
				const auto [position, inserted] =
					configAccess.Configs().emplace(index, std::move(config));
				(void)position;
				if (!inserted) throw runtime_error("Configuration index collision");
				result.configIndices.push_back(index);
				insertedIds[index] = position->second.configId;
			}
		}
		catch (...) {
			configAccess.Configs() = std::move(configsSnapshot);
			configAccess.Selection() = currentConfigSnapshot;
			RestoreNormalConfigIndexAllocator(allocatorSnapshot);
			result.configIndices.clear();
			result.errorCode = "minecraft.config_batch.allocate_failed";
			return result;
		}

		allocatorAfter = SnapshotNormalConfigIndexAllocator();
		configAccess.Selection() = result.configIndices.front();
		SettingsState().defaultBackupRootPath = request.defaultBackupRoot.wstring();
		if (request.markCoreValidationPending) {
			SettingsState().coreValidationPending.store(true);
			SettingsState().coreValidationPassed.store(false);
		}
	}

	MB_LOG_INFO(minebackup::logging::LogCategory::Application,
		"minecraft.config_batch.commit_started", "selected={}", request.drafts.size());
	// An unexpected persistence exception cannot prove that disk was unchanged.
	ConfigSaveState saveState = ConfigSaveState::NotCommitted;
	try {
		saveState = dependencies_.saveConfigs().state;
	}
	catch (...) {
		saveState = ConfigSaveState::RecoveryRequired;
	}
    if (saveState == ConfigSaveState::RecoveryRequired) {
        g_appState.profileRecoveryRequired.store(true);
        result.errorCode = "profile.transaction.recovery_required";
        return result;
    }
	if (saveState == ConfigSaveState::NotCommitted) {
		// config.ini 从未被替换：这次提交逻辑上什么都没有发生，
		// 恢复所有内存状态，使用户重试得到相同名称和目录。
		auto configAccess = g_appState.configuration.Write();
		for (const auto& [index, id] : insertedIds) {
            auto found = configAccess.Configs().find(index);
            if (found != configAccess.Configs().end() && found->second.configId == id)
                configAccess.Configs().erase(found);
        }
        if (!configAccess.Configs().contains(configAccess.Selection()))
            configAccess.Selection() = configAccess.Configs().contains(currentConfigSnapshot)
                ? currentConfigSnapshot : (configAccess.Configs().empty() ? 1 : configAccess.Configs().begin()->first);
        if (SnapshotNormalConfigIndexAllocator().nextIndex == allocatorAfter.nextIndex)
            RestoreNormalConfigIndexAllocator(allocatorSnapshot);
		SettingsState().defaultBackupRootPath = std::move(backupRootSnapshot);
		SettingsState().coreValidationPending.store(validationPendingSnapshot);
		SettingsState().coreValidationPassed.store(validationPassedSnapshot);
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
