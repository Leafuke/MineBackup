// SpecialMode.cpp — 特殊模式（控制台模式）执行逻辑
// 从 MineBackup.cpp 拆分出，包含 RunSpecialMode() 函数

#include "Broadcast.h"
#include "Globals.h"
#include "AppState.h"
#include "i18n.h"
#include "Console.h"
#include "BackupManager.h"
#include "ConfigManager.h"
#include "text_to_text.h"
#ifdef _WIN32
#include "Platform_win.h"
#elif defined(__APPLE__)
#include "Platform_macos.h"
#else
#include "Platform_linux.h"
#endif

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

// 前向声明
extern Console console;
void ConsoleLog(Console* console, const char* format, ...);
string ProcessCommand(const string& commandStr, Console* console);

void RunSpecialMode(int configId) {
	SpecialConfig spCfg;
	if (g_appState.specialConfigs.count(configId)) {
		spCfg = g_appState.specialConfigs[configId];
	}
	else {
		ConsoleLog(nullptr, L("SPECIAL_CONFIG_NOT_FOUND"), configId);
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
	ConsoleLog(&console, L("AUTO_LOG_START"), time_buf);

	// 设置控制台标题和头部信息
#ifdef _WIN32
	system(("title MineBackup - Automated Task: " + utf8_to_gbk(spCfg.name)).c_str());
#endif
	ConsoleLog(&console, L("AUTOMATED_TASK_RUNNER_HEADER"));
	ConsoleLog(&console, L("EXECUTING_CONFIG_NAME"), (spCfg.name.c_str()));
	ConsoleLog(&console, "----------------------------------------------");
	if (!spCfg.hideWindow) {
		ConsoleLog(&console, L("CONSOLE_QUIT_PROMPT"));
		ConsoleLog(&console, "----------------------------------------------");
	}

	atomic<bool> shouldExit = false;
	vector<thread> taskThreads;
	static Console dummyConsole; // 用于传递给 DoBackup

	// --- 1. 执行旧版一次性命令（向后兼容）---
	for (const auto& cmd : spCfg.commands) {
		ConsoleLog(&console, L("LOG_CMD_EXECUTING"), wstring_to_utf8(cmd).c_str());
#ifdef _WIN32
		system(utf8_to_gbk(wstring_to_utf8(cmd)).c_str());
#else
		system(wstring_to_utf8(cmd).c_str());
#endif
	}

	// --- 2. 如果有新版统一任务，使用新版系统 ---
	if (!spCfg.unifiedTasks.empty()) {
		ConsoleLog(&console, L("UNIFIED_TASK_SYSTEM_START"), static_cast<int>(spCfg.unifiedTasks.size()));
		
		// 按 ID 排序任务
		vector<UnifiedTaskV2> sortedTasks = spCfg.unifiedTasks;
		sort(sortedTasks.begin(), sortedTasks.end(), 
			[](const UnifiedTaskV2& a, const UnifiedTaskV2& b) { return a.id < b.id; });

		// 跟踪并行任务
		vector<thread> parallelThreads;

		for (size_t i = 0; i < sortedTasks.size() && !shouldExit; ++i) {
			const UnifiedTaskV2& task = sortedTasks[i];
			
			if (!task.enabled) {
				ConsoleLog(&console, L("TASK_SKIPPED_DISABLED"), task.name.c_str());
				continue;
			}

			// 创建任务执行函数
			auto executeTask = [&spCfg, &shouldExit](const UnifiedTaskV2& task) {
				ConsoleLog(&console, L("TASK_EXECUTING"), task.name.c_str());

				switch (task.type) {
					case TaskTypeV2::Backup: {
						// 验证配置和世界索引
						if (!g_appState.configs.count(task.configIndex)) {
							ConsoleLog(&console, L("ERROR_INVALID_CONFIG_IN_TASK"), task.configIndex);
							return;
						}

						Config taskConfig = g_appState.configs[task.configIndex];
						if (task.worldIndex < 0 || task.worldIndex >= static_cast<int>(taskConfig.worlds.size())) {
							ConsoleLog(&console, L("ERROR_INVALID_WORLD_IN_TASK"), task.configIndex, task.worldIndex);
							return;
						}

						// 合并特殊配置的参数
						taskConfig.hotBackup = spCfg.hotBackup;
						taskConfig.zipLevel = spCfg.zipLevel;
						if (spCfg.keepCount > 0) taskConfig.keepCount = spCfg.keepCount;
						if (spCfg.cpuThreads > 0) taskConfig.cpuThreads = spCfg.cpuThreads;
						taskConfig.useLowPriority = spCfg.useLowPriority;

						const auto& worldData = taskConfig.worlds[task.worldIndex];
						MyFolder world = { JoinPath(taskConfig.saveRoot, worldData.first).wstring(), worldData.first, worldData.second, taskConfig, task.configIndex, task.worldIndex };

						// 根据触发模式执行
						if (task.triggerMode == TaskTrigger::Once) {
							ConsoleLog(&console, L("TASK_QUEUE_ONETIME_BACKUP"), wstring_to_utf8(worldData.first).c_str());
							g_appState.realConfigIndex = task.configIndex;
							DoBackup(world, console, L"SpecialMode");
							ConsoleLog(&console, L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(worldData.first).c_str());
						}
						else if (task.triggerMode == TaskTrigger::Interval) {
							// 间隔备份：在循环中执行
							ConsoleLog(&console, L("THREAD_STARTED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
							while (!shouldExit) {
								this_thread::sleep_for(chrono::minutes(task.intervalMinutes));
								if (shouldExit) break;
								ConsoleLog(&console, L("BACKUP_PERFORMING_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
								g_appState.realConfigIndex = task.configIndex;
								DoBackup(world, console, L"SpecialMode");
								ConsoleLog(&console, L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(world.name).c_str());
							}
							ConsoleLog(&console, L("THREAD_STOPPED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
						}
						else if (task.triggerMode == TaskTrigger::Scheduled) {
							// 计划备份
							ConsoleLog(&console, L("THREAD_STARTED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
							while (!shouldExit) {
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
								ConsoleLog(&console, L("SCHEDULE_NEXT_BACKUP_AT"), wstring_to_utf8(world.name).c_str(), time_buf2);

								while (time(nullptr) < next_run_t && !shouldExit) {
									this_thread::sleep_for(chrono::seconds(1));
								}

								if (shouldExit) break;

								ConsoleLog(&console, L("BACKUP_PERFORMING_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
								g_appState.realConfigIndex = task.configIndex;
								DoBackup(world, console, L"SpecialMode");
								ConsoleLog(&console, L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(world.name).c_str());
							}
							ConsoleLog(&console, L("THREAD_STOPPED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
						}
						break;
					}

					case TaskTypeV2::Command: {
						ConsoleLog(&console, L("LOG_CMD_EXECUTING"), wstring_to_utf8(task.command).c_str());
#ifdef _WIN32
						wstring workDir = task.workingDirectory.empty() ? L"." : task.workingDirectory;
						RunCommandInBackground(task.command, console, false, workDir);
#else
						if (!task.workingDirectory.empty()) {
							string cmdWithCd = "cd \"" + wstring_to_utf8(task.workingDirectory) + "\" && " + wstring_to_utf8(task.command);
							system(cmdWithCd.c_str());
						} else {
							system(wstring_to_utf8(task.command).c_str());
						}
#endif
						ConsoleLog(&console, L("TASK_COMMAND_COMPLETED"), task.name.c_str());
						break;
					}

					case TaskTypeV2::Script: {
						ConsoleLog(&console, L("TASK_SCRIPT_NOT_IMPLEMENTED"));
						break;
					}
				}
			};

			// 根据执行模式决定是并行还是顺序执行
			bool needsBackgroundThread = (task.type == TaskTypeV2::Backup && 
				(task.triggerMode == TaskTrigger::Interval || task.triggerMode == TaskTrigger::Scheduled));

			if (task.executionMode == TaskExecMode::Parallel || needsBackgroundThread) {
				// 并行执行或需要后台线程
				taskThreads.emplace_back([task, executeTask]() {
					executeTask(task);
				});
				// 如果是并行一次性任务，短暂等待以确保启动
				if (task.executionMode == TaskExecMode::Parallel && !needsBackgroundThread) {
					this_thread::sleep_for(chrono::milliseconds(50));
				}
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
		ConsoleLog(&console, L("LEGACY_TASK_SYSTEM_START"));
		
		for (const auto& task : spCfg.tasks) {
			if (!g_appState.configs.count(task.configIndex) ||
				task.worldIndex < 0 ||
				task.worldIndex >= static_cast<int>(g_appState.configs[task.configIndex].worlds.size()))
			{
				ConsoleLog(&console, L("ERROR_INVALID_WORLD_IN_TASK"), task.configIndex, task.worldIndex);
				continue;
			}

			// 创建任务专用配置（合并基础配置和特殊设置）
			Config taskConfig = g_appState.configs[task.configIndex];
			const auto& worldData = taskConfig.worlds[task.worldIndex];
			taskConfig.hotBackup = spCfg.hotBackup;
			taskConfig.zipLevel = spCfg.zipLevel;
			taskConfig.keepCount = spCfg.keepCount;
			taskConfig.cpuThreads = spCfg.cpuThreads;
			taskConfig.useLowPriority = spCfg.useLowPriority;

			MyFolder world = { JoinPath(taskConfig.saveRoot, worldData.first).wstring(), worldData.first, worldData.second, taskConfig, task.configIndex, task.worldIndex };

			if (task.backupType == 0) { // 类型 0: 一次性备份
				ConsoleLog(&console, L("TASK_QUEUE_ONETIME_BACKUP"), wstring_to_utf8(worldData.first).c_str());
				g_appState.realConfigIndex = task.configIndex;
				DoBackup(world, dummyConsole, L"SpecialMode");
				ConsoleLog(&console, L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(worldData.first).c_str());
			}
			else { // 类型 1 (间隔) 和 2 (计划) 在后台线程运行
				taskThreads.emplace_back([task, world, &shouldExit]() {
					ConsoleLog(&console, L("THREAD_STARTED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());

					while (!shouldExit) {
						time_t next_run_t = 0;
						if (task.backupType == 1) { // 间隔备份
							this_thread::sleep_for(chrono::minutes(task.intervalMinutes));
						}
						else { // 计划备份
							while (true) {
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
								this_thread::sleep_for(chrono::seconds(1));
							}

							char time_buf2[26];
							ctime_s(time_buf2, sizeof(time_buf2), &next_run_t);
							time_buf2[strlen(time_buf2) - 1] = '\0';
							ConsoleLog(&console, L("SCHEDULE_NEXT_BACKUP_AT"), wstring_to_utf8(world.name).c_str(), time_buf2);

							while (time(nullptr) < next_run_t && !shouldExit) {
								this_thread::sleep_for(chrono::seconds(1));
							}
						}

						if (shouldExit) break;

						ConsoleLog(&console, L("BACKUP_PERFORMING_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
						g_appState.realConfigIndex = task.configIndex;
						DoBackup(world, console, L"SpecialMode");
						ConsoleLog(&console, L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(world.name).c_str());
					}
					ConsoleLog(&console, L("THREAD_STOPPED_FOR_WORLD"), wstring_to_utf8(world.name).c_str());
				});
			}
		}
	}

	ConsoleLog(&console, L("INFO_TASKS_INITIATED"));

	// --- 3. 用户输入主循环（如果控制台可见）---
	while (!shouldExit) {
		if (!spCfg.hideWindow && _kbhit()) {
			char c = tolower(_getch_special());
			if (c == 'q') {
				g_stopExitWatcher = true;
				if (g_exitWatcherThread.joinable()) {
					g_exitWatcherThread.join();
				}
				shouldExit = true;
				ConsoleLog(&console, L("INFO_QUIT_SIGNAL_RECEIVED"));
			}
			else if (c == 'm') {
				g_stopExitWatcher = true;
				if (g_exitWatcherThread.joinable()) {
					g_exitWatcherThread.join();
				}
				shouldExit = true;
				g_appState.specialConfigs[configId].autoExecute = false;
				SaveConfigs();
				ConsoleLog(&console, L("INFO_SWITCHING_TO_GUI_MODE"));
#ifdef _WIN32
				wchar_t selfPath[MAX_PATH];
				GetModuleFileNameW(NULL, selfPath, MAX_PATH); // 获得程序路径
				ShellExecuteW(NULL, L"open", selfPath, NULL, NULL, SW_SHOWNORMAL); // 开启
#endif
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
			t.join();
		}
	}

	// 停止所有启动的任务
	{
		lock_guard<mutex> lock(g_appState.task_mutex);
		for (auto& kv : g_appState.g_active_auto_backups) {
			kv.second.stop_flag = true;
		}
	}
	for (auto& kv : g_appState.g_active_auto_backups) {
		if (kv.second.worker.joinable()) kv.second.worker.join();
	}
	g_appState.g_active_auto_backups.clear();

	ConsoleLog(&console, L("INFO_ALL_TASKS_SHUT_DOWN"));

	
	return;
}
