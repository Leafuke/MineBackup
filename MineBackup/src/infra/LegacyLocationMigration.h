#pragma once

#include "LegacyLocationDiscovery.h"

#include <filesystem>
#include <string>

struct LegacyLocationMigrationResult {
    bool success = false;
    std::wstring error;
};

LegacyLocationMigrationResult ImportLegacyLocation(
    const LegacyLocationCandidate& source,
    const std::filesystem::path& targetConfigFile,
    const std::filesystem::path& targetHistoryFile);
