#pragma once

#include <filesystem>
#include <stop_token>
#include <string>

class NetworkService;
struct AppPaths;

namespace minebackup::knotlink {

struct KnotLinkPackageInfo {
    bool supported = false;
    std::string version;
    std::string fileName;
    std::string officialUrl;
    std::string mirrorUrl;
    std::string detail;
};

struct KnotLinkPackageInstallResult {
    bool success = false;
    std::filesystem::path packagePath;
    std::string sourceUrl;
    std::wstring error;
};

const KnotLinkPackageInfo& CurrentKnotLinkPackage();

KnotLinkPackageInstallResult DownloadAndOpenCurrentKnotLinkPackage(
    const NetworkService& network,
    const AppPaths& paths,
    std::stop_token stopToken = {});

bool OpenKnotLinkPackage(
    const std::filesystem::path& packagePath,
    std::wstring& error);

} // namespace minebackup::knotlink
