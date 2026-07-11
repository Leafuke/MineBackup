#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace RotatingFileLog {

inline constexpr std::uintmax_t kDefaultMaximumBytes = 5u * 1024u * 1024u;
inline constexpr int kDefaultFileCount = 5;

bool Append(
    const std::filesystem::path& path,
    const std::string& text,
    std::uintmax_t maximumBytes = kDefaultMaximumBytes,
    int fileCount = kDefaultFileCount);

} // namespace RotatingFileLog
