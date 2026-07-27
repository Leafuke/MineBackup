#include "TaskSystem.h"
#include "AppState.h"
#include "BackupManager.h"
#include "Console.h"
#include "i18n.h"
#include "text_to_text.h"
#include "PlatformCompat.h"
#include "ProcessRunner.h"
#include "TaskCoordinator.h"
#include "LegacyServicePolicy.h"

#ifdef _WIN32
#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#endif

#include <chrono>
#include <cwctype>
#include <thread>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <filesystem>
#include <vector>

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
                    JoinPath(cfg.saveRoot, worldData.first).wstring(),
                    worldData.first,
                    worldData.second,
                    cfg,
                    task.configIndex,
                    task.worldIndex
                };

                g_appState.realConfigIndex = task.configIndex;
                TaskCoordinator::Instance().SubmitAndWait(L"task-system backup",
                    {TaskCoordinator::WorldResourceKey(world.config.configId, world.path)},
                    [world](stop_token) { DoBackup(world, L"TaskSystem"); });
                ConsoleLog(console, L("TASK_SPECIAL_BACKUP_DONE"), wstring_to_utf8(worldData.first).c_str());
                break;
            }

            case TaskType::Command: {
                ConsoleLog(console, L("LOG_CMD_EXECUTING"), wstring_to_utf8(task.command).c_str());
                ShellTaskSpec spec;
                spec.command = task.command;
                spec.workingDirectory = task.workingDirectory;
                const auto result = ProcessRunner::RunShellTask(spec);
                if (!result.standardOutput.empty()) console->AddLog("%s", result.standardOutput.c_str());
                if (!result.standardError.empty()) console->AddLog("%s", result.standardError.c_str());
                if (result.status != ProcessStatus::Succeeded) {
                    console->AddLog("[Error] Shell task failed with exit code %d.", result.exitCode);
                }
                
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

        vector<jthread> parallelThreads;
        
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
    namespace {

    struct ServiceHandle {
        SC_HANDLE value = nullptr;
        ~ServiceHandle() { if (value) CloseServiceHandle(value); }
        ServiceHandle(const ServiceHandle&) = delete;
        ServiceHandle& operator=(const ServiceHandle&) = delete;
        ServiceHandle() = default;
    };

    bool IsValidServiceName(const wstring& serviceName) {
        if (serviceName.empty() || serviceName.size() > 256
            || serviceName.find_first_of(L"/\\") != wstring::npos) {
            return false;
        }
        for (const wchar_t character : serviceName) {
            if (iswcntrl(character)) return false;
        }
        return true;
    }

    wstring WindowsErrorMessage(DWORD code) {
        wchar_t* buffer = nullptr;
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        wstring message = length && buffer ? wstring(buffer, length) : L"Windows error " + to_wstring(code);
        if (buffer) LocalFree(buffer);
        while (!message.empty() && iswspace(message.back())) message.pop_back();
        return message;
    }

    bool IsLegacyMineBackupExecutable(const filesystem::path& executable) {
        error_code error;
        if (!filesystem::is_regular_file(executable, error)) return false;
        HMODULE module = LoadLibraryExW(executable.c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!module) return false;
        const bool hasSevenZip = FindResourceW(module, MAKEINTRESOURCEW(101), L"EXE") != nullptr;
        const bool hasIcon = FindResourceW(module, MAKEINTRESOURCEW(102),
            MAKEINTRESOURCEW(14)) != nullptr;
        FreeLibrary(module);
        return hasSevenZip && hasIcon;
    }

    LegacyServiceInspection InspectServiceHandle(const wstring& serviceName, SC_HANDLE service) {
        LegacyServiceInspection inspection;
        inspection.serviceName = serviceName;
        DWORD required = 0;
        QueryServiceConfigW(service, nullptr, 0, &required);
        const DWORD sizeQueryError = GetLastError();
        if (sizeQueryError != ERROR_INSUFFICIENT_BUFFER || required == 0) {
            inspection.state = LegacyServiceState::QueryFailed;
            inspection.diagnostic = L"Could not read the legacy service configuration: "
                + WindowsErrorMessage(sizeQueryError);
            return inspection;
        }
        vector<BYTE> storage(required);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
        if (!QueryServiceConfigW(service, config, required, &required)) {
            inspection.state = LegacyServiceState::QueryFailed;
            inspection.diagnostic = L"Could not read the legacy service ImagePath: "
                + WindowsErrorMessage(GetLastError());
            return inspection;
        }
        inspection.imagePath = config->lpBinaryPathName ? config->lpBinaryPathName : L"";
        const auto parsed = ParseLegacyServiceImagePath(inspection.imagePath);
        if (!parsed.valid) {
            inspection.state = LegacyServiceState::Unsafe;
            inspection.diagnostic = parsed.diagnostic;
            return inspection;
        }
        inspection.executable = parsed.executable;
        if (!IsLegacyMineBackupExecutable(inspection.executable)) {
            inspection.state = LegacyServiceState::Unsafe;
            inspection.diagnostic = L"The service executable is missing or does not contain MineBackup resources.";
            return inspection;
        }
        SERVICE_STATUS_PROCESS status{};
        DWORD statusBytes = 0;
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<BYTE*>(&status), sizeof(status), &statusBytes)) {
            inspection.running = status.dwCurrentState != SERVICE_STOPPED;
        }
        inspection.state = LegacyServiceState::Removable;
        inspection.diagnostic = L"The service ImagePath was verified as a legacy MineBackup service.";
        return inspection;
    }

    wstring QuoteWindowsArgument(const wstring& argument) {
        wstring quoted = L"\"";
        size_t backslashes = 0;
        for (const wchar_t character : argument) {
            if (character == L'\\') {
                ++backslashes;
                continue;
            }
            if (character == L'\"') {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'\"');
        return quoted;
    }

    } // namespace

    LegacyServiceInspection InspectLegacyService(const wstring& serviceName) {
        LegacyServiceInspection inspection;
        inspection.serviceName = serviceName;
        if (!IsValidServiceName(serviceName)) {
            inspection.state = LegacyServiceState::Unsafe;
            inspection.diagnostic = L"The legacy service name is empty or invalid.";
            return inspection;
        }
        ServiceHandle manager;
        manager.value = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager.value) {
            inspection.state = LegacyServiceState::QueryFailed;
            inspection.diagnostic = L"Could not open Windows Service Control Manager: "
                + WindowsErrorMessage(GetLastError());
            return inspection;
        }
        ServiceHandle service;
        service.value = OpenServiceW(manager.value, serviceName.c_str(),
            SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
        if (!service.value) {
            const DWORD code = GetLastError();
            if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
                inspection.state = LegacyServiceState::NotInstalled;
                inspection.diagnostic = L"No legacy service with this configured name is installed.";
            }
            else {
                inspection.state = LegacyServiceState::QueryFailed;
                inspection.diagnostic = L"Could not inspect the configured legacy service: "
                    + WindowsErrorMessage(code);
            }
            return inspection;
        }
        return InspectServiceHandle(serviceName, service.value);
    }

    bool RequestElevatedLegacyServiceRemoval(const wstring& serviceName, wstring& error) {
        error.clear();
        const auto inspection = InspectLegacyService(serviceName);
        if (!inspection.CanRemove()) {
            error = inspection.diagnostic;
            return false;
        }
        vector<wchar_t> executable(32768);
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
            static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size()) {
            error = L"Could not locate the current MineBackup executable.";
            return false;
        }
        const wstring parameters = L"--cleanup-legacy-service " + QuoteWindowsArgument(serviceName);
        SHELLEXECUTEINFOW request{sizeof(request)};
        request.fMask = SEE_MASK_NOASYNC;
        request.lpVerb = L"runas";
        request.lpFile = executable.data();
        request.lpParameters = parameters.c_str();
        request.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&request)) {
            const DWORD code = GetLastError();
            error = code == ERROR_CANCELLED
                ? L"Administrator approval was cancelled; the service was not changed."
                : L"Could not start the validated service cleanup helper: "
                    + WindowsErrorMessage(code);
            return false;
        }
        return true;
    }

    bool RemoveLegacyServiceAfterValidation(const wstring& serviceName, wstring& error) {
        error.clear();
        if (!IsValidServiceName(serviceName)) {
            error = L"The legacy service name is empty or invalid.";
            return false;
        }
        ServiceHandle manager;
        manager.value = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager.value) {
            error = L"Could not open Windows Service Control Manager: "
                + WindowsErrorMessage(GetLastError());
            return false;
        }
        ServiceHandle service;
        service.value = OpenServiceW(manager.value, serviceName.c_str(),
            SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);
        if (!service.value) {
            error = L"Could not open the legacy service for removal: "
                + WindowsErrorMessage(GetLastError());
            return false;
        }
        const auto inspection = InspectServiceHandle(serviceName, service.value);
        if (!inspection.CanRemove()) {
            error = L"Safety validation refused to remove the service: " + inspection.diagnostic;
            return false;
        }

        SERVICE_STATUS_PROCESS status{};
        DWORD statusBytes = 0;
        if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<BYTE*>(&status), sizeof(status), &statusBytes)) {
            error = L"Could not query the legacy service state: "
                + WindowsErrorMessage(GetLastError());
            return false;
        }
        if (status.dwCurrentState != SERVICE_STOPPED) {
            SERVICE_STATUS ignored{};
            if (!ControlService(service.value, SERVICE_CONTROL_STOP, &ignored)) {
                const DWORD code = GetLastError();
                if (code == ERROR_SERVICE_NOT_ACTIVE) {
                    status.dwCurrentState = SERVICE_STOPPED;
                }
                else {
                    error = L"Could not stop the legacy service: "
                        + WindowsErrorMessage(code);
                    return false;
                }
            }
            const auto deadline = chrono::steady_clock::now() + chrono::seconds(15);
            while (status.dwCurrentState != SERVICE_STOPPED
                && chrono::steady_clock::now() < deadline) {
                Sleep(250);
                if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
                        reinterpret_cast<BYTE*>(&status), sizeof(status), &statusBytes)) {
                    error = L"Could not verify that the legacy service stopped: "
                        + WindowsErrorMessage(GetLastError());
                    return false;
                }
            }
            if (status.dwCurrentState != SERVICE_STOPPED) {
                error = L"The legacy service did not stop within 15 seconds and was not deleted.";
                return false;
            }
        }
        if (!DeleteService(service.value)) {
            const DWORD code = GetLastError();
            if (code != ERROR_SERVICE_MARKED_FOR_DELETE) {
                error = L"Could not delete the validated legacy service: "
                    + WindowsErrorMessage(code);
                return false;
            }
        }
        return true;
    }

#else
    LegacyServiceInspection InspectLegacyService(const wstring& serviceName) {
        return {LegacyServiceState::Unsupported, serviceName, {}, {}, false,
            L"Legacy Windows service cleanup is unavailable on this platform."};
    }

    bool RequestElevatedLegacyServiceRemoval(const wstring&, wstring& error) {
        error = L"Legacy Windows service cleanup is unavailable on this platform.";
        return false;
    }

    bool RemoveLegacyServiceAfterValidation(const wstring&, wstring& error) {
        error = L"Legacy Windows service cleanup is unavailable on this platform.";
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
