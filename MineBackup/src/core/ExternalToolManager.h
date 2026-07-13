#pragma once

#include "AppPaths.h"

#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>

class NetworkService;

enum class ExternalToolSource {
    Unavailable,
    User,
    Managed,
    Bundled,
    System
};

struct ExternalToolProbe {
    bool available = false;
    std::filesystem::path executable;
    std::wstring version;
    std::wstring diagnostic;
};

struct ExternalToolResolution : ExternalToolProbe {
    ExternalToolSource source = ExternalToolSource::Unavailable;
    bool fellBackFromUserPath = false;
};

struct ManagedToolInstallResult {
    bool success = false;
    std::filesystem::path executable;
    std::wstring error;
};

namespace ExternalToolManager {

inline constexpr wchar_t SevenZipVersion[] = L"26.01-zs-v1.5.7-r1";
inline constexpr char SevenZipWindowsSha256[] = "051afc5dce51d2c8802d81a44ed5433a8d31fb158d9e9eb0a37e75b3b81fd867";
inline constexpr wchar_t RcloneVersion[] = L"1.74.4";

ExternalToolProbe ProbeSevenZip(const std::filesystem::path& executable, std::stop_token stopToken = {});
ExternalToolProbe ProbeRclone(
    const std::filesystem::path& executable,
    const std::wstring& requiredVersion = {},
    std::stop_token stopToken = {});

ExternalToolResolution ResolveSevenZip(
    const std::filesystem::path& userPath,
    const AppPaths& paths,
    std::stop_token stopToken = {});
ExternalToolResolution ResolveRclone(
    const std::filesystem::path& userPath,
    const AppPaths& paths,
    std::stop_token stopToken = {});

ManagedToolInstallResult InstallBundledSevenZipForWindows(
    const void* data,
    std::size_t size,
    const AppPaths& paths,
    std::stop_token stopToken = {});
ManagedToolInstallResult InstallPinnedRclone(
    const NetworkService& network,
    const std::filesystem::path& sevenZipExecutable,
    const AppPaths& paths,
    std::stop_token stopToken = {});

} // namespace ExternalToolManager
