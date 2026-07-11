#pragma once

#include <filesystem>
#include <string>

namespace AtomicFileWriter {

struct WriteOptions {
    bool keepBackup = true;
    bool createParentDirectories = true;
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
