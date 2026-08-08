#include "MainUI.h"

#include "AppPaths.h"
#include "AppState.h"
#include "BackupManager.h"
#include "ExternalToolManager.h"
#include "Logging.h"
#include "SpecialTaskDocument.h"
#include "SpecialTaskRunner.h"
#include "TaskCoordinator.h"
#include "text_to_text.h"

#include <filesystem>
#include <optional>

using namespace std;

namespace {

optional<BackupRequest> ResolveDesktopTaskTarget(
	const SpecialTaskTarget& target) {
	lock_guard lock(g_appState.configsMutex);
	for (const auto& [configIndex, config] : g_appState.configs) {
		if (config.configId != target.configId) continue;
		for (size_t worldIndex = 0; worldIndex < config.worlds.size(); ++worldIndex) {
			wstring normalized;
			if (!SpecialTaskStorage::TryNormalizeWorldPath(
					config.worlds[worldIndex].first, normalized)
				|| normalized != target.worldPath) continue;
			BackupRequest request;
			request.config = config;
			request.world = {config.configId, normalized};
			request.sourcePath = filesystem::path(config.saveRoot)
				/ filesystem::path(normalized);
			request.displayName = config.worlds[worldIndex].second;
			request.comment = L"SpecialMode";
			request.legacyConfigIndex = configIndex;
			return request;
		}
	}
	return nullopt;
}
MyFolder ToDesktopFolder(const BackupRequest& request) {
	return {
		request.sourcePath.wstring(),
		request.world.relativePath,
		request.displayName,
		request.config,
		request.legacyConfigIndex,
		-1};
}

} // namespace

void RunSpecialMode(int configId) {
	const auto found = g_appState.specialConfigs.find(configId);
	if (found == g_appState.specialConfigs.end()) {
		MB_LOG_ERROR(
			minebackup::logging::LogCategory::Task,
			"special.config.not_found",
			"Special configuration index {} does not exist.", configId);
		return;
	}
	const SpecialConfig specialConfig = found->second;

	SpecialTaskRunnerDependencies dependencies;
	dependencies.resolveBackup = ResolveDesktopTaskTarget;
	dependencies.preflightBackup = [](const BackupRequest& request) {
		SpecialTaskPreflightResult result;
		const auto tool = ExternalToolManager::ResolveSevenZip(
			request.config.zipPath, GetAppPaths());
		if (!tool.available) {
			result.code = OperationCode::ToolUnavailable;
			result.diagnostics.push_back({
				"backup.tool.unavailable", DiagnosticSeverity::Error,
				wstring_to_utf8(tool.diagnostic)});
		}
		return result;
	};
	dependencies.runBackup = [](const BackupRequest& request, stop_token stopToken) {
		return RunDesktopBackup(
			ToDesktopFolder(request), request.comment, stopToken);
	};
	dependencies.runCommand = [](const ShellTaskSpec& spec, stop_token stopToken) {
		return ProcessRunner::RunShellTask(spec, stopToken);
	};

	SpecialTaskRunner runner(std::move(dependencies));
	const SpecialRunResult result = runner.Run(
		specialConfig,
		TaskCoordinator::CurrentStopToken());
	if (IsSuccessful(result.code)) {
		MB_LOG_INFO(
			minebackup::logging::LogCategory::Task,
			"special.run.completed",
			"Special task run completed with code {}.", ToString(result.code));
	}
	else {
		MB_LOG_ERROR(
			minebackup::logging::LogCategory::Task,
			"special.run.failed",
			"Special task run failed with code {}.", ToString(result.code));
	}
}
