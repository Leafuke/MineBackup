#include "KnownUserFolders.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <shlobj.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <limits.h>
#endif

namespace {

using Path = std::filesystem::path;
using KnownUserFolders::EnvironmentReader;
using KnownUserFolders::FileReader;
using KnownUserFolders::Platform;

constexpr std::size_t MaximumUserDirsFileBytes = 64 * 1024;

std::optional<std::string> ReadEnvironment(std::string_view name) {
    const std::string variable(name);
    const char* value = std::getenv(variable.c_str());
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string(value);
}

std::optional<std::string> ReadTextFile(const Path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return std::nullopt;

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return std::nullopt;
    std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (contents.size() > MaximumUserDirsFileBytes) return std::nullopt;
    return contents;
}

std::optional<Path> ResolveWindowsDocuments() {
#ifdef _WIN32
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &value))) {
        Path result(value);
        CoTaskMemFree(value);
        return result;
    }
#endif
    return std::nullopt;
}

std::optional<Path> ResolveMacDocuments() {
#ifdef __APPLE__
    FSRef documents{};
    UInt8 buffer[PATH_MAX]{};
    // 优先询问系统已本地化/重定向的 Documents 位置，再由上层回退到 HOME。
    if (FSFindFolder(kUserDomain, kDocumentsFolderType, kDontCreateFolder, &documents)
            == noErr
        && FSRefMakePath(&documents, buffer, sizeof(buffer)) == noErr) {
        return Path(reinterpret_cast<const char*>(buffer));
    }
#endif
    return std::nullopt;
}

bool IsAbsolute(const Path& path) {
    return !path.empty() && path.is_absolute();
}

std::string Trim(std::string value) {
    const auto notSpace = [](unsigned char character) {
        return character != ' ' && character != '\t' && character != '\r' && character != '\n';
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::optional<std::string> Unquote(std::string value) {
    value = Trim(std::move(value));
    if (value.empty()) return std::nullopt;
    if (value.front() != '"') return value;
    if (value.size() < 2 || value.back() != '"') return std::nullopt;

    std::string result;
    result.reserve(value.size() - 2);
    for (std::size_t index = 1; index + 1 < value.size(); ++index) {
        if (value[index] == '\\' && index + 2 < value.size()) {
            ++index;
        }
        result.push_back(value[index]);
    }
    return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
}

std::optional<std::string> EnvironmentValue(
    const EnvironmentReader& readEnvironment,
    std::string_view name) {
    if (!readEnvironment) return std::nullopt;
    return readEnvironment(name);
}

std::optional<std::string> ExpandHome(
    std::string value,
    const EnvironmentReader& readEnvironment) {
    value = Trim(std::move(value));
    if (value.empty()) return std::nullopt;

    const auto home = EnvironmentValue(readEnvironment, "HOME");
    const auto replaceToken = [&](std::string_view token) {
        if (!home || home->empty()) return false;
        const auto position = value.find(token);
        if (position == std::string::npos) return false;
        value.replace(position, token.size(), *home);
        return true;
    };

    if (value == "~") {
        if (!home || home->empty()) return std::nullopt;
        value = *home;
    }
    else if (value.rfind("~/", 0) == 0) {
        if (!home || home->empty()) return std::nullopt;
        value.replace(0, 1, *home);
    }
    else {
        replaceToken("${HOME}");
        replaceToken("$HOME");
    }
    return value;
}

std::optional<Path> AbsoluteCandidate(
    std::string value,
    const EnvironmentReader& readEnvironment) {
    auto expanded = ExpandHome(std::move(value), readEnvironment);
    if (!expanded) return std::nullopt;
    try {
        Path path(*expanded);
        return IsAbsolute(path) ? std::optional<Path>(std::move(path)) : std::nullopt;
    }
    catch (const std::filesystem::filesystem_error&) {
        return std::nullopt;
    }
}

std::optional<Path> ReadLinuxUserDirsDocuments(
    const EnvironmentReader& readEnvironment,
    const FileReader& readFile) {
    if (!readFile) return std::nullopt;

    auto configHome = EnvironmentValue(readEnvironment, "XDG_CONFIG_HOME");
    Path configDirectory;
    if (configHome) {
        const auto candidate = AbsoluteCandidate(*configHome, readEnvironment);
        if (candidate) configDirectory = *candidate;
    }
    if (configDirectory.empty()) {
        const auto home = EnvironmentValue(readEnvironment, "HOME");
        const auto homePath = home ? AbsoluteCandidate(*home, readEnvironment) : std::nullopt;
        if (!homePath) return std::nullopt;
        configDirectory = *homePath / L".config";
    }

    const auto contents = readFile(configDirectory / L"user-dirs.dirs");
    if (!contents || contents->size() > MaximumUserDirsFileBytes) return std::nullopt;
    constexpr std::string_view key = "XDG_DOCUMENTS_DIR=";
    std::size_t lineStart = 0;
    while (lineStart <= contents->size()) {
        const auto lineEnd = contents->find('\n', lineStart);
        const auto line = contents->substr(
            lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
        const auto trimmed = Trim(line);
        if (trimmed.rfind(key, 0) == 0) {
            const auto value = Unquote(trimmed.substr(key.size()));
            if (value) {
                const auto candidate = AbsoluteCandidate(*value, readEnvironment);
                if (candidate) return candidate;
            }
        }
        if (lineEnd == std::string::npos) break;
        lineStart = lineEnd + 1;
    }
    return std::nullopt;
}

std::optional<Path> ResolveLinuxDocuments(
    const EnvironmentReader& readEnvironment,
    const FileReader& readFile) {
    if (const auto direct = EnvironmentValue(readEnvironment, "XDG_DOCUMENTS_DIR")) {
        if (const auto candidate = AbsoluteCandidate(*direct, readEnvironment)) return candidate;
    }
    if (const auto configured = ReadLinuxUserDirsDocuments(readEnvironment, readFile)) {
        return configured;
    }
    if (const auto home = EnvironmentValue(readEnvironment, "HOME")) {
        if (const auto homePath = AbsoluteCandidate(*home, readEnvironment)) {
            // Linux 没有统一的 Documents API，user-dirs 配置失败时回退到 HOME/Documents。
            return *homePath / L"Documents";
        }
    }
    return std::nullopt;
}

Platform DetectPlatform() noexcept {
#ifdef _WIN32
    return Platform::Windows;
#elif defined(__APPLE__)
    return Platform::MacOS;
#else
    return Platform::Linux;
#endif
}

KnownUserFolders::Dependencies ProductionDependencies() {
    KnownUserFolders::Dependencies dependencies;
    dependencies.platform = DetectPlatform();
    dependencies.readEnvironment = ReadEnvironment;
    dependencies.readFile = ReadTextFile;
    dependencies.resolveWindowsDocuments = ResolveWindowsDocuments;
    dependencies.resolveMacDocuments = ResolveMacDocuments;
    return dependencies;
}

} // namespace

namespace KnownUserFolders {

Resolver::Resolver(Dependencies dependencies)
    : dependencies_(std::move(dependencies)) {
    const auto production = ProductionDependencies();
    if (dependencies_.platform == Platform::Current) dependencies_.platform = production.platform;
    if (!dependencies_.readEnvironment) dependencies_.readEnvironment = production.readEnvironment;
    if (!dependencies_.readFile) dependencies_.readFile = production.readFile;
    if (!dependencies_.resolveWindowsDocuments) {
        dependencies_.resolveWindowsDocuments = production.resolveWindowsDocuments;
    }
    if (!dependencies_.resolveMacDocuments) {
        dependencies_.resolveMacDocuments = production.resolveMacDocuments;
    }
}

std::optional<std::filesystem::path> Resolver::ResolveDocuments() const {
    switch (dependencies_.platform) {
    case Platform::Current:
        return std::nullopt;
    case Platform::Windows:
        if (dependencies_.resolveWindowsDocuments) {
            const auto result = dependencies_.resolveWindowsDocuments();
            if (result && IsAbsolute(*result)) return result;
        }
        return std::nullopt;
    case Platform::Linux:
        return ResolveLinuxDocuments(dependencies_.readEnvironment, dependencies_.readFile);
    case Platform::MacOS:
        if (dependencies_.resolveMacDocuments) {
            const auto result = dependencies_.resolveMacDocuments();
            if (result && IsAbsolute(*result)) return result;
        }
        if (const auto home = EnvironmentValue(dependencies_.readEnvironment, "HOME")) {
            if (const auto homePath = AbsoluteCandidate(*home, dependencies_.readEnvironment)) {
                // macOS 系统目录解析不可用时，使用约定的 HOME/Documents。
                return *homePath / L"Documents";
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::filesystem::path Resolver::ResolveRecommendedBackupRoot(const AppPaths& appPaths) const {
    if (const auto documents = ResolveDocuments()) {
        return *documents / RecommendedBackupDirectoryName;
    }
    // Documents 和环境变量都不可用时，只允许回退到已解析的应用数据根目录。
    if (IsAbsolute(appPaths.dataRoot)) return appPaths.dataRoot / L"backups";
    return {};
}

Platform Resolver::CurrentPlatform() noexcept {
    return DetectPlatform();
}

} // namespace KnownUserFolders
