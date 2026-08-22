#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace AtomicFileWriter {

struct WriteOptions {
    bool keepBackup = true;
    bool createParentDirectories = true;
    // Optional deterministic observation point for low-level contention tests.
    std::function<void(const std::error_code&, std::size_t)> replaceFailureObserver;
};

struct WriteResult {
    bool success = false;
    std::filesystem::path backupPath;
    std::wstring error;
};

WriteResult WriteText(
    const std::filesystem::path& target,
    const std::string& content,
    const WriteOptions& options = {});

} // namespace AtomicFileWriter
