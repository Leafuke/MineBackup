#pragma once
#ifndef _TASK_SYSTEM_H
#define _TASK_SYSTEM_H

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <functional>
#include <filesystem>

// 任务类型枚举
enum class TaskType {
    Backup,     // 备份任务
    Command,    // CMD命令任务
    Script      // 脚本任务（未来扩展）
};

// 任务执行模式
enum class TaskExecutionMode {
    Sequential,    // 顺序执行（等待上一个任务完成）
    Parallel       // 并行执行（和上一个任务同时进行）
};

// 任务触发模式
enum class TaskTriggerMode {
    Once,          // 单次执行
    Interval,      // 间隔执行
    Scheduled      // 计划执行
};

// 统一的任务结构体
struct UnifiedTask {
    int id = 0;                                    // 任务ID（用于排序）
    std::string name;                              // 任务名称
    TaskType type = TaskType::Backup;              // 任务类型
    TaskExecutionMode executionMode = TaskExecutionMode::Sequential;  // 执行模式
    TaskTriggerMode triggerMode = TaskTriggerMode::Once;              // 触发模式
    bool enabled = true;                           // 是否启用

    // 备份任务相关
    int configIndex = -1;                          // 配置索引
    int worldIndex = -1;                           // 世界索引

    // CMD命令相关
    std::wstring command;                          // 要执行的命令
    std::wstring workingDirectory;                 // 工作目录

    // 计划相关
    int intervalMinutes = 15;                      // 间隔时间（分钟）
    int schedMonth = 0;                            // 计划月份 (0=每月)
    int schedDay = 0;                              // 计划日期 (0=每天)
    int schedHour = 0;                             // 计划小时
    int schedMinute = 0;                           // 计划分钟

    // 高级选项
    int retryCount = 0;                            // 失败重试次数
    int retryDelaySeconds = 30;                    // 重试延迟（秒）
    int timeoutMinutes = 0;                        // 超时时间（0=无限制）
    bool notifyOnComplete = false;                 // 完成时通知
    bool notifyOnError = true;                     // 错误时通知
};

// 任务运行状态
struct TaskRunState {
    int taskId = 0;
    bool isRunning = false;
    std::atomic<bool> shouldStop{false};
    std::jthread workerThread;
    std::wstring lastResult;
    std::wstring lastError;
    time_t lastRunTime = 0;
    time_t nextRunTime = 0;
};

// 前向声明
struct Console;

namespace TaskSystem {

enum class LegacyServiceState {
    Unsupported,
    NotInstalled,
    Removable,
    Unsafe,
    QueryFailed
};

struct LegacyServiceInspection {
    LegacyServiceState state = LegacyServiceState::Unsupported;
    std::wstring serviceName;
    std::wstring imagePath;
    std::filesystem::path executable;
    bool running = false;
    std::wstring diagnostic;

    [[nodiscard]] bool CanRemove() const noexcept {
        return state == LegacyServiceState::Removable;
    }
};

} // namespace TaskSystem

// 任务系统管理函数声明
namespace TaskSystem {
    // 任务执行
    void ExecuteTask(const UnifiedTask& task, Console* console);
    void ExecuteAllTasks(const std::vector<UnifiedTask>& tasks, Console* console, bool& shouldExit);
    
    // 1.16 only: inspect and remove an already-installed legacy Windows service.
    // There is deliberately no install/start API.
    [[nodiscard]] LegacyServiceInspection InspectLegacyService(
        const std::wstring& serviceName);
    bool RequestElevatedLegacyServiceRemoval(
        const std::wstring& serviceName, std::wstring& error);
    bool RemoveLegacyServiceAfterValidation(
        const std::wstring& serviceName, std::wstring& error);
    
    // 任务序列化
    std::wstring SerializeTask(const UnifiedTask& task);
    UnifiedTask DeserializeTask(const std::wstring& data);
    
    // 工具函数
    std::string GetTaskTypeName(TaskType type);
    std::string GetExecutionModeName(TaskExecutionMode mode);
    std::string GetTriggerModeName(TaskTriggerMode mode);
}

#endif // _TASK_SYSTEM_H
