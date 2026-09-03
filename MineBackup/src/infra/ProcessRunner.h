#pragma once

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

enum class ProcessStatus {
    Succeeded,
    ExitedWithError,
    FailedToStart,
    TimedOut,
    Cancelled
};

struct ProcessSpec {
    std::filesystem::path executable;
    std::vector<std::wstring> arguments;
    std::filesystem::path workingDirectory;
    std::chrono::milliseconds timeout{0};
    std::size_t maximumCapturedBytes = 4u * 1024u * 1024u;
    bool useLowPriority = false;
};

struct ProcessResult {
    ProcessStatus status = ProcessStatus::FailedToStart;
    int exitCode = -1;
    std::string standardOutput;
    std::string standardError;
    bool outputTruncated = false;
    std::wstring error;
};

namespace ProcessRunner {
// 底层进程执行器不感知 TaskCoordinator；需要任务级取消的调用方必须显式传入 token。
ProcessResult Run(const ProcessSpec& spec, std::stop_token stopToken = {});
void TerminateActiveProcess();
}
