#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct InterruptedTaskRecoveryReport {
    std::vector<std::filesystem::path> removedPaths;
    std::vector<std::wstring> errors;
    std::uintmax_t removedBytes = 0;
    std::filesystem::path reportPath;
};

InterruptedTaskRecoveryReport RecoverInterruptedTaskArtifacts(
    const std::filesystem::path& runtimeRoot,
    const std::filesystem::path& reportPath);
