#include "ExternalToolManager.h"

#include "AtomicFileWriter.h"
#include "NetworkService.h"
#include "ProcessRunner.h"
#include "Sha256.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;

namespace {

struct RcloneManifest {
    const char* url;
    const char* sha256;
    const wchar_t* archiveDirectory;
    const wchar_t* executableName;
};

RcloneManifest CurrentRcloneManifest() {
#ifdef _WIN32
    return {
        "https://downloads.rclone.org/v1.74.4/rclone-v1.74.4-windows-amd64.zip",
        "ef097ef9de37a57feb7d9f9c7afb34148ad3c65be8025f1d8f7f521554a701ea",
        L"rclone-v1.74.4-windows-amd64",
        L"rclone.exe"};
#elif defined(__APPLE__)
    return {
        "https://downloads.rclone.org/v1.74.4/rclone-v1.74.4-osx-arm64.zip",
        "c2100e2d4a4b3be04c55cd45380cafe7647e1ad772bb055f52f00876ed701167",
        L"rclone-v1.74.4-osx-arm64",
        L"rclone"};
#else
    return {
        "https://downloads.rclone.org/v1.74.4/rclone-v1.74.4-linux-amd64.zip",
        "fe435e0c36228e7c2f116a8701f01127bb1f694005fc11d1f27186c8bca4115d",
        L"rclone-v1.74.4-linux-amd64",
        L"rclone"};
#endif
}

wstring Utf8ToWideLossy(const string& text) {
    if (text.empty()) return {};
#ifdef _WIN32
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return wstring(text.begin(), text.end());
    wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
#else
    return wstring(text.begin(), text.end());
#endif
}

string LowerAscii(string text) {
    transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        return static_cast<char>(tolower(value));
    });
    return text;
}

string NarrowAscii(const wstring& text) {
    string result;
    result.reserve(text.size());
    for (const wchar_t value : text) {
        if (value < 0 || value > 0x7f) return {};
        result.push_back(static_cast<char>(value));
    }
    return result;
}

bool IsAbsoluteRegularFile(const filesystem::path& path) {
    if (path.empty() || !path.is_absolute()) return false;
    error_code error;
    return filesystem::is_regular_file(path, error) && !error;
}

ExternalToolProbe FailedProbe(const filesystem::path& executable, wstring diagnostic) {
    ExternalToolProbe result;
    result.executable = executable;
    result.diagnostic = std::move(diagnostic);
    return result;
}

vector<filesystem::path> SevenZipBundledCandidates(const AppPaths& paths) {
#ifdef _WIN32
    return {paths.toolsRoot / L"7zip" / ExternalToolManager::SevenZipVersion / L"7za.exe"};
#elif defined(__APPLE__)
    return {
        paths.resourcesRoot / L"tools" / L"7zip" / ExternalToolManager::SevenZipVersion / L"7zz",
        paths.resourcesRoot / L"7zz"};
#else
    return {
        paths.resourcesRoot / L"tools" / L"7zip" / ExternalToolManager::SevenZipVersion / L"7zz",
        paths.resourcesRoot / L"usr" / L"lib" / L"minebackup" / L"7zz",
        paths.resourcesRoot / L"7zz"};
#endif
}

vector<filesystem::path> SevenZipSystemCandidates() {
#ifdef _WIN32
    vector<filesystem::path> result;
    if (const wchar_t* programFiles = _wgetenv(L"ProgramFiles")) {
        result.emplace_back(filesystem::path(programFiles) / L"7-Zip" / L"7z.exe");
    }
    return result;
#elif defined(__APPLE__)
    return {L"/opt/homebrew/bin/7zz", L"/usr/local/bin/7zz", L"/opt/local/bin/7zz", L"/usr/bin/7z"};
#else
    return {L"/usr/bin/7zz", L"/usr/bin/7z", L"/usr/local/bin/7zz", L"/usr/local/bin/7z"};
#endif
}

vector<filesystem::path> RcloneSystemCandidates() {
#ifdef _WIN32
    vector<filesystem::path> result;
    const wchar_t* pathValue = _wgetenv(L"PATH");
    if (!pathValue) return result;
    wstringstream stream(pathValue);
    wstring segment;
    while (getline(stream, segment, L';')) {
        if (!segment.empty()) result.emplace_back(filesystem::path(segment) / L"rclone.exe");
    }
    return result;
#elif defined(__APPLE__)
    return {L"/opt/homebrew/bin/rclone", L"/usr/local/bin/rclone", L"/opt/local/bin/rclone", L"/usr/bin/rclone"};
#else
    return {L"/usr/bin/rclone", L"/usr/local/bin/rclone", L"/snap/bin/rclone"};
#endif
}

filesystem::path ManagedRcloneExecutable(const AppPaths& paths) {
    const auto manifest = CurrentRcloneManifest();
    return paths.toolsRoot / L"rclone" / L"versions" / ExternalToolManager::RcloneVersion / manifest.executableName;
}

ExternalToolResolution ResolveFromCandidates(
    const filesystem::path& userPath,
    const vector<pair<ExternalToolSource, filesystem::path>>& candidates,
    bool sevenZip,
    stop_token stopToken) {
    ExternalToolResolution result;
    result.fellBackFromUserPath = !userPath.empty();
    wstring firstFailure;
    for (const auto& [source, candidate] : candidates) {
        if (stopToken.stop_requested()) {
            result.diagnostic = L"Tool resolution was cancelled.";
            return result;
        }
        if (!IsAbsoluteRegularFile(candidate)) {
            if (source == ExternalToolSource::User && !userPath.empty()) {
                firstFailure = L"The configured tool path is not an existing absolute file.";
            }
            continue;
        }
        auto probe = sevenZip
            ? ExternalToolManager::ProbeSevenZip(candidate, stopToken)
            : ExternalToolManager::ProbeRclone(
                candidate,
                source == ExternalToolSource::Managed ? ExternalToolManager::RcloneVersion : L"",
                stopToken);
        if (!probe.available) {
            if (firstFailure.empty()) firstFailure = probe.diagnostic;
            continue;
        }
        static_cast<ExternalToolProbe&>(result) = std::move(probe);
        result.source = source;
        result.fellBackFromUserPath = !userPath.empty() && source != ExternalToolSource::User;
        if (result.fellBackFromUserPath && !firstFailure.empty()) {
            result.diagnostic = firstFailure + L" MineBackup selected a verified fallback.";
        }
        return result;
    }
    result.diagnostic = firstFailure.empty() ? L"No verified tool installation is available." : firstFailure;
    return result;
}

wstring UniqueTransactionId() {
    return to_wstring(chrono::steady_clock::now().time_since_epoch().count());
}

class ScopedDirectoryCleanup {
public:
    explicit ScopedDirectoryCleanup(filesystem::path path) : path_(std::move(path)) {}
    ~ScopedDirectoryCleanup() {
        error_code ignored;
        filesystem::remove_all(path_, ignored);
    }
private:
    filesystem::path path_;
};

} // namespace

namespace ExternalToolManager {

ExternalToolProbe ProbeSevenZip(const filesystem::path& executable, stop_token stopToken) {
    if (!IsAbsoluteRegularFile(executable)) {
        return FailedProbe(executable, L"7-Zip must be an existing absolute executable path.");
    }
    ProcessSpec spec;
    spec.executable = executable;
    spec.arguments = {L"i"};
    spec.timeout = chrono::seconds(15);
    spec.maximumCapturedBytes = 2u * 1024u * 1024u;
    const auto process = ProcessRunner::Run(spec, stopToken);
    if (process.status != ProcessStatus::Succeeded) {
        return FailedProbe(executable, process.error.empty() ? L"7-Zip capability probe failed." : process.error);
    }
    const string output = LowerAscii(process.standardOutput + "\n" + process.standardError);
    const pair<const char*, const wchar_t*> requirements[] = {
        {"7z", L"7z format"}, {"zip", L"ZIP format"}, {"lzma2", L"LZMA2"},
        {"deflate", L"Deflate"}, {"bzip2", L"BZip2"}, {"zstd", L"zstd"}};
    for (const auto& [needle, label] : requirements) {
        if (output.find(needle) == string::npos) {
            return FailedProbe(executable, wstring(L"7-Zip is missing required capability: ") + label + L".");
        }
    }
    ExternalToolProbe result;
    result.available = true;
    result.executable = executable;
    const auto lineEnd = output.find('\n');
    result.version = Utf8ToWideLossy(process.standardOutput.substr(0, lineEnd));
    return result;
}

ExternalToolProbe ProbeRclone(const filesystem::path& executable, const wstring& requiredVersion, stop_token stopToken) {
    if (!IsAbsoluteRegularFile(executable)) {
        return FailedProbe(executable, L"rclone must be an existing absolute executable path.");
    }
    ProcessSpec spec;
    spec.executable = executable;
    spec.arguments = {L"version"};
    spec.timeout = chrono::seconds(15);
    spec.maximumCapturedBytes = 512u * 1024u;
    const auto process = ProcessRunner::Run(spec, stopToken);
    if (process.status != ProcessStatus::Succeeded) {
        return FailedProbe(executable, process.error.empty() ? L"rclone version probe failed." : process.error);
    }
    const string output = process.standardOutput + "\n" + process.standardError;
    const string required = NarrowAscii(requiredVersion);
    if (output.find("rclone v") == string::npos || (!required.empty() && output.find("rclone v" + required) == string::npos)) {
        return FailedProbe(executable, requiredVersion.empty()
            ? L"The executable did not identify itself as rclone."
            : L"The downloaded rclone version does not match the pinned manifest.");
    }
    ExternalToolProbe result;
    result.available = true;
    result.executable = executable;
    const auto lineEnd = output.find('\n');
    result.version = Utf8ToWideLossy(output.substr(0, lineEnd));
    return result;
}

ExternalToolResolution ResolveSevenZip(const filesystem::path& userPath, const AppPaths& paths, stop_token stopToken) {
    vector<pair<ExternalToolSource, filesystem::path>> candidates;
    if (!userPath.empty()) candidates.emplace_back(ExternalToolSource::User, userPath);
    for (const auto& path : SevenZipBundledCandidates(paths)) candidates.emplace_back(ExternalToolSource::Bundled, path);
    for (const auto& path : SevenZipSystemCandidates()) candidates.emplace_back(ExternalToolSource::System, path);
    return ResolveFromCandidates(userPath, candidates, true, stopToken);
}

ExternalToolResolution ResolveRclone(const filesystem::path& userPath, const AppPaths& paths, stop_token stopToken) {
    vector<pair<ExternalToolSource, filesystem::path>> candidates;
    if (!userPath.empty()) candidates.emplace_back(ExternalToolSource::User, userPath);
    candidates.emplace_back(ExternalToolSource::Managed, ManagedRcloneExecutable(paths));
    for (const auto& path : RcloneSystemCandidates()) candidates.emplace_back(ExternalToolSource::System, path);
    return ResolveFromCandidates(userPath, candidates, false, stopToken);
}

ManagedToolInstallResult InstallBundledSevenZipForWindows(
    const void* data, size_t size, const AppPaths& paths, stop_token stopToken) {
    ManagedToolInstallResult result;
    if (!data || size == 0 || stopToken.stop_requested()) {
        result.error = L"The embedded 7-Zip resource is unavailable or installation was cancelled.";
        return result;
    }
    Sha256 embeddedHash;
    embeddedHash.Update(data, size);
    if (embeddedHash.FinalHex() != SevenZipWindowsSha256) {
        result.error = L"The embedded 7-Zip resource does not match the pinned SHA-256.";
        return result;
    }
    const filesystem::path target = paths.toolsRoot / L"7zip" / SevenZipVersion / L"7za.exe";
    string existingHash;
    wstring hashError;
    bool needsWrite = !Sha256::FileHex(target, existingHash, hashError) || existingHash != SevenZipWindowsSha256;
    if (needsWrite) {
        AtomicFileWriter::WriteOptions options;
        options.keepBackup = false;
        const string content(static_cast<const char*>(data), size);
        const auto write = AtomicFileWriter::WriteText(target, content, options);
        if (!write.success) {
            result.error = write.error;
            return result;
        }
    }
    const auto probe = ProbeSevenZip(target, stopToken);
    if (!probe.available) {
        result.error = probe.diagnostic;
        return result;
    }
    result.success = true;
    result.executable = target;
    return result;
}

ManagedToolInstallResult InstallPinnedRclone(
    const NetworkService& network,
    const filesystem::path& sevenZipExecutable,
    const AppPaths& paths,
    stop_token stopToken) {
    ManagedToolInstallResult result;
    const auto sevenZip = ProbeSevenZip(sevenZipExecutable, stopToken);
    if (!sevenZip.available) {
        result.error = L"A verified 7-Zip installation is required to install rclone. " + sevenZip.diagnostic;
        return result;
    }

    const auto manifest = CurrentRcloneManifest();
    const filesystem::path finalExecutable = ManagedRcloneExecutable(paths);
    const auto existing = ProbeRclone(finalExecutable, RcloneVersion, stopToken);
    if (existing.available) {
        result.success = true;
        result.executable = finalExecutable;
        return result;
    }

    const filesystem::path rcloneRoot = paths.toolsRoot / L"rclone";
    const filesystem::path staging = rcloneRoot / (L".staging-" + UniqueTransactionId());
    ScopedDirectoryCleanup cleanup(staging);
    error_code error;
    filesystem::create_directories(staging, error);
    if (error) {
        result.error = L"Unable to create the rclone staging directory: " + Utf8ToWideLossy(error.message());
        return result;
    }

    NetworkRequest request;
    request.url = manifest.url;
    request.totalTimeout = chrono::minutes(5);
    const filesystem::path archive = staging / L"rclone.zip";
    const auto download = network.Download(request, archive, manifest.sha256, stopToken);
    if (download.status != NetworkStatus::Succeeded) {
        result.error = download.error.empty() ? L"The pinned rclone download failed verification." : download.error;
        return result;
    }

    const filesystem::path extracted = staging / L"extracted";
    ProcessSpec extraction;
    extraction.executable = sevenZipExecutable;
    extraction.arguments = {L"x", archive.wstring(), L"-o" + extracted.wstring(), L"-y"};
    extraction.timeout = chrono::minutes(2);
    const auto extractedResult = ProcessRunner::Run(extraction, stopToken);
    if (extractedResult.status != ProcessStatus::Succeeded) {
        result.error = extractedResult.error.empty() ? L"Unable to extract the verified rclone archive." : extractedResult.error;
        return result;
    }

    const filesystem::path stagedExecutable = extracted / manifest.archiveDirectory / manifest.executableName;
    const auto probe = ProbeRclone(stagedExecutable, RcloneVersion, stopToken);
    if (!probe.available) {
        result.error = probe.diagnostic;
        return result;
    }

    const filesystem::path finalDirectory = finalExecutable.parent_path();
    error.clear();
    if (filesystem::exists(finalDirectory, error)) {
        result.error = L"An invalid managed rclone version directory already exists; it was left unchanged.";
        return result;
    }
    if (error) {
        result.error = L"Unable to inspect the managed rclone version directory: " + Utf8ToWideLossy(error.message());
        return result;
    }
    filesystem::create_directories(finalDirectory.parent_path(), error);
    if (error) {
        result.error = L"Unable to create the managed rclone versions directory: " + Utf8ToWideLossy(error.message());
        return result;
    }
    filesystem::rename(stagedExecutable.parent_path(), finalDirectory, error);
    if (error) {
        result.error = L"Unable to atomically activate the verified rclone version: " + Utf8ToWideLossy(error.message());
        return result;
    }
    const auto activated = ProbeRclone(finalExecutable, RcloneVersion, stopToken);
    if (!activated.available) {
        result.error = L"The activated managed rclone failed its final version probe.";
        return result;
    }
    result.success = true;
    result.executable = finalExecutable;
    return result;
}

} // namespace ExternalToolManager
