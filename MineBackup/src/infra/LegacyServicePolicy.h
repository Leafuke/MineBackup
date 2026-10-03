#pragma once

#include <filesystem>
#include <string>

struct LegacyServiceImagePath {
    bool valid = false;
    std::filesystem::path executable;
    std::wstring diagnostic;
};

// Parses the ImagePath written by MineBackup 1.15 and earlier. Both the old
// unquoted form and the corrected quoted form are accepted, but --service must
// be the only argument and the executable must be an absolute MineBackup.exe.
[[nodiscard]] LegacyServiceImagePath ParseLegacyServiceImagePath(
    const std::wstring& imagePath);
