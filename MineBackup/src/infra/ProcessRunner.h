#pragma once

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include "ProcessModels.h"

namespace ProcessRunner {
// 底层进程执行器不感知 TaskCoordinator；需要任务级取消的调用方必须显式传入 token。
ProcessResult Run(const ProcessSpec& spec, std::stop_token stopToken = {});
// POSIX: terminal shutdown of all owned process groups; further Run calls are
// cancelled. Call only from ordinary thread context, never a signal handler.
// Windows lifetime is enforced by per-process kill-on-close Job Objects.
void TerminateActiveProcess();
}
