#pragma once

#include "LaunchOptions.h"

#include <filesystem>
#include <string>

enum class AppPathMode {
    Installed,
    Portable,
    Explicit
};

struct AppPaths {
    std::filesystem::path configRoot;
    std::filesystem::path dataRoot;
    std::filesystem::path stateRoot;
    std::filesystem::path cacheRoot;
    std::filesystem::path runtimeRoot;
    std::filesystem::path toolsRoot;
    std::filesystem::path resourcesRoot;
    std::filesystem::path logsRoot;
    std::wstring profileIdentity;
    AppPathMode mode = AppPathMode::Installed;

    std::filesystem::path ConfigFile() const { return configRoot / L"config.ini"; }
    std::filesystem::path HistoryFile() const { return dataRoot / L"history.json"; }
};

std::filesystem::path GetExecutablePath();
bool ResolveAppPaths(
    const LaunchOptions& options,
    const std::filesystem::path& executablePath,
    AppPaths& paths,
    std::wstring& error);
void SetCurrentAppPaths(AppPaths paths);
const AppPaths& GetAppPaths();
