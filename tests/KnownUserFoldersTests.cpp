#include "KnownUserFolders.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>

namespace {

struct CheckContext {
    int failures = 0;

    void Expect(bool condition, const char* message) {
        if (condition) return;
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
};

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::canonical(std::filesystem::temp_directory_path())
            / ("MineBackupKnownUserFolders-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

KnownUserFolders::Dependencies DependenciesFor(
    KnownUserFolders::Platform platform,
    const std::map<std::string, std::string>& environment,
    const std::map<std::filesystem::path, std::string>& files = {}) {
    KnownUserFolders::Dependencies dependencies;
    dependencies.platform = platform;
    dependencies.readEnvironment = [environment](std::string_view name) -> std::optional<std::string> {
        const auto found = environment.find(std::string(name));
        return found == environment.end() ? std::nullopt : std::optional<std::string>(found->second);
    };
    dependencies.readFile = [files](const std::filesystem::path& path) -> std::optional<std::string> {
        const auto found = files.find(path);
        return found == files.end() ? std::nullopt : std::optional<std::string>(found->second);
    };
    dependencies.resolveWindowsDocuments = [] { return std::nullopt; };
    dependencies.resolveMacDocuments = [] { return std::nullopt; };
    return dependencies;
}

void TestLinuxResolution(CheckContext& test, const TemporaryDirectory& temporary) {
    const auto home = temporary.path / "home";
    const auto configured = home / "configured-documents";
    auto dependencies = DependenciesFor(
        KnownUserFolders::Platform::Linux,
        {{"HOME", home.string()}, {"XDG_DOCUMENTS_DIR", configured.string()}});
    KnownUserFolders::Resolver resolver(dependencies);
    test.Expect(resolver.ResolveDocuments() == configured,
        "Linux should prefer XDG_DOCUMENTS_DIR");

    const auto configHome = home / "config";
    dependencies = DependenciesFor(
        KnownUserFolders::Platform::Linux,
        {{"HOME", home.string()}, {"XDG_CONFIG_HOME", configHome.string()}},
        {{configHome / "user-dirs.dirs", "XDG_DOCUMENTS_DIR=\"$HOME/xdg-docs\"\n"}});
    resolver = KnownUserFolders::Resolver(dependencies);
    test.Expect(resolver.ResolveDocuments() == home / "xdg-docs",
        "Linux should expand HOME in user-dirs.dirs");

    dependencies = DependenciesFor(
        KnownUserFolders::Platform::Linux, {{"HOME", home.string()}});
    resolver = KnownUserFolders::Resolver(dependencies);
    test.Expect(resolver.ResolveDocuments() == home / "Documents",
        "Linux should fall back to HOME/Documents");
}

void TestPlatformResolvers(CheckContext& test, const TemporaryDirectory& temporary) {
    const auto home = temporary.path / "home";
    const auto windowsDocuments = temporary.path / "windows-documents";
    auto dependencies = DependenciesFor(
        KnownUserFolders::Platform::Windows, {{"HOME", home.string()}});
    dependencies.resolveWindowsDocuments = [windowsDocuments] {
        return std::optional<std::filesystem::path>(windowsDocuments);
    };
    KnownUserFolders::Resolver resolver(dependencies);
    test.Expect(resolver.ResolveDocuments() == windowsDocuments,
        "Windows should use the injected Known Folder resolver");

    dependencies = DependenciesFor(
        KnownUserFolders::Platform::MacOS, {{"HOME", home.string()}});
    resolver = KnownUserFolders::Resolver(dependencies);
    test.Expect(resolver.ResolveDocuments() == home / "Documents",
        "macOS should fall back to HOME/Documents");

    const auto macDocuments = temporary.path / "mac-documents";
    dependencies.resolveMacDocuments = [macDocuments] {
        return std::optional<std::filesystem::path>(macDocuments);
    };
    resolver = KnownUserFolders::Resolver(dependencies);
    test.Expect(resolver.ResolveDocuments() == macDocuments,
        "macOS should prefer the injected system resolver");
}

void TestFallbacks(CheckContext& test, const TemporaryDirectory& temporary) {
    auto dependencies = DependenciesFor(KnownUserFolders::Platform::Windows, {});
    dependencies.resolveWindowsDocuments = [] {
        return std::optional<std::filesystem::path>(std::filesystem::path(L"relative-documents"));
    };
    KnownUserFolders::Resolver resolver(dependencies);
    AppPaths paths;
    paths.dataRoot = temporary.path / "profile-data";
    test.Expect(!resolver.ResolveDocuments(),
        "relative Documents candidates must not resolve through the working directory");
    test.Expect(resolver.ResolveRecommendedBackupRoot(paths)
            == paths.dataRoot / L"backups",
        "recommended backup root should fall back to AppPaths dataRoot/backups");

    dependencies = DependenciesFor(KnownUserFolders::Platform::Linux, {});
    resolver = KnownUserFolders::Resolver(dependencies);
    test.Expect(resolver.ResolveRecommendedBackupRoot(paths)
            == paths.dataRoot / L"backups",
        "missing environment should use the application data fallback");

    paths.dataRoot = std::filesystem::path(L"relative-data-root");
    test.Expect(resolver.ResolveRecommendedBackupRoot(paths).empty(),
        "relative AppPaths data root must not fall back through the working directory");
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    CheckContext test;
    TestLinuxResolution(test, temporary);
    TestPlatformResolvers(test, temporary);
    TestFallbacks(test, temporary);
    if (test.failures != 0) {
        std::cerr << test.failures << " KnownUserFolders test(s) failed\n";
        return 1;
    }
    std::cout << "All KnownUserFolders tests passed\n";
    return 0;
}
