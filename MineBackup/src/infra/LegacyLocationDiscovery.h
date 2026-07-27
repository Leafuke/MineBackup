#pragma once

#include <filesystem>
#include <vector>

enum class LegacyLocationOrigin {
    ExecutableDirectory,
    OriginalWorkingDirectory,
    KnownPlatformLocation
};

struct LegacyLocationProbe {
    std::filesystem::path root;
    LegacyLocationOrigin origin = LegacyLocationOrigin::KnownPlatformLocation;
};

struct LegacyLocationCandidate {
    std::filesystem::path root;
    std::filesystem::path configFile;
    std::filesystem::path historyFile;
    std::vector<LegacyLocationOrigin> origins;
};

struct LegacyLocationDiscoveryResult {
    bool targetInitialized = false;
    std::vector<LegacyLocationCandidate> candidates;
};

LegacyLocationDiscoveryResult DiscoverLegacyLocations(
    const std::filesystem::path& targetConfigFile,
    const std::filesystem::path& targetHistoryFile,
    const std::vector<LegacyLocationProbe>& probes);
