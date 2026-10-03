#pragma once
#include <chrono>
#include <filesystem>
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
