#include "KnotLinkServerManager.h"

#include "ProcessRunner.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winver.h>
#pragma comment(lib, "version.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace minebackup::knotlink {
namespace {

bool ParseVersion(
    std::string_view version, std::array<int, 4>& components) {
    components.fill(0);
    std::size_t start = 0;
    int count = 0;
    while (start <= version.size() && count < 4) {
        const std::size_t end = version.find('.', start);
        const std::string_view part =
            version.substr(start, end == std::string_view::npos
                                      ? version.size() - start
                                      : end - start);
        if (part.empty()) {
            return false;
        }
        int value = 0;
        const auto parsed =
            std::from_chars(part.data(), part.data() + part.size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != part.data() + part.size() || value < 0) {
            return false;
        }
        components[static_cast<std::size_t>(count++)] = value;
        if (end == std::string_view::npos) {
            return count >= 3;
        }
        start = end + 1;
    }
    return false;
}

#ifdef _WIN32

std::wstring ReadRegistryString(
    HKEY root, const wchar_t* subkey, const wchar_t* valueName,
    REGSAM view = 0) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            root, subkey, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(
            key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) {
        RegCloseKey(key);
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(
            key, valueName, nullptr, &type,
            reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    if (type == REG_EXPAND_SZ) {
        const DWORD required =
            ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (required > 0) {
            std::wstring expanded(required, L'\0');
            ExpandEnvironmentStringsW(
                value.c_str(), expanded.data(), required);
            while (!expanded.empty() && expanded.back() == L'\0') {
                expanded.pop_back();
            }
            value = std::move(expanded);
        }
    }
    return value;
}

std::string NarrowVersion(
    DWORD major, DWORD minor, DWORD build, DWORD revision) {
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(build) + "." + std::to_string(revision);
}

std::string Utf8FromWide(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required,
            nullptr, nullptr) <= 0) {
        return {};
    }
    return result;
}

std::string ReadFileVersion(const std::filesystem::path& executable) {
    DWORD ignored = 0;
    const DWORD size =
        GetFileVersionInfoSizeW(executable.c_str(), &ignored);
    if (size == 0) {
        return {};
    }
    std::vector<unsigned char> buffer(size);
    if (!GetFileVersionInfoW(
            executable.c_str(), 0, size, buffer.data())) {
        return {};
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(
            buffer.data(), L"\\",
            reinterpret_cast<void**>(&info), &infoSize) ||
        info == nullptr || infoSize < sizeof(VS_FIXEDFILEINFO)) {
        return {};
    }
    return NarrowVersion(
        HIWORD(info->dwFileVersionMS),
        LOWORD(info->dwFileVersionMS),
        HIWORD(info->dwFileVersionLS),
        LOWORD(info->dwFileVersionLS));
}

class NativeKnotLinkServerPlatform final : public IKnotLinkServerPlatform {
public:
    KnotLinkServerDiscovery Discover() override {
        KnotLinkServerDiscovery result;
        result.managedPlatform = true;

        constexpr wchar_t appPaths[] =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"
            L"KnotLinkService.exe";
        constexpr wchar_t uninstall[] =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
            L"KnotLinkService";

        std::wstring executable =
            ReadRegistryString(HKEY_LOCAL_MACHINE, appPaths, nullptr);
        if (executable.empty()) {
            executable =
                ReadRegistryString(HKEY_CURRENT_USER, appPaths, nullptr);
        }

        for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            if (result.version.empty()) {
                const std::wstring version = ReadRegistryString(
                    HKEY_LOCAL_MACHINE, uninstall, L"DisplayVersion", view);
                result.version = Utf8FromWide(version);
            }
            if (executable.empty()) {
                std::wstring location = ReadRegistryString(
                    HKEY_LOCAL_MACHINE, uninstall, L"InstallLocation", view);
                if (!location.empty()) {
                    executable =
                        (std::filesystem::path(location) /
                         L"KnotLinkService.exe").wstring();
                }
            }
        }

        if (!executable.empty() &&
            executable.front() == L'"' && executable.back() == L'"') {
            executable = executable.substr(1, executable.size() - 2);
        }
        result.executablePath = executable;
        result.installed = !executable.empty() &&
                           std::filesystem::is_regular_file(executable);
        if (result.installed && result.version.empty()) {
            result.version = ReadFileVersion(result.executablePath);
        }
        if (!result.installed) {
            result.detail = "KnotLinkService is not installed.";
        } else if (result.version.empty()) {
            result.detail = "The installed KnotLinkService version is unknown.";
        }
        return result;
    }

    bool IsProcessRunning() override {
        const HANDLE snapshot =
            CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return false;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool running = false;
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(
                        entry.szExeFile, L"KnotLinkService.exe") == 0) {
                    running = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return running;
    }

    bool IsPortReady(unsigned short port) override {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
        const SOCKET socketHandle =
            socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socketHandle == INVALID_SOCKET) {
            WSACleanup();
            return false;
        }
        DWORD timeout = 250;
        setsockopt(
            socketHandle, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool ready =
            connect(
                socketHandle,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0;
        closesocket(socketHandle);
        WSACleanup();
        return ready;
    }

    bool Start(const std::filesystem::path& executablePath) override {
        if (!std::filesystem::is_regular_file(executablePath)) {
            return false;
        }
        return reinterpret_cast<std::intptr_t>(ShellExecuteW(
                   nullptr, L"open", executablePath.c_str(), nullptr,
                   executablePath.parent_path().c_str(), SW_SHOWNORMAL)) > 32;
    }

    bool WaitForReady(
        std::chrono::milliseconds timeout,
        unsigned short signalPort,
        unsigned short responderPort) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (IsPortReady(signalPort) && IsPortReady(responderPort)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }
};

#elif defined(__APPLE__)

class NativeKnotLinkServerPlatform final : public IKnotLinkServerPlatform {
public:
    KnotLinkServerDiscovery Discover() override {
        KnotLinkServerDiscovery result;
        result.managedPlatform = true;
        result.executablePath = L"/usr/local/KnotLinkService/KnotLinkService";

        ProcessSpec spec;
        spec.executable = L"/usr/sbin/pkgutil";
        spec.arguments = {L"--pkg-info", L"com.knotlink.service"};
        spec.timeout = std::chrono::seconds(5);
        spec.maximumCapturedBytes = 64u * 1024u;
        std::error_code executableError;
        result.installed =
            std::filesystem::is_regular_file(
                result.executablePath, executableError) &&
            !executableError;
        const auto package = ProcessRunner::Run(spec);
        if (package.status == ProcessStatus::Succeeded) {
            constexpr std::string_view prefix = "version:";
            std::size_t start = 0;
            while (start < package.standardOutput.size()) {
                const std::size_t end =
                    package.standardOutput.find('\n', start);
                std::string_view line(
                    package.standardOutput.data() + start,
                    (end == std::string::npos
                         ? package.standardOutput.size()
                         : end) - start);
                if (line.starts_with(prefix)) {
                    line.remove_prefix(prefix.size());
                    while (!line.empty() &&
                           (line.front() == ' ' || line.front() == '\t')) {
                        line.remove_prefix(1);
                    }
                    while (!line.empty() &&
                           (line.back() == '\r' || line.back() == ' ')) {
                        line.remove_suffix(1);
                    }
                    result.version.assign(line);
                    break;
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
        }
        if (!result.installed) {
            result.detail = package.status == ProcessStatus::Succeeded
                ? "The KnotLinkService package is registered but its executable is missing."
                : "KnotLinkService is not installed.";
        } else if (result.version.empty()) {
            result.detail =
                "The installed KnotLinkService version is unknown.";
        }
        return result;
    }

    bool IsProcessRunning() override {
        ProcessSpec spec;
        spec.executable = L"/bin/launchctl";
        spec.arguments = {L"print", L"system/com.knotlink.service"};
        spec.timeout = std::chrono::seconds(3);
        spec.maximumCapturedBytes = 64u * 1024u;
        return ProcessRunner::Run(spec).status == ProcessStatus::Succeeded;
    }

    bool IsPortReady(unsigned short port) override {
        const int socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socketHandle < 0) {
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool ready =
            connect(
                socketHandle,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0;
        close(socketHandle);
        return ready;
    }

    bool Start(const std::filesystem::path&) override {
        ProcessSpec spec;
        spec.executable = L"/bin/launchctl";
        spec.arguments = {
            L"kickstart", L"-k", L"system/com.knotlink.service"};
        spec.timeout = std::chrono::seconds(10);
        spec.maximumCapturedBytes = 64u * 1024u;
        return ProcessRunner::Run(spec).status == ProcessStatus::Succeeded;
    }

    bool WaitForReady(
        std::chrono::milliseconds timeout,
        unsigned short signalPort,
        unsigned short responderPort) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (IsPortReady(signalPort) && IsPortReady(responderPort)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }
};

#else

class NativeKnotLinkServerPlatform final : public IKnotLinkServerPlatform {
public:
    KnotLinkServerDiscovery Discover() override {
        KnotLinkServerDiscovery result;
        result.managedPlatform = true;
        result.executablePath = L"/opt/KnotLinkService/KnotLinkService";

        ProcessSpec spec;
        spec.executable = L"/usr/bin/dpkg-query";
        spec.arguments = {
            L"-W", L"-f=${Status}\t${Version}", L"knotlinkservice"};
        spec.timeout = std::chrono::seconds(5);
        spec.maximumCapturedBytes = 64u * 1024u;
        std::error_code executableError;
        result.installed =
            std::filesystem::is_regular_file(
                result.executablePath, executableError) &&
            !executableError;
        const auto package = ProcessRunner::Run(spec);
        constexpr std::string_view installedPrefix =
            "install ok installed\t";
        if (package.status == ProcessStatus::Succeeded &&
            package.standardOutput.starts_with(installedPrefix)) {
            std::string_view version(package.standardOutput);
            version.remove_prefix(installedPrefix.size());
            while (!version.empty() &&
                   (version.back() == '\n' || version.back() == '\r' ||
                    version.back() == ' ')) {
                version.remove_suffix(1);
            }
            result.version.assign(version);
        }
        if (!result.installed) {
            result.detail =
                package.status == ProcessStatus::Succeeded
                ? "The KnotLinkService package is registered but its executable is missing."
                : "KnotLinkService is not installed.";
        } else if (result.version.empty()) {
            result.detail =
                "The installed KnotLinkService version is unknown.";
        }
        return result;
    }

    bool IsProcessRunning() override {
        ProcessSpec spec;
        spec.executable = L"/usr/bin/systemctl";
        spec.arguments = {L"is-active", L"--quiet", L"knotlink.service"};
        spec.timeout = std::chrono::seconds(3);
        spec.maximumCapturedBytes = 64u * 1024u;
        return ProcessRunner::Run(spec).status == ProcessStatus::Succeeded;
    }

    bool IsPortReady(unsigned short port) override {
        const int socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socketHandle < 0) {
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool ready =
            connect(
                socketHandle,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0;
        close(socketHandle);
        return ready;
    }

    bool Start(const std::filesystem::path&) override {
        ProcessSpec spec;
        spec.executable = L"/usr/bin/systemctl";
        spec.arguments = {L"start", L"knotlink.service"};
        spec.timeout = std::chrono::seconds(10);
        spec.maximumCapturedBytes = 64u * 1024u;
        return ProcessRunner::Run(spec).status == ProcessStatus::Succeeded;
    }

    bool WaitForReady(
        std::chrono::milliseconds timeout,
        unsigned short signalPort,
        unsigned short responderPort) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (IsPortReady(signalPort) && IsPortReady(responderPort)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }
};

#endif

} // namespace

KnotLinkServerManager::KnotLinkServerManager(
    std::shared_ptr<IKnotLinkServerPlatform> platform)
    : platform_(std::move(platform)) {}

KnotLinkServerStatus KnotLinkServerManager::Refresh(
    bool integrationEnabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    return RefreshLocked(integrationEnabled);
}

KnotLinkServerStatus KnotLinkServerManager::EnsureReady(
    bool integrationEnabled, bool autoStart) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RefreshLocked(integrationEnabled);
        if (status_.state != KnotLinkServerState::Stopped || !autoStart) {
            return status_;
        }
    }
    return StartCompatibleServer();
}

KnotLinkServerStatus KnotLinkServerManager::StartCompatibleServer() {
    std::filesystem::path executablePath;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RefreshLocked(true);
        if (status_.state == KnotLinkServerState::Ready) {
            return status_;
        }
        if (status_.state != KnotLinkServerState::Stopped) {
            return status_;
        }
        status_.state = KnotLinkServerState::Starting;
        status_.message = "Starting KnotLink server...";
        executablePath = status_.executablePath;
    }

    if (!platform_->Start(executablePath)) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = KnotLinkServerState::Failed;
        status_.message = "KnotLink server could not be started.";
        return status_;
    }
    const bool ready = platform_->WaitForReady(
        std::chrono::seconds(10), SignalPort, ResponderPort);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready) {
        status_.state = KnotLinkServerState::Failed;
        status_.processRunning = platform_->IsProcessRunning();
        status_.signalPortReady = platform_->IsPortReady(SignalPort);
        status_.responderPortReady = platform_->IsPortReady(ResponderPort);
        status_.message =
            "KnotLink server did not become ready within 10 seconds.";
        return status_;
    }
    status_.state = KnotLinkServerState::Ready;
    status_.processRunning = true;
    status_.signalPortReady = true;
    status_.responderPortReady = true;
    status_.message = "KnotLink server is ready.";
    return status_;
}

KnotLinkServerStatus KnotLinkServerManager::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool KnotLinkServerManager::IsVersionCompatible(
    std::string_view version) {
    std::array<int, 4> actual{};
    std::array<int, 4> required{};
    return ParseVersion(version, actual) &&
           ParseVersion(MinimumVersion, required) &&
           actual >= required;
}

const char* KnotLinkServerManager::StateName(KnotLinkServerState state) {
    switch (state) {
        case KnotLinkServerState::Disabled: return "Disabled";
        case KnotLinkServerState::NotInstalled: return "Not installed";
        case KnotLinkServerState::Incompatible: return "Incompatible";
        case KnotLinkServerState::Stopped: return "Stopped";
        case KnotLinkServerState::Starting: return "Starting";
        case KnotLinkServerState::Ready: return "Ready";
        case KnotLinkServerState::Failed: return "Failed";
    }
    return "Unknown";
}

KnotLinkServerStatus KnotLinkServerManager::RefreshLocked(
    bool integrationEnabled) {
    status_ = {};
    if (!integrationEnabled) {
        status_.state = KnotLinkServerState::Disabled;
        status_.message = "KnotLink integration is disabled.";
        return status_;
    }

    const KnotLinkServerDiscovery discovery = platform_->Discover();
    status_.managedPlatform = discovery.managedPlatform;
    status_.installed = discovery.installed;
    status_.executablePath = discovery.executablePath;
    status_.version = discovery.version;
    status_.processRunning = platform_->IsProcessRunning();
    status_.signalPortReady = platform_->IsPortReady(SignalPort);
    status_.responderPortReady = platform_->IsPortReady(ResponderPort);

    if (!discovery.managedPlatform) {
        if (status_.signalPortReady && status_.responderPortReady) {
            status_.state = KnotLinkServerState::Ready;
            status_.message = "An external KnotLink server is ready.";
        } else {
            status_.state = KnotLinkServerState::NotInstalled;
            status_.message = discovery.detail;
        }
        return status_;
    }
    if (!discovery.installed) {
        status_.state = KnotLinkServerState::NotInstalled;
        status_.message = discovery.detail;
        return status_;
    }
    if (!IsVersionCompatible(discovery.version)) {
        status_.state = KnotLinkServerState::Incompatible;
        status_.message = discovery.version.empty()
            ? "The KnotLink server version is unknown; connection was blocked."
            : "KnotLinkService 3.2.0.0 or newer is required.";
        return status_;
    }
    if (status_.signalPortReady && status_.responderPortReady) {
        status_.state = KnotLinkServerState::Ready;
        status_.message = "KnotLink server is ready.";
    } else {
        status_.state = KnotLinkServerState::Stopped;
        status_.message = status_.processRunning
            ? "KnotLink process is running but required ports are not ready."
            : "KnotLink server is installed and stopped.";
    }
    return status_;
}

std::shared_ptr<IKnotLinkServerPlatform> CreateKnotLinkServerPlatform() {
    return std::make_shared<NativeKnotLinkServerPlatform>();
}

KnotLinkServerManager& GetKnotLinkServerManager() {
    static KnotLinkServerManager manager(CreateKnotLinkServerPlatform());
    return manager;
}

} // namespace minebackup::knotlink
