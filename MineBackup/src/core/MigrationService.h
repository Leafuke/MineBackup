#pragma once

#ifndef MINEBACKUP_ENABLE_V15_MIGRATION
#define MINEBACKUP_ENABLE_V15_MIGRATION 1
#endif

#include "AppState.h"

#include <string>

namespace MigrationService {

std::wstring GenerateLegacyConfigId(const Config& config, int configIndex);
MigrationReport RunStartupMigration();
MigrationUnitResult EnsureWorldMigrated(int configIndex, const std::wstring& folderName, const std::wstring& fallbackPath = L"");
MigrationUnitResult EnsureWorldMigrated(const Config& config, int configIndex, const std::wstring& folderName, const std::wstring& fallbackPath = L"");
MigrationUnitResult EnsureCloudMigrated(int configIndex);
void RecordCloudMigrationResult(int configIndex, MigrationStatus status, const std::wstring& message, const std::wstring& snapshotPath = L"");
bool RetryMigration(const std::wstring& unitId);
MigrationReport GetMigrationReport();
bool ShouldShowStartupSummary();
void DismissStartupSummary();
bool IsHistoryPersistenceBlocked();
void DrawMigrationSettings();

} // namespace MigrationService
