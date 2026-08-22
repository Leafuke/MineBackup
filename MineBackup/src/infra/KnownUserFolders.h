#pragma once

#include "AppPaths.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace KnownUserFolders {

enum class Platform {
    Current,
    Windows,
    Linux,
    MacOS
};

using EnvironmentReader = std::function<std::optional<std::string>(std::string_view)>;
using FileReader = std::function<std::optional<std::string>(const std::filesystem::path&)>;
using DocumentsResolver = std::function<std::optional<std::filesystem::path>()>;

struct Dependencies {
    Platform platform = Platform::Current;
    EnvironmentReader readEnvironment;
    FileReader readFile;
    DocumentsResolver resolveWindowsDocuments;
    DocumentsResolver resolveMacDocuments;
};

class Resolver {
public:
    explicit Resolver(Dependencies dependencies = {});

    std::optional<std::filesystem::path> ResolveDocuments() const;

    // 返回推荐的 MineBackup 备份根目录；无法解析 Documents 时使用 dataRoot/backups。
    std::filesystem::path ResolveRecommendedBackupRoot(const AppPaths& appPaths) const;

    static Platform CurrentPlatform() noexcept;

private:
    Dependencies dependencies_;
};

inline constexpr std::wstring_view RecommendedBackupDirectoryName = L"MineBackup-Backups";

} // namespace KnownUserFolders
