#include "MigrationCoordinator.h"

#include "AtomicFileWriter.h"
#include "FolderRewindFormat.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <utility>

using namespace std;

namespace MigrationCoordinator {
namespace {

mutex g_mutex;
MigrationPaths g_paths;
AdapterCallbacks g_adapter;
MigrationReport g_report;
bool g_showSummary = false;
atomic<bool> g_historyPersistenceBlocked{false};
atomic<bool> g_configurationPersistenceBlocked{false};

MigrationUnitResult NotNeededUnit(const wstring& unitId) {
    MigrationUnitResult unit;
    unit.unitId = unitId;
    unit.status = MigrationStatus::NotNeeded;
    unit.message = L"No legacy migration adapter is installed.";
    return unit;
}

void PersistReport(const MigrationReport& report, const filesystem::path& path) {
    if (path.empty()) return;
    nlohmann::json root;
    root["Version"] = "1.15-to-1.16";
    root["UpdatedAtUtc"] = wstring_to_utf8(report.updatedAtUtc);
    root["Status"] = static_cast<int>(report.status);
    root["Units"] = nlohmann::json::array();
    for (const auto& item : report.units) {
        nlohmann::json value;
        value["UnitId"] = wstring_to_utf8(item.unitId);
        value["Status"] = static_cast<int>(item.status);
        value["Message"] = wstring_to_utf8(item.message);
        value["SnapshotPath"] = wstring_to_utf8(item.snapshotPath);
        value["MigratedItems"] = item.migratedItems;
        value["SkippedItems"] = item.skippedItems;
        root["Units"].push_back(std::move(value));
    }
    AtomicFileWriter::WriteText(path, root.dump(2));
}

bool TryMigrationStatus(int value, MigrationStatus& status) {
    if (value < static_cast<int>(MigrationStatus::NotNeeded)
        || value > static_cast<int>(MigrationStatus::Failed)) {
        return false;
    }
    status = static_cast<MigrationStatus>(value);
    return true;
}

MigrationReport LoadReport(const filesystem::path& path) {
    MigrationReport report;
    if (path.empty()) return report;
    ifstream input(path, ios::binary);
    if (!input.is_open()) return report;
    const auto root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded() || !root.is_object()
        || root.value("Version", string{}) != "1.15-to-1.16") {
        return report;
    }
    try {
        const auto units = root.find("Units");
        if (units == root.end() || !units->is_array()) return {};
        report.updatedAtUtc = utf8_to_wstring(root.value("UpdatedAtUtc", string{}));
        for (const auto& value : *units) {
            if (!value.is_object()) return {};
            MigrationUnitResult unit;
            unit.unitId = utf8_to_wstring(value.value("UnitId", string{}));
            if (unit.unitId.empty() || !TryMigrationStatus(value.value("Status", -1), unit.status)) return {};
            unit.message = utf8_to_wstring(value.value("Message", string{}));
            unit.snapshotPath = utf8_to_wstring(value.value("SnapshotPath", string{}));
            unit.migratedItems = value.value("MigratedItems", 0);
            unit.skippedItems = value.value("SkippedItems", 0);
            report.units.push_back(std::move(unit));
        }
        report.status = MigrationStatus::NotNeeded;
        for (const auto& unit : report.units) {
            report.status = HigherPriorityStatus(report.status, unit.status);
        }
    }
    catch (...) {
        return {};
    }
    return report;
}

} // namespace

void ConfigurePaths(MigrationPaths paths) {
    MigrationReport report = LoadReport(paths.reportFile);
    lock_guard<mutex> lock(g_mutex);
    g_paths = std::move(paths);
    g_report = std::move(report);
}

MigrationPaths GetPaths() {
    lock_guard<mutex> lock(g_mutex);
    return g_paths;
}

void InstallAdapter(AdapterCallbacks callbacks) {
    lock_guard<mutex> lock(g_mutex);
    g_adapter = std::move(callbacks);
}

bool HasAdapter() {
    lock_guard<mutex> lock(g_mutex);
    return static_cast<bool>(g_adapter.runStartupMigration);
}

wstring GenerateLegacyConfigId(const Config& config, int configIndex) {
    function<wstring(const Config&, int)> callback;
    {
        lock_guard<mutex> lock(g_mutex);
        callback = g_adapter.generateLegacyConfigId;
    }
    return callback ? callback(config, configIndex) : FolderRewindFormat::GenerateGuidString();
}

MigrationStatus HigherPriorityStatus(MigrationStatus lhs, MigrationStatus rhs) {
    auto priority = [](MigrationStatus status) {
        switch (status) {
        case MigrationStatus::Failed: return 4;
        case MigrationStatus::Degraded: return 3;
        case MigrationStatus::Pending: return 2;
        case MigrationStatus::Succeeded: return 1;
        case MigrationStatus::NotNeeded: default: return 0;
        }
    };
    return priority(rhs) > priority(lhs) ? rhs : lhs;
}

MigrationReport RunStartupMigration() {
    function<MigrationReport()> callback;
    {
        lock_guard<mutex> lock(g_mutex);
        callback = g_adapter.runStartupMigration;
    }
    return callback ? callback() : GetMigrationReport();
}

MigrationUnitResult EnsureWorldMigrated(int configIndex, const wstring& folderName, const wstring& fallbackPath) {
    function<MigrationUnitResult(int, const wstring&, const wstring&)> callback;
    {
        lock_guard<mutex> lock(g_mutex);
        callback = g_adapter.ensureWorldMigratedByIndex;
    }
    return callback ? callback(configIndex, folderName, fallbackPath)
        : NotNeededUnit(L"world:" + to_wstring(configIndex) + L":" + folderName);
}

MigrationUnitResult EnsureWorldMigrated(
    const Config& config, int configIndex, const wstring& folderName, const wstring& fallbackPath) {
    function<MigrationUnitResult(const Config&, int, const wstring&, const wstring&)> callback;
    {
        lock_guard<mutex> lock(g_mutex);
        callback = g_adapter.ensureWorldMigrated;
    }
    return callback ? callback(config, configIndex, folderName, fallbackPath)
        : NotNeededUnit(L"world:" + to_wstring(configIndex) + L":" + folderName);
}

MigrationUnitResult EnsureCloudMigrated(int configIndex) {
    function<MigrationUnitResult(int)> callback;
    {
        lock_guard<mutex> lock(g_mutex);
        callback = g_adapter.ensureCloudMigrated;
    }
    return callback ? callback(configIndex) : NotNeededUnit(L"cloud:" + to_wstring(configIndex));
}

void RecordCloudMigrationResult(
    int configIndex, MigrationStatus status, const wstring& message, const wstring& snapshotPath) {
    function<void(int, MigrationStatus, const wstring&, const wstring&)> callback;
    {
        lock_guard<mutex> lock(g_mutex);
        callback = g_adapter.recordCloudMigrationResult;
    }
    if (callback) callback(configIndex, status, message, snapshotPath);
}

bool RetryMigration(const wstring& unitId) {
    function<bool(const wstring&)> callback;
    {
        lock_guard<mutex> lock(g_mutex);
        callback = g_adapter.retryMigration;
    }
    return callback && callback(unitId);
}

void RecordUnit(const MigrationUnitResult& unit) {
    MigrationReport report;
    filesystem::path reportPath;
    {
        lock_guard<mutex> lock(g_mutex);
        MigrationUnitResult mergedUnit = unit;
        auto it = find_if(g_report.units.begin(), g_report.units.end(),
            [&](const MigrationUnitResult& value) { return value.unitId == unit.unitId; });
        if (it == g_report.units.end()) g_report.units.push_back(mergedUnit);
        else {
            if (mergedUnit.snapshotPath.empty()) mergedUnit.snapshotPath = it->snapshotPath;
            *it = std::move(mergedUnit);
        }
        g_report.updatedAtUtc = FolderRewindFormat::MakeUtcTimestampString();
        g_report.status = MigrationStatus::NotNeeded;
        for (const auto& item : g_report.units) {
            g_report.status = HigherPriorityStatus(g_report.status, item.status);
        }
        report = g_report;
        reportPath = g_paths.reportFile;
    }
    PersistReport(report, reportPath);
}

MigrationReport GetMigrationReport() {
    lock_guard<mutex> lock(g_mutex);
    return g_report;
}

bool ShouldShowStartupSummary() {
    lock_guard<mutex> lock(g_mutex);
    return g_showSummary;
}

void SetStartupSummaryVisible(bool visible) {
    lock_guard<mutex> lock(g_mutex);
    g_showSummary = visible;
}

void DismissStartupSummary() {
    SetStartupSummaryVisible(false);
}

bool IsHistoryPersistenceBlocked() {
    return g_historyPersistenceBlocked.load();
}

void SetHistoryPersistenceBlocked(bool blocked) {
    g_historyPersistenceBlocked.store(blocked);
}

bool IsConfigurationPersistenceBlocked() {
    return g_configurationPersistenceBlocked.load();
}

void SetConfigurationPersistenceBlocked(bool blocked) {
    g_configurationPersistenceBlocked.store(blocked);
}

} // namespace MigrationCoordinator
