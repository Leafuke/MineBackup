#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

struct TaskEvent {
    std::wstring type;
    std::wstring message;
    std::map<std::wstring, std::wstring> values;
};

class TaskCoordinator {
public:
    using TaskFunction = std::function<void(std::stop_token)>;

    static TaskCoordinator& Instance();
    bool Submit(std::wstring name, std::vector<std::wstring> resourceKeys, TaskFunction function);
    bool SubmitAndWait(std::wstring name, std::vector<std::wstring> resourceKeys, TaskFunction function);
    bool RequestStop(const std::wstring& name);
    void PostEvent(TaskEvent event);
    std::vector<TaskEvent> PollEvents();
    void StopAndJoin();
    bool IsAcceptingTasks() const;

    static std::stop_token CurrentStopToken();
    static std::wstring WorldResourceKey(
        const std::wstring& configId,
        const std::filesystem::path& worldPath);
    static std::wstring CloudResourceKey(const std::wstring& profileIdentity);
    static std::wstring AutoBackupTaskName(int configIndex, int worldIndex);

private:
    TaskCoordinator();
    ~TaskCoordinator();
    TaskCoordinator(const TaskCoordinator&) = delete;
    TaskCoordinator& operator=(const TaskCoordinator&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};
