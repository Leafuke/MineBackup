#include "TaskSystem.h"
#include "AppState.h"
#include "BackupManager.h"
#include "Console.h"
#include "i18n.h"
#include "text_to_text.h"

#ifdef _WIN32
#include "Platform_win.h"
#include <windows.h>
#include <winsvc.h>
#pragma comment(lib, "advapi32.lib")
#endif

#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <filesystem>

using namespace std;

extern Console console;
void ConsoleLog(Console* console, const char* format, ...);

namespace TaskSystem {

    // 获取任务类型名称
    string GetTaskTypeName(TaskType type) {
        switch (type) {
            case TaskType::Backup: return "Backup";
            case TaskType::Command: return "Command";
            case TaskType::Script: return "Script";
            default: return "Unknown";
        }
    }

    // 获取执行模式名称
    string GetExecutionModeName(TaskExecutionMode mode) {
        switch (mode) {
            case TaskExecutionMode::Sequential: return "Sequential";
            case TaskExecutionMode::Parallel: return "Parallel";
            default: return "Unknown";
        }
    }

    // 获取触发模式名称
    string GetTriggerModeName(TaskTriggerMode mode) {
        switch (mode) {
            case TaskTriggerMode::Once: return "Once";
            case TaskTriggerMode::Interval: return "Interval";
            case TaskTriggerMode::Scheduled: return "Scheduled";
            default: return "Unknown";
        }
    }

    // 执行单个任务
    void ExecuteTask(const UnifiedTask& task, Console* console) {
        if (!task.enabled) return;

        ConsoleLog(console, "[Task] Executing: %s (Type: %s)", 
            task.name.c_str(), GetTaskTypeName(task.type).c_str());

        switch (task.type) {
            case TaskType::Backup: {
                // 验证配置和世界索引
                if (!g_appState.configs.count(task.configIndex)) {
                    ConsoleLog(console, L("ERROR_INVALID_WORLD_IN_TASK"), task.configIndex, task.worldIndex);
                    return;
                }

                Config& cfg = g_appState.configs[task.configIndex];
                if (task.worldIndex < 0 || task.worldIndex >= static_cast<int>(cfg.worlds.size())) {
                    ConsoleLog(console, L("ERROR_INVALID_WORLD_IN_TASK"), task.configIndex, task.worldIndex);
                    return;
                }

                const auto& worldData = cfg.worlds[task.worldIndex];
                MyFolder world = {
                    cfg.saveRoot + L"\\" + worldData.first,
                    worldData.first,
                    worldData.second,
                    cfg,
                    task.configIndex,
                    task.worldIndex
                };

                g_appState.realConfigIndex = task.configIndex;
                DoBackup(world, *console, L"TaskSystem");
                ConsoleLog(console, L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(worldData.first).c_str());
                break;
            }

            case TaskType::Command: {
                ConsoleLog(console, L("LOG_CMD_EXECUTING"), wstring_to_utf8(task.command).c_str());
                
                #ifdef _WIN32
                wstring workDir = task.workingDirectory.empty() ? L"." : task.workingDirectory;
                RunCommandInBackground(task.command, *console, false, workDir);
                #else
                system(wstring_to_utf8(task.command).c_str());
                #endif
                
                ConsoleLog(console, "[Task] Command completed: %s", task.name.c_str());
                break;
            }

            case TaskType::Script: {
                // 未来扩展：脚本执行
                ConsoleLog(console, "[Task] Script execution not yet implemented");
                break;
            }
        }
    }

    // 执行所有任务
    void ExecuteAllTasks(const vector<UnifiedTask>& tasks, Console* console, bool& shouldExit) {
        // 按ID排序任务
        vector<UnifiedTask> sortedTasks = tasks;
        sort(sortedTasks.begin(), sortedTasks.end(), 
            [](const UnifiedTask& a, const UnifiedTask& b) { return a.id < b.id; });

        vector<thread> parallelThreads;
        
        for (size_t i = 0; i < sortedTasks.size() && !shouldExit; ++i) {
            const UnifiedTask& task = sortedTasks[i];
            
            if (!task.enabled) continue;

            // 检查执行模式
            if (task.executionMode == TaskExecutionMode::Parallel) {
                // 并行执行：在新线程中运行
                parallelThreads.emplace_back([task, console]() {
                    ExecuteTask(task, console);
                });
            } else {
                // 顺序执行：等待之前的并行任务完成
                for (auto& t : parallelThreads) {
                    if (t.joinable()) t.join();
                }
                parallelThreads.clear();
                
                // 执行当前任务
                ExecuteTask(task, console);
            }
        }

        // 等待所有并行任务完成
        for (auto& t : parallelThreads) {
            if (t.joinable()) t.join();
        }
    }

#ifdef _WIN32
    // Windows服务相关实现
    
    bool InstallService(const ServiceConfig& config) {
        SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
        if (!schSCManager) {
            return false;
        }

        wchar_t modulePath[MAX_PATH];
        GetModuleFileNameW(NULL, modulePath, MAX_PATH);
        
        // 添加服务模式参数
        wstring serviceCmdLine = wstring(modulePath) + L" --service";

        DWORD startType = config.startWithSystem ? SERVICE_AUTO_START : SERVICE_DEMAND_START;
        if (config.delayedStart) {
            startType = SERVICE_AUTO_START;
        }

        SC_HANDLE schService = CreateServiceW(
            schSCManager,
            config.serviceName.c_str(),
            config.serviceDisplayName.c_str(),
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            startType,
            SERVICE_ERROR_NORMAL,
            serviceCmdLine.c_str(),
            NULL, NULL, NULL, NULL, NULL
        );

        if (!schService) {
            CloseServiceHandle(schSCManager);
            return false;
        }

        // 设置服务描述
        SERVICE_DESCRIPTIONW sd;
        sd.lpDescription = const_cast<LPWSTR>(config.serviceDescription.c_str());
        ChangeServiceConfig2W(schService, SERVICE_CONFIG_DESCRIPTION, &sd);

        // 设置延迟启动
        if (config.delayedStart) {
            SERVICE_DELAYED_AUTO_START_INFO delayedInfo;
            delayedInfo.fDelayedAutostart = TRUE;
            ChangeServiceConfig2W(schService, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayedInfo);
        }

        // 设置恢复操作
        SERVICE_FAILURE_ACTIONSW failureActions = {};
        SC_ACTION actions[3];
        actions[0].Type = SC_ACTION_RESTART;
        actions[0].Delay = 60000; // 1分钟后重启
        actions[1].Type = SC_ACTION_RESTART;
        actions[1].Delay = 120000; // 2分钟后重启
        actions[2].Type = SC_ACTION_NONE;
        actions[2].Delay = 0;
        failureActions.dwResetPeriod = 86400; // 24小时重置
        failureActions.lpRebootMsg = NULL;
        failureActions.lpCommand = NULL;
        failureActions.cActions = 3;
        failureActions.lpsaActions = actions;
        ChangeServiceConfig2W(schService, SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions);

        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return true;
    }

    bool UninstallService(const wstring& serviceName) {
        SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (!schSCManager) return false;

        SC_HANDLE schService = OpenServiceW(schSCManager, serviceName.c_str(), DELETE | SERVICE_STOP);
        if (!schService) {
            CloseServiceHandle(schSCManager);
            return false;
        }

        // 停止服务
        SERVICE_STATUS status;
        ControlService(schService, SERVICE_CONTROL_STOP, &status);
        
        // 等待服务停止
        Sleep(1000);

        BOOL result = DeleteService(schService);
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return result == TRUE;
    }

    bool IsServiceInstalled(const wstring& serviceName) {
        SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (!schSCManager) return false;

        SC_HANDLE schService = OpenServiceW(schSCManager, serviceName.c_str(), SERVICE_QUERY_STATUS);
        bool installed = (schService != NULL);
        
        if (schService) CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return installed;
    }

    bool MineStartService(const wstring& serviceName) {
        SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (!schSCManager) return false;

        SC_HANDLE schService = OpenServiceW(schSCManager, serviceName.c_str(), SERVICE_START);
        if (!schService) {
            CloseServiceHandle(schSCManager);
            return false;
        }

        BOOL result = ::StartServiceW(schService, 0, NULL);
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return result == TRUE;
    }

    bool StopService(const wstring& serviceName) {
        SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (!schSCManager) return false;

        SC_HANDLE schService = OpenServiceW(schSCManager, serviceName.c_str(), SERVICE_STOP);
        if (!schService) {
            CloseServiceHandle(schSCManager);
            return false;
        }

        SERVICE_STATUS status;
        BOOL result = ControlService(schService, SERVICE_CONTROL_STOP, &status);
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return result == TRUE;
    }

    bool IsServiceRunning(const wstring& serviceName) {
        SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (!schSCManager) return false;

        SC_HANDLE schService = OpenServiceW(schSCManager, serviceName.c_str(), SERVICE_QUERY_STATUS);
        if (!schService) {
            CloseServiceHandle(schSCManager);
            return false;
        }

        SERVICE_STATUS status;
        if (!QueryServiceStatus(schService, &status)) {
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
            return false;
        }

        bool running = (status.dwCurrentState == SERVICE_RUNNING);
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return running;
    }

#else
    // Linux/macOS 平台的空实现
    bool InstallService(const ServiceConfig& config) { 
        (void)config; // 避免未使用参数警告
        return false; 
    }
    bool UninstallService(const wstring& serviceName) { 
        (void)serviceName;
        return false; 
    }
    bool IsServiceInstalled(const wstring& serviceName) { 
        (void)serviceName;
        return false; 
    }
    bool MineStartService(const wstring& serviceName) { 
        (void)serviceName;
        return false; 
    }
    bool StopService(const wstring& serviceName) { 
        (void)serviceName;
        return false; 
    }
    bool IsServiceRunning(const wstring& serviceName) { 
        (void)serviceName;
        return false; 
    }
#endif

    // 序列化任务
    wstring SerializeTask(const UnifiedTask& task) {
        wstringstream ss;
        ss << task.id << L";"
           << task.name.c_str() << L";"
           << static_cast<int>(task.type) << L";"
           << static_cast<int>(task.executionMode) << L";"
           << static_cast<int>(task.triggerMode) << L";"
           << (task.enabled ? 1 : 0) << L";"
           << task.configIndex << L";"
           << task.worldIndex << L";"
           << task.command << L";"
           << task.workingDirectory << L";"
           << task.intervalMinutes << L";"
           << task.schedMonth << L";"
           << task.schedDay << L";"
           << task.schedHour << L";"
           << task.schedMinute << L";"
           << task.retryCount << L";"
           << task.retryDelaySeconds << L";"
           << task.timeoutMinutes << L";"
           << (task.notifyOnComplete ? 1 : 0) << L";"
           << (task.notifyOnError ? 1 : 0);
        return ss.str();
    }

    // 反序列化任务
    UnifiedTask DeserializeTask(const wstring& data) {
        UnifiedTask task;
        wstringstream ss(data);
        wstring token;
        int idx = 0;
        
        while (getline(ss, token, L';')) {
            switch (idx++) {
                case 0: task.id = stoi(token); break;
                case 1: task.name = wstring_to_utf8(token); break;
                case 2: task.type = static_cast<TaskType>(stoi(token)); break;
                case 3: task.executionMode = static_cast<TaskExecutionMode>(stoi(token)); break;
                case 4: task.triggerMode = static_cast<TaskTriggerMode>(stoi(token)); break;
                case 5: task.enabled = (stoi(token) != 0); break;
                case 6: task.configIndex = stoi(token); break;
                case 7: task.worldIndex = stoi(token); break;
                case 8: task.command = token; break;
                case 9: task.workingDirectory = token; break;
                case 10: task.intervalMinutes = stoi(token); break;
                case 11: task.schedMonth = stoi(token); break;
                case 12: task.schedDay = stoi(token); break;
                case 13: task.schedHour = stoi(token); break;
                case 14: task.schedMinute = stoi(token); break;
                case 15: task.retryCount = stoi(token); break;
                case 16: task.retryDelaySeconds = stoi(token); break;
                case 17: task.timeoutMinutes = stoi(token); break;
                case 18: task.notifyOnComplete = (stoi(token) != 0); break;
                case 19: task.notifyOnError = (stoi(token) != 0); break;
            }
        }
        
        return task;
    }

} // namespace TaskSystem
