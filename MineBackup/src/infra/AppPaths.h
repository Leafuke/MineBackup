#pragma once

#include <filesystem>
#include <optional>
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
    std::filesystem::path SpecialTasksFile() const { return configRoot / L"special-tasks.json"; }
    std::filesystem::path JobsFile() const { return configRoot / L"jobs.json"; }
    std::filesystem::path HistoryFile() const { return dataRoot / L"history.json"; }
};

struct AppPathRequest {
    std::optional<std::filesystem::path> dataDirectory;
};

std::filesystem::path GetExecutablePath();
bool ResolveAppPaths(
	const AppPathRequest& request,
    const std::filesystem::path& executablePath,
    AppPaths& paths,
    std::wstring& error);
void SetCurrentAppPaths(AppPaths paths);
const AppPaths& GetAppPaths();
