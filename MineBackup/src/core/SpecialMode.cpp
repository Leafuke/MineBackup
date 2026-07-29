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

	// --- 1. 执行旧版一次性命令（向后兼容）---
	for (size_t commandIndex = 0; commandIndex < spCfg.commands.size(); ++commandIndex) {
		const string taskName = "legacy-command-" + to_string(commandIndex + 1);
		minebackup::logging::ScopedLogContext taskContext({{"task", taskName}});
		MB_LOG_INFO(minebackup::logging::LogCategory::Task,
			"task.command.started",
			"Executing legacy shell task '{}' (working_directory=default)", taskName);
		RunUserShellTask(spCfg.commands[commandIndex], {}, taskName);
	}

	// --- 2. 如果有新版统一任务，使用新版系统 ---
	if (!spCfg.unifiedTasks.empty()) {
		SPECIAL_INFO(L("UNIFIED_TASK_SYSTEM_START"), static_cast<int>(spCfg.unifiedTasks.size()));
		
		// 按 ID 排序任务
		vector<UnifiedTaskV2> sortedTasks = spCfg.unifiedTasks;
		sort(sortedTasks.begin(), sortedTasks.end(), 
			[](const UnifiedTaskV2& a, const UnifiedTaskV2& b) { return a.id < b.id; });

		// 跟踪并行任务
		vector<jthread> parallelThreads;

		for (size_t i = 0; i < sortedTasks.size() && !shouldExit; ++i) {
			const UnifiedTaskV2& task = sortedTasks[i];
			
			if (!task.enabled) {
				SPECIAL_INFO(L("TASK_SKIPPED_DISABLED"), task.name.c_str());
				continue;
			}

			// 创建任务执行函数
			auto executeTask = [&spCfg, &shouldExit](const UnifiedTaskV2& task, stop_token stopToken = {}) {
				minebackup::logging::ScopedLogContext taskContext({
					{"task", task.name},
					{"config_index", to_string(task.configIndex)}
				});
				SPECIAL_INFO(L("TASK_EXECUTING"), task.name.c_str());

				switch (task.type) {
					case TaskTypeV2::Backup: {
						// 验证配置和世界索引
						if (!g_appState.configs.count(task.configIndex)) {
							SPECIAL_ERROR(L("ERROR_INVALID_CONFIG_IN_TASK"), task.configIndex);
							return;
						}

						Config taskConfig = g_appState.configs[task.configIndex];
						if (task.worldIndex < 0 || task.worldIndex >= static_cast<int>(taskConfig.worlds.size())) {
							SPECIAL_ERROR(L("ERROR_INVALID_WORLD_IN_TASK"), task.configIndex, task.worldIndex);
							return;
						}

						// 合并特殊配置的参数
						taskConfig.zipLevel = spCfg.zipLevel;
						if (spCfg.keepCount > 0) taskConfig.keepCount = spCfg.keepCount;
						if (spCfg.cpuThreads > 0) taskConfig.cpuThreads = spCfg.cpuThreads;
						taskConfig.useLowPriority = spCfg.useLowPriority;

						const auto& worldData = taskConfig.worlds[task.worldIndex];
						MyFolder world = { JoinPath(taskConfig.saveRoot, worldData.first).wstring(), worldData.first, worldData.second, taskConfig, task.configIndex, task.worldIndex };

						// 根据触发模式执行
						if (task.triggerMode == TaskTrigger::Once) {
							SPECIAL_INFO(L("TASK_QUEUE_ONETIME_BACKUP"), wstring_to_utf8(worldData.first).c_str());
							g_appState.realConfigIndex = task.configIndex;
							RunSpecialBackup(world);
							SPECIAL_INFO(L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(worldData.first).c_str());
						}
						else if (task.triggerMode == TaskTrigger::Interval) {
							// 间隔备份：在循环中执行
							SPECIAL_INFO(L("THREAD_STARTED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
							while (!shouldExit && !stopToken.stop_requested()) {
								if (WaitForSpecialTask(stopToken, shouldExit, chrono::minutes(task.intervalMinutes))) break;
								SPECIAL_INFO(L("BACKUP_PERFORMING_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
								g_appState.realConfigIndex = task.configIndex;
								RunSpecialBackup(world);
								SPECIAL_INFO(L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(world.name).c_str());
							}
							SPECIAL_INFO(L("THREAD_STOPPED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
						}
						else if (task.triggerMode == TaskTrigger::Scheduled) {
							// 计划备份
							SPECIAL_INFO(L("THREAD_STARTED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
							while (!shouldExit && !stopToken.stop_requested()) {
								time_t now_t = time(nullptr);
								tm local_tm;
								localtime_s(&local_tm, &now_t);

								tm target_tm = local_tm;
								target_tm.tm_hour = task.schedHour;
								target_tm.tm_min = task.schedMinute;
								target_tm.tm_sec = 0;

								if (task.schedDay != 0) target_tm.tm_mday = task.schedDay;
								if (task.schedMonth != 0) target_tm.tm_mon = task.schedMonth - 1;

								time_t next_run_t = mktime(&target_tm);

								if (next_run_t <= now_t) {
									if (task.schedDay == 0) target_tm.tm_mday++;
									else if (task.schedMonth == 0) target_tm.tm_mon++;
									else target_tm.tm_year++;
									next_run_t = mktime(&target_tm);
								}

								char time_buf2[26];
								ctime_s(time_buf2, sizeof(time_buf2), &next_run_t);
								time_buf2[strlen(time_buf2) - 1] = '\0';
								SPECIAL_INFO(L("SCHEDULE_NEXT_BACKUP_AT"), wstring_to_utf8(world.name).c_str(), time_buf2);

								while (time(nullptr) < next_run_t && !shouldExit && !stopToken.stop_requested()) {
									this_thread::sleep_for(chrono::seconds(1));
								}

								if (shouldExit || stopToken.stop_requested()) break;

								SPECIAL_INFO(L("BACKUP_PERFORMING_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
								g_appState.realConfigIndex = task.configIndex;
								RunSpecialBackup(world);
								SPECIAL_INFO(L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(world.name).c_str());
							}
							SPECIAL_INFO(L("THREAD_STOPPED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
						}
						break;
					}

					case TaskTypeV2::Command: {
						MB_LOG_INFO(minebackup::logging::LogCategory::Task,
							"task.command.started",
							"Executing shell task '{}' (working_directory={})",
							task.name,
							task.workingDirectory.empty() ? "default" : "configured");
						RunUserShellTask(task.command, task.workingDirectory, task.name);
						break;
					}

					case TaskTypeV2::Script: {
						SPECIAL_WARNING(L("TASK_SCRIPT_NOT_IMPLEMENTED"));
						break;
					}
				}
			};

			// 根据执行模式决定是并行还是顺序执行
			bool needsBackgroundThread = (task.type == TaskTypeV2::Backup && 
				(task.triggerMode == TaskTrigger::Interval || task.triggerMode == TaskTrigger::Scheduled));

			if (needsBackgroundThread) {
				// 周期和计划任务必须保持在可取消的后台线程中。
				taskThreads.emplace_back([task, executeTask](stop_token stopToken) {
					executeTask(task, stopToken);
				});
			}
			else if (task.executionMode == TaskExecMode::Parallel) {
				parallelThreads.emplace_back([task, executeTask](stop_token stopToken) {
					executeTask(task, stopToken);
				});
			} else {
				// 顺序执行：等待之前的并行任务完成
				for (auto& t : parallelThreads) {
					if (t.joinable()) t.join();
				}
				parallelThreads.clear();
				
				// 执行当前任务
				executeTask(task);
			}
		}

		// 等待所有一次性并行任务完成
		for (auto& t : parallelThreads) {
			if (t.joinable()) t.join();
		}
	}
	// --- 3. 如果没有新版任务但有旧版任务，使用旧版系统（向后兼容）---
	else if (!spCfg.tasks.empty()) {
		SPECIAL_INFO(L("LEGACY_TASK_SYSTEM_START"));
		
		for (const auto& task : spCfg.tasks) {
			if (!g_appState.configs.count(task.configIndex) ||
				task.worldIndex < 0 ||
				task.worldIndex >= static_cast<int>(g_appState.configs[task.configIndex].worlds.size()))
			{
				SPECIAL_ERROR(L("ERROR_INVALID_WORLD_IN_TASK"), task.configIndex, task.worldIndex);
				continue;
			}

			// 创建任务专用配置（合并基础配置和特殊设置）
			Config taskConfig = g_appState.configs[task.configIndex];
			const auto& worldData = taskConfig.worlds[task.worldIndex];
			taskConfig.zipLevel = spCfg.zipLevel;
			taskConfig.keepCount = spCfg.keepCount;
			taskConfig.cpuThreads = spCfg.cpuThreads;
			taskConfig.useLowPriority = spCfg.useLowPriority;

			MyFolder world = { JoinPath(taskConfig.saveRoot, worldData.first).wstring(), worldData.first, worldData.second, taskConfig, task.configIndex, task.worldIndex };

			if (task.backupType == 0) { // 类型 0: 一次性备份
				SPECIAL_INFO(L("TASK_QUEUE_ONETIME_BACKUP"), wstring_to_utf8(worldData.first).c_str());
				g_appState.realConfigIndex = task.configIndex;
				RunSpecialBackup(world);
				SPECIAL_INFO(L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(worldData.first).c_str());
			}
			else { // 类型 1 (间隔) 和 2 (计划) 在后台线程运行
				taskThreads.emplace_back([task, world, &shouldExit](stop_token stopToken) {
					SPECIAL_INFO(L("THREAD_STARTED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());

					while (!shouldExit && !stopToken.stop_requested()) {
						time_t next_run_t = 0;
						if (task.backupType == 1) { // 间隔备份
							if (WaitForSpecialTask(stopToken, shouldExit, chrono::minutes(task.intervalMinutes))) break;
						}
						else { // 计划备份
							while (!stopToken.stop_requested() && !shouldExit) {
								time_t now_t = time(nullptr);
								tm local_tm;
								localtime_s(&local_tm, &now_t);

								tm target_tm = local_tm;
								target_tm.tm_hour = task.schedHour;
								target_tm.tm_min = task.schedMinute;
								target_tm.tm_sec = 0;

								if (task.schedDay != 0) target_tm.tm_mday = task.schedDay;
								if (task.schedMonth != 0) target_tm.tm_mon = task.schedMonth - 1;

								next_run_t = mktime(&target_tm);

								if (next_run_t <= now_t) {
									if (task.schedDay == 0) target_tm.tm_mday++;
									else if (task.schedMonth == 0) target_tm.tm_mon++;
									else target_tm.tm_year++;
									next_run_t = mktime(&target_tm);
								}

								if (next_run_t > now_t) break;
								this_thread::sleep_for(chrono::milliseconds(100));
							}
							if (stopToken.stop_requested() || shouldExit) break;

							char time_buf2[26];
							ctime_s(time_buf2, sizeof(time_buf2), &next_run_t);
							time_buf2[strlen(time_buf2) - 1] = '\0';
							SPECIAL_INFO(L("SCHEDULE_NEXT_BACKUP_AT"), wstring_to_utf8(world.name).c_str(), time_buf2);

							while (time(nullptr) < next_run_t && !shouldExit && !stopToken.stop_requested()) {
								this_thread::sleep_for(chrono::seconds(1));
							}
						}

						if (shouldExit || stopToken.stop_requested()) break;

						SPECIAL_INFO(L("BACKUP_PERFORMING_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
						g_appState.realConfigIndex = task.configIndex;
						RunSpecialBackup(world);
						SPECIAL_INFO(L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(world.name).c_str());
					}
					SPECIAL_INFO(L("THREAD_STOPPED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
				});
			}
		}
	}

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
