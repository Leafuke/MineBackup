#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct LaunchOptions {
    std::optional<std::filesystem::path> dataDirectory;
    bool autostart = false;
    bool silentStartup = false;
    std::wstring selectConfigId;
    std::wstring runSpecialId;
    std::wstring legacyServiceCleanup;
    std::optional<int> legacySpecialConfigIndex;
    bool legacyServiceMode = false;
};

bool ParseLaunchOptions(
    const std::vector<std::wstring>& arguments,
    LaunchOptions& options,
    std::wstring& error);
