#include "AppPaths.h"

#include "FolderRewindFormat.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace std;

namespace {

mutex g_pathsMutex;
AppPaths g_paths;
bool g_pathsConfigured = false;

filesystem::path EnvironmentPath(const char* name) {
#ifdef _WIN32
    const wstring wideName(name, name + char_traits<char>::length(name));
    const size_t required = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (required == 0) return {};
    wstring value(required, L'\0');
    if (GetEnvironmentVariableW(wideName.c_str(), value.data(), static_cast<DWORD>(value.size())) == 0) return {};
    value.resize(required - 1);
    return filesystem::path(value);
#else
    const char* value = getenv(name);
    return value && *value ? filesystem::path(value) : filesystem::path{};
#endif
}

filesystem::path HomeDirectory() {
#ifdef _WIN32
    auto home = EnvironmentPath("USERPROFILE");
#else
    auto home = EnvironmentPath("HOME");
#endif
    return home;
}

filesystem::path NormalizeAbsolute(const filesystem::path& path, error_code& error) {
    auto absolute = filesystem::absolute(path, error);
    if (error) return {};
    auto normalized = filesystem::weakly_canonical(absolute, error);
    if (error) {
        error.clear();
        normalized = absolute.lexically_normal();
    }
    return normalized;
}

bool IsLinkOrReparsePoint(const filesystem::path& path) {
    error_code error;
    if (!filesystem::exists(path, error) || error) return false;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return filesystem::is_symlink(filesystem::symlink_status(path, error));
#endif
}

bool HasLinkedExistingAncestor(const filesystem::path& path) {
    filesystem::path cursor;
    for (const auto& component : path) {
        cursor /= component;
        error_code error;
        if (!filesystem::exists(cursor, error)) {
            if (error) return true;
            continue;
        }
        if (IsLinkOrReparsePoint(cursor)) return true;
    }
    return false;
}

bool EnsureWritableDirectory(const filesystem::path& requested, bool privateDirectory, wstring& error) {
    error_code fileError;
    const auto path = NormalizeAbsolute(requested, fileError);
    if (fileError || path.empty() || HasLinkedExistingAncestor(path)) {
        error = L"Application data path is invalid or passes through a symbolic link: " + requested.wstring();
        return false;
    }
    filesystem::create_directories(path, fileError);
    if (fileError || !filesystem::is_directory(path, fileError)) {
        error = L"Could not create application data directory: " + path.wstring();
        return false;
    }
#ifndef _WIN32
    if (privateDirectory && chmod(path.c_str(), S_IRWXU) != 0) {
        error = L"Could not restrict application runtime directory permissions: " + path.wstring();
        return false;
    }
#else
    (void)privateDirectory;
#endif
    const auto probe = path / (L".minebackup-write-probe-" + FolderRewindFormat::GenerateGuidString());
    {
        ofstream output(probe, ios::binary | ios::trunc);
        if (!output.is_open()) {
            error = L"Application data directory is not writable: " + path.wstring();
            return false;
        }
        output << "MineBackup";
        if (!output.good()) {
            error = L"Application data directory failed a write probe: " + path.wstring();
            return false;
        }
    }
    filesystem::remove(probe, fileError);
    return true;
}

bool IsSecureRuntimeBase(const filesystem::path& path) {
#ifdef _WIN32
    (void)path;
    return false;
#else
    if (path.empty() || !path.is_absolute()) return false;
    struct stat status{};
    if (stat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) return false;
    return status.st_uid == geteuid() && (status.st_mode & (S_IRWXG | S_IRWXO)) == 0;
#endif
}

filesystem::path ResourcesRootFor(const filesystem::path& executablePath) {
    const auto executableDirectory = executablePath.parent_path();
#ifdef __APPLE__
    if (executableDirectory.filename() == "MacOS"
        && executableDirectory.parent_path().filename() == "Contents") {
        return executableDirectory.parent_path() / "Resources";
    }
#endif
    error_code error;
    for (const auto& candidate : {
             executableDirectory / "Resources",
             executableDirectory.parent_path() / "share" / "MineBackup",
             executableDirectory.parent_path() / "share" / "minebackup"}) {
        if (filesystem::is_directory(candidate, error)) return candidate;
        error.clear();
    }
    return executableDirectory;
}

void SetProfileLayout(const filesystem::path& root, AppPaths& paths) {
    paths.configRoot = root / "config";
    paths.dataRoot = root / "data";
    paths.stateRoot = root / "state";
    paths.cacheRoot = root / "cache";
    paths.runtimeRoot = root / "runtime";
    paths.toolsRoot = root / "tools";
    paths.logsRoot = root / "logs";
}

bool EnsureRoots(AppPaths& paths, wstring& error) {
    for (const auto& root : {paths.configRoot, paths.dataRoot, paths.stateRoot, paths.cacheRoot,
             paths.toolsRoot, paths.logsRoot}) {
        if (root.empty() || !root.is_absolute()) {
            error = L"Application data roots must be absolute.";
            return false;
        }
        if (!EnsureWritableDirectory(root, false, error)) return false;
    }
    if (paths.runtimeRoot.empty() || !paths.runtimeRoot.is_absolute()) {
        error = L"The application runtime root must be absolute.";
        return false;
    }
    return EnsureWritableDirectory(paths.runtimeRoot, true, error);
}

} // namespace

filesystem::path GetExecutablePath() {
#ifdef _WIN32
    wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return filesystem::path(buffer);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    buffer.resize(char_traits<char>::length(buffer.c_str()));
    error_code error;
    const auto path = filesystem::path(buffer);
    const auto canonical = filesystem::weakly_canonical(path, error);
    return error ? path : canonical;
#else
    array<char, 4096> buffer{};
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return {};
    const auto path = filesystem::path(string(buffer.data(), static_cast<size_t>(length)));
    error_code error;
    const auto canonical = filesystem::weakly_canonical(path, error);
    return error ? path : canonical;
#endif
}

bool ResolveAppPaths(const LaunchOptions& options, const filesystem::path& requestedExecutablePath,
    AppPaths& paths, wstring& error) {
    paths = {};
    error.clear();
    error_code fileError;
    const auto executablePath = NormalizeAbsolute(requestedExecutablePath, fileError);
    if (fileError || executablePath.empty()) {
        error = L"Could not resolve the MineBackup executable path.";
        return false;
    }
    paths.resourcesRoot = ResourcesRootFor(executablePath);

    if (options.dataDirectory) {
        if (!options.dataDirectory->is_absolute()) {
            error = L"--data-dir must be an absolute profile directory.";
            return false;
        }
        const auto root = NormalizeAbsolute(*options.dataDirectory, fileError);
        if (fileError || root.empty()) {
            error = L"--data-dir could not be resolved.";
            return false;
        }
        SetProfileLayout(root, paths);
        paths.mode = AppPathMode::Explicit;
    }
    else {
        filesystem::path portableBase = executablePath.parent_path();
        bool portableAllowed = false;
#ifdef _WIN32
        portableAllowed = true;
#elif !defined(__APPLE__)
        const auto appImage = EnvironmentPath("APPIMAGE");
        if (!appImage.empty()) {
            portableAllowed = true;
            portableBase = appImage.parent_path();
        }
#endif
        if (portableAllowed && filesystem::is_regular_file(portableBase / "portable.flag", fileError)) {
            SetProfileLayout(portableBase / "MineBackupData", paths);
            paths.mode = AppPathMode::Portable;
        }
        else {
            const auto home = HomeDirectory();
#ifdef _WIN32
            auto root = EnvironmentPath("LOCALAPPDATA");
            if (root.empty()) root = home / "AppData" / "Local";
            SetProfileLayout(root / "MineBackup", paths);
#elif defined(__APPLE__)
            const auto support = home / "Library" / "Application Support" / "MineBackup";
            paths.configRoot = support / "config";
            paths.dataRoot = support / "data";
            paths.stateRoot = support / "state";
            paths.toolsRoot = support / "tools";
            const auto caches = home / "Library" / "Caches" / "MineBackup";
            paths.cacheRoot = caches / "cache";
            paths.runtimeRoot = caches / "runtime";
            paths.logsRoot = home / "Library" / "Logs" / "MineBackup";
#else
            auto configBase = EnvironmentPath("XDG_CONFIG_HOME");
            auto dataBase = EnvironmentPath("XDG_DATA_HOME");
            auto stateBase = EnvironmentPath("XDG_STATE_HOME");
            auto cacheBase = EnvironmentPath("XDG_CACHE_HOME");
            if (configBase.empty() || !configBase.is_absolute()) configBase = home / ".config";
            if (dataBase.empty() || !dataBase.is_absolute()) dataBase = home / ".local" / "share";
            if (stateBase.empty() || !stateBase.is_absolute()) stateBase = home / ".local" / "state";
            if (cacheBase.empty() || !cacheBase.is_absolute()) cacheBase = home / ".cache";
            paths.configRoot = configBase / "MineBackup";
            paths.dataRoot = dataBase / "MineBackup";
            paths.stateRoot = stateBase / "MineBackup";
            paths.cacheRoot = cacheBase / "MineBackup";
            paths.toolsRoot = paths.dataRoot / "tools";
            paths.logsRoot = paths.stateRoot / "logs";
            const auto runtimeBase = EnvironmentPath("XDG_RUNTIME_DIR");
            paths.runtimeRoot = IsSecureRuntimeBase(runtimeBase)
                ? runtimeBase / "MineBackup" : paths.stateRoot / "runtime";
#endif
            paths.mode = AppPathMode::Installed;
        }
    }

    if (!EnsureRoots(paths, error)) return false;
    const auto identityPath = NormalizeAbsolute(paths.configRoot, fileError);
    if (fileError || identityPath.empty()) {
        error = L"Could not establish the profile identity.";
        return false;
    }
    paths.profileIdentity = identityPath.wstring();
#ifdef _WIN32
    transform(paths.profileIdentity.begin(), paths.profileIdentity.end(), paths.profileIdentity.begin(), ::towlower);
#endif
    return true;
}

void SetCurrentAppPaths(AppPaths paths) {
    lock_guard<mutex> lock(g_pathsMutex);
    g_paths = std::move(paths);
    g_pathsConfigured = true;
}

const AppPaths& GetAppPaths() {
    lock_guard<mutex> lock(g_pathsMutex);
    if (!g_pathsConfigured) throw logic_error("AppPaths has not been configured");
    return g_paths;
}
