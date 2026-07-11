#pragma once

#include "AppState.h"

#include <filesystem>
#include <functional>
#include <string>

namespace MigrationCoordinator {

struct MigrationPaths {
    std::filesystem::path configFile;
    std::filesystem::path historyFile;
    std::filesystem::path reportFile;
    std::filesystem::path snapshotRoot;
};

struct AdapterCallbacks {
    std::function<std::wstring(const Config&, int)> generateLegacyConfigId;
    std::function<MigrationReport()> runStartupMigration;
    std::function<MigrationUnitResult(int, const std::wstring&, const std::wstring&)> ensureWorldMigratedByIndex;
    std::function<MigrationUnitResult(const Config&, int, const std::wstring&, const std::wstring&)> ensureWorldMigrated;
    std::function<MigrationUnitResult(int)> ensureCloudMigrated;
    std::function<void(int, MigrationStatus, const std::wstring&, const std::wstring&)> recordCloudMigrationResult;
    std::function<bool(const std::wstring&)> retryMigration;
};

void ConfigurePaths(MigrationPaths paths);
MigrationPaths GetPaths();

void InstallAdapter(AdapterCallbacks callbacks);
bool HasAdapter();

std::wstring GenerateLegacyConfigId(const Config& config, int configIndex);
MigrationStatus HigherPriorityStatus(MigrationStatus lhs, MigrationStatus rhs);
MigrationReport RunStartupMigration();
MigrationUnitResult EnsureWorldMigrated(int configIndex, const std::wstring& folderName, const std::wstring& fallbackPath = L"");
MigrationUnitResult EnsureWorldMigrated(const Config& config, int configIndex, const std::wstring& folderName, const std::wstring& fallbackPath = L"");
MigrationUnitResult EnsureCloudMigrated(int configIndex);
void RecordCloudMigrationResult(int configIndex, MigrationStatus status, const std::wstring& message, const std::wstring& snapshotPath = L"");
bool RetryMigration(const std::wstring& unitId);

void RecordUnit(const MigrationUnitResult& unit);
MigrationReport GetMigrationReport();
bool ShouldShowStartupSummary();
void SetStartupSummaryVisible(bool visible);
void DismissStartupSummary();
bool IsHistoryPersistenceBlocked();
void SetHistoryPersistenceBlocked(bool blocked);
bool IsConfigurationPersistenceBlocked();
void SetConfigurationPersistenceBlocked(bool blocked);

} // namespace MigrationCoordinator
