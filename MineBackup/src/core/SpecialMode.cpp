// SpecialMode.cpp — 特殊模式（控制台模式）执行逻辑
// 从 MineBackup.cpp 拆分出，包含 RunSpecialMode() 函数

#include "Broadcast.h"
#include "Globals.h"
#include "AppState.h"
#include "i18n.h"
#include "Logging.h"
#include "BackupManager.h"
#include "ConfigManager.h"
#include "text_to_text.h"
#include "PlatformCompat.h"
#include "DesktopServices.h"
#include "ProcessRunner.h"
#include "TaskCoordinator.h"
#include "SpecialTaskDocument.h"

#ifdef _WIN32
#include <conio.h>
inline int _getch_special() { return _getch(); }
#else
#include <cstdio>
#include <unistd.h>
inline int _getch_special() { return std::getchar(); }
#endif

#include <fstream>

using namespace std;

namespace {
#define SPECIAL_INFO(...) \
	MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Task, \
		"special_mode.message", __VA_ARGS__)
#define SPECIAL_WARNING(...) \
	MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Task, \
		"special_mode.warning", __VA_ARGS__)
#define SPECIAL_ERROR(...) \
	MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Task, \
		"special_mode.error", __VA_ARGS__)

bool WaitForSpecialTask(std::stop_token stopToken, const atomic<bool>& shouldExit, chrono::milliseconds duration) {
	const auto deadline = chrono::steady_clock::now() + duration;
	while (!stopToken.stop_requested() && !shouldExit && chrono::steady_clock::now() < deadline) {
		this_thread::sleep_for(chrono::milliseconds(100));
	}
	return stopToken.stop_requested() || shouldExit;
}

bool RunSpecialBackup(const MyFolder& world) {
	return TaskCoordinator::Instance().SubmitAndWait(L"special-mode backup",
		{ TaskCoordinator::WorldResourceKey(world.config.configId, world.path) },
		[world](stop_token) { DoBackup(world, L"SpecialMode"); });
}

void RunUserShellTask(
	const wstring& command,
	const filesystem::path& workingDirectory,
	string_view taskName) {
	const auto startedAt = chrono::steady_clock::now();
	ShellTaskSpec spec;
	spec.command = command;
	spec.workingDirectory = workingDirectory;
	const auto result = ProcessRunner::RunShellTask(spec);
	if (!result.standardOutput.empty()) {
		minebackup::logging::LogRaw(
			minebackup::logging::LogCategory::Process,
			"process.stdout", result.standardOutput,
			minebackup::logging::LogLevel::Debug, MB_LOG_SOURCE);
	}
	if (!result.standardError.empty()) {
		minebackup::logging::LogRaw(
			minebackup::logging::LogCategory::Process,
			"process.stderr", result.standardError,
			minebackup::logging::LogLevel::Debug, MB_LOG_SOURCE);
	}
	const auto elapsedMs = chrono::duration_cast<chrono::milliseconds>(
		chrono::steady_clock::now() - startedAt).count();
	if (result.status != ProcessStatus::Succeeded) {
		MB_LOG_ERROR(minebackup::logging::LogCategory::Process,
			"process.exit.nonzero",
			"Shell task '{}' failed (exit_code={}, duration_ms={})",
			taskName, result.exitCode, elapsedMs);
	} else {
		MB_LOG_INFO(minebackup::logging::LogCategory::Task,
			"task.command.completed",
			"Shell task '{}' completed (exit_code={}, duration_ms={})",
			taskName, result.exitCode, elapsedMs);
	}
}

bool ResolveSpecialTaskWorld(
	const SpecialTaskTarget& target,
	int& configIndex,
	int& worldIndex,
	Config& config) {
	for (const auto& [candidateIndex, candidate] : g_appState.configs) {
		if (candidate.configId != target.configId) continue;
		for (int candidateWorld = 0;
			candidateWorld < static_cast<int>(candidate.worlds.size());
			++candidateWorld) {
			wstring normalized;
			if (SpecialTaskStorage::TryNormalizeWorldPath(
					candidate.worlds[candidateWorld].first, normalized)
				&& normalized == target.worldPath) {
				configIndex = candidateIndex;
				worldIndex = candidateWorld;
				config = candidate;
				return true;
			}
		}
	}
	return false;
}
}

void RunSpecialMode(int configId) {
	SpecialConfig spCfg;
	if (g_appState.specialConfigs.count(configId)) {
		spCfg = g_appState.specialConfigs[configId];
	}
	else {
		SPECIAL_ERROR(L("SPECIAL_CONFIG_NOT_FOUND"), configId);
		Sleep(3000);
		return;
	}
#ifdef _WIN32
	// 隐藏控制台窗口（如果配置要求）
	if (spCfg.hideWindow) {
		ShowWindow(GetConsoleWindow(), SW_HIDE);
	}
#endif

	time_t now = time(0);
	char time_buf[100];
	ctime_s(time_buf, sizeof(time_buf), &now);
	minebackup::logging::ScopedLogContext specialContext({
		{"config_id", to_string(configId)},
		{"task", spCfg.name},
		{"mode", "special"}
	});
	SPECIAL_INFO(L("AUTO_LOG_START"), time_buf);

	// 设置控制台标题和头部信息
#ifdef _WIN32
	SetConsoleTitleW((L"MineBackup - Automated Task: " + utf8_to_wstring(spCfg.name)).c_str());
#endif
	SPECIAL_INFO(L("AUTOMATED_TASK_RUNNER_HEADER"));
	SPECIAL_INFO(L("EXECUTING_CONFIG_NAME"), (spCfg.name.c_str()));
	SPECIAL_INFO("----------------------------------------------");
	if (!spCfg.hideWindow) {
		SPECIAL_INFO(L("CONSOLE_QUIT_PROMPT"));
		SPECIAL_INFO("----------------------------------------------");
	}

	atomic<bool> shouldExit = false;
	vector<jthread> taskThreads;

	SPECIAL_INFO(L("UNIFIED_TASK_SYSTEM_START"), static_cast<int>(spCfg.specialTasks.size()));
	vector<jthread> parallelThreads;
	for (const SpecialTask& task : spCfg.specialTasks) {
		if (shouldExit) break;
		if (!task.enabled) {
			SPECIAL_INFO(L("TASK_SKIPPED_DISABLED"), task.name.c_str());
			continue;
		}

		auto executeTask = [&spCfg, &shouldExit](const SpecialTask& task, stop_token stopToken = {}) {
			minebackup::logging::ScopedLogContext taskContext({
				{"task", task.name}, {"task_id", wstring_to_utf8(task.taskId)}});
			SPECIAL_INFO(L("TASK_EXECUTING"), task.name.c_str());
			if (task.type == SpecialTaskType::Command) {
				MB_LOG_INFO(minebackup::logging::LogCategory::Task,
					"task.command.started",
					"Executing shell task '{}' (working_directory={})", task.name,
					task.workingDirectory.empty() ? "default" : "configured");
				RunUserShellTask(task.command, task.workingDirectory, task.name);
				return;
			}
			if (task.type == SpecialTaskType::Script) {
				SPECIAL_WARNING(L("TASK_SCRIPT_NOT_IMPLEMENTED"));
				return;
			}

			int configIndex = -1;
			int worldIndex = -1;
			Config taskConfig;
			if (!ResolveSpecialTaskWorld(
					task.target, configIndex, worldIndex, taskConfig)) {
				SPECIAL_ERROR(L("ERROR_INVALID_WORLD_IN_TASK"), -1, -1);
				return;
			}
			taskConfig.zipLevel = spCfg.zipLevel;
			if (spCfg.keepCount > 0) taskConfig.keepCount = spCfg.keepCount;
			if (spCfg.cpuThreads > 0) taskConfig.cpuThreads = spCfg.cpuThreads;
			taskConfig.useLowPriority = spCfg.useLowPriority;
			const auto& worldData = taskConfig.worlds[worldIndex];
			MyFolder world = {
				JoinPath(taskConfig.saveRoot, worldData.first).wstring(),
				worldData.first, worldData.second, taskConfig, configIndex, worldIndex};
			auto runBackup = [&]() {
				g_appState.realConfigIndex = configIndex;
				RunSpecialBackup(world);
				SPECIAL_INFO(L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(world.name).c_str());
			};

			if (task.trigger.type == SpecialTaskTriggerType::Once) {
				SPECIAL_INFO(L("TASK_QUEUE_ONETIME_BACKUP"), wstring_to_utf8(world.name).c_str());
				runBackup();
				return;
			}
			SPECIAL_INFO(L("THREAD_STARTED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
			while (!shouldExit && !stopToken.stop_requested()) {
				if (task.trigger.type == SpecialTaskTriggerType::Interval) {
					if (WaitForSpecialTask(
							stopToken, shouldExit,
							chrono::minutes(task.trigger.intervalMinutes))) break;
				}
				else {
					time_t nowTime = time(nullptr);
					tm target{};
					localtime_s(&target, &nowTime);
					target.tm_hour = task.trigger.hour;
					target.tm_min = task.trigger.minute;
					target.tm_sec = 0;
					if (task.trigger.day != 0) target.tm_mday = task.trigger.day;
					if (task.trigger.month != 0) target.tm_mon = task.trigger.month - 1;
					time_t nextRun = mktime(&target);
					if (nextRun <= nowTime) {
						if (task.trigger.day == 0) target.tm_mday++;
						else if (task.trigger.month == 0) target.tm_mon++;
						else target.tm_year++;
						nextRun = mktime(&target);
					}
					char nextRunText[26]{};
					ctime_s(nextRunText, sizeof(nextRunText), &nextRun);
					if (strlen(nextRunText) > 0) nextRunText[strlen(nextRunText) - 1] = '\0';
					SPECIAL_INFO(L("SCHEDULE_NEXT_BACKUP_AT"),
						wstring_to_utf8(world.name).c_str(), nextRunText);
					while (time(nullptr) < nextRun
						&& !shouldExit && !stopToken.stop_requested()) {
						this_thread::sleep_for(chrono::seconds(1));
					}
				}
				if (shouldExit || stopToken.stop_requested()) break;
				SPECIAL_INFO(L("BACKUP_PERFORMING_FOR_WORLD"),
					wstring_to_utf8(world.name).c_str());
				runBackup();
			}
			SPECIAL_INFO(L("THREAD_STOPPED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
		};

		const bool recurring = task.type == SpecialTaskType::Backup
			&& task.trigger.type != SpecialTaskTriggerType::Once;
		if (recurring) {
			taskThreads.emplace_back([task, executeTask](stop_token stopToken) {
				executeTask(task, stopToken);
			});
		}
		else if (task.executionMode == SpecialTaskExecutionMode::Parallel) {
			parallelThreads.emplace_back([task, executeTask](stop_token stopToken) {
				executeTask(task, stopToken);
			});
		}
		else {
			for (auto& thread : parallelThreads) if (thread.joinable()) thread.join();
			parallelThreads.clear();
			executeTask(task);
		}
	}
	for (auto& thread : parallelThreads) if (thread.joinable()) thread.join();

	SPECIAL_INFO(L("INFO_TASKS_INITIATED"));

	// --- 3. 用户输入主循环（如果控制台可见）---
	while (!shouldExit) {
		if (!spCfg.hideWindow && _kbhit()) {
			char c = tolower(_getch_special());
			if (c == 'q') {
				TaskCoordinator::Instance().RequestStop(L"game-session-watcher");
				shouldExit = true;
				SPECIAL_INFO(L("INFO_QUIT_SIGNAL_RECEIVED"));
			}
			else if (c == 'm') {
				TaskCoordinator::Instance().RequestStop(L"game-session-watcher");
				shouldExit = true;
				g_appState.specialConfigs[configId].autoExecute = false;
				SaveConfigs();
				SPECIAL_INFO(L("INFO_SWITCHING_TO_GUI_MODE"));
				(void)GetDesktopServices()->RestartApplication();
			}
		}

		// 如果启用自动退出且没有后台线程，则可以退出
		if (spCfg.exitAfterExecution && taskThreads.empty()) {
			shouldExit = true;
		}

		this_thread::sleep_for(chrono::milliseconds(200));
	}

	// --- 4. 清理 ---
	for (auto& t : taskThreads) {
		if (t.joinable()) {
			t.request_stop();
			t.join();
		}
	}

	// 停止所有启动的任务
	vector<wstring> autoBackupTaskNames;
	{
		lock_guard<mutex> lock(g_appState.task_mutex);
		for (auto& kv : g_appState.g_active_auto_backups) {
			autoBackupTaskNames.push_back(kv.second.taskName);
		}
		g_appState.g_active_auto_backups.clear();
	}
	for (const auto& taskName : autoBackupTaskNames) {
		TaskCoordinator::Instance().RequestStop(taskName);
	}

	SPECIAL_INFO(L("INFO_ALL_TASKS_SHUT_DOWN"));

	
	return;
}
