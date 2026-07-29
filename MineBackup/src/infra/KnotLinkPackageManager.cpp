#include "KnotLinkPackageManager.h"

#include "AppPaths.h"
#include "NetworkService.h"
#include "ProcessRunner.h"

#include <chrono>
#include <cstdint>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace minebackup::knotlink {
namespace {

constexpr const char* CurrentVersion = "3.2.0.0";
constexpr const char* ReleaseRoot =
    "https://github.com/KnotLink-Protocol/KnotLinkService/releases/download/"
    "v3.2.0.0/";
constexpr const char* MirrorPrefix = "https://gh-proxy.org/";

KnotLinkPackageInfo BuildPackageInfo() {
    KnotLinkPackageInfo result;
    result.version = CurrentVersion;
#ifdef _WIN32
    result.supported = true;
    result.fileName =
        "KnotLinkService-3.2.0.0-windows-x86-Installer.exe";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    result.supported = true;
    result.fileName = "KnotLinkService-3.2.0.0-macos-arm64.pkg";
#elif defined(__linux__) && defined(__x86_64__)
    result.supported = true;
    result.fileName = "KnotLinkService-3.2.0.0-linux-amd64.deb";
#else
    result.detail =
        "KnotLinkService 3.2.0.0 has no package for this platform architecture.";
    return result;
#endif
    result.officialUrl = std::string(ReleaseRoot) + result.fileName;
    result.mirrorUrl = std::string(MirrorPrefix) + result.officialUrl;
    return result;
}

std::wstring DownloadFailure(
    const NetworkDownloadResult& result,
    const wchar_t* fallback) {
    return result.error.empty() ? std::wstring(fallback) : result.error;
}

} // namespace

const KnotLinkPackageInfo& CurrentKnotLinkPackage() {
    static const KnotLinkPackageInfo package = BuildPackageInfo();
    return package;
}

KnotLinkPackageInstallResult DownloadAndOpenCurrentKnotLinkPackage(
    const NetworkService& network,
    const AppPaths& paths,
    std::stop_token stopToken) {
    KnotLinkPackageInstallResult output;
    const auto& package = CurrentKnotLinkPackage();
    if (!package.supported) {
        output.error = std::wstring(package.detail.begin(), package.detail.end());
        return output;
    }

    const std::filesystem::path destination =
        paths.cacheRoot / L"KnotLinkService" /
        std::filesystem::path(package.version) /
        std::filesystem::path(package.fileName);
    NetworkDownloadResult lastDownload;
    for (const auto& candidate :
         std::vector<std::string>{package.officialUrl, package.mirrorUrl}) {
        if (stopToken.stop_requested()) {
            output.error = L"KnotLinkService download was cancelled.";
            return output;
        }
        NetworkRequest request;
        request.url = candidate;
        request.userAgent = "MineBackup KnotLink Installer/1.16";
        request.totalTimeout = std::chrono::minutes(3);
        lastDownload = network.Download(
            request, destination, {}, stopToken);
        if (lastDownload.status == NetworkStatus::Succeeded) {
            output.sourceUrl = candidate;
            break;
        }
    }
    if (lastDownload.status != NetworkStatus::Succeeded) {
        output.error = DownloadFailure(
            lastDownload, L"Unable to download the KnotLinkService installer.");
        return output;
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(destination, error);
    if (error || size == 0 ||
        size > NetworkService::MaximumDownloadBytes) {
        output.error =
            L"The downloaded KnotLinkService installer has an invalid size.";
        return output;
    }

    if (!OpenKnotLinkPackage(destination, output.error)) {
        return output;
    }
    output.success = true;
    output.packagePath = destination;
    return output;
}

bool OpenKnotLinkPackage(
    const std::filesystem::path& packagePath,
    std::wstring& error) {
    std::error_code filesystemError;
    if (!packagePath.is_absolute() ||
        !std::filesystem::is_regular_file(packagePath, filesystemError) ||
        filesystemError) {
        error = L"The KnotLinkService installer file is unavailable.";
        return false;
    }
#ifdef _WIN32
    const auto result = reinterpret_cast<std::intptr_t>(ShellExecuteW(
        nullptr, L"open", packagePath.c_str(), nullptr,
        packagePath.parent_path().c_str(), SW_SHOWNORMAL));
    if (result <= 32) {
        error = L"Windows could not open the KnotLinkService installer.";
        return false;
    }
    return true;
#else
    ProcessSpec spec;
#ifdef __APPLE__
    spec.executable = L"/usr/bin/open";
#else
    spec.executable = L"/usr/bin/xdg-open";
#endif
    spec.arguments = {packagePath.wstring()};
    spec.timeout = std::chrono::seconds(30);
    spec.maximumCapturedBytes = 256u * 1024u;
    const auto result = ProcessRunner::Run(spec);
    if (result.status != ProcessStatus::Succeeded) {
        error = result.error.empty()
            ? L"The system package installer could not be opened."
            : result.error;
        return false;
    }
    return true;
#endif
}

} // namespace minebackup::knotlink
