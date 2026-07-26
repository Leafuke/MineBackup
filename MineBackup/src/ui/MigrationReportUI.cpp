#include "MigrationReportUI.h"

#include "MigrationCoordinator.h"
#include "PlatformCompat.h"
#include "DesktopServices.h"
#include "i18n.h"
#include "imgui-all.h"
#include "text_to_text.h"

#include <filesystem>

namespace MigrationReportUI {

const char* StatusLabel(MigrationStatus status) {
    switch (status) {
    case MigrationStatus::Succeeded: return L("MIGRATION_STATE_SUCCEEDED");
    case MigrationStatus::Degraded: return L("MIGRATION_STATE_DEGRADED");
    case MigrationStatus::Failed: return L("MIGRATION_STATE_FAILED");
    case MigrationStatus::Pending: return L("MIGRATION_STATE_PENDING");
    case MigrationStatus::NotNeeded: return L("MIGRATION_STATE_NOT_NEEDED");
    }
    return L("CAP_STATE_UNKNOWN");
}

std::string UnitLabel(const std::wstring& unitId) {
    if (unitId == L"startup:config") return L("MIGRATION_UNIT_CONFIG");
    if (unitId == L"startup:history") return L("MIGRATION_UNIT_HISTORY");
    if (unitId == L"startup:legacy-location") return L("LEGACY_LOCATION_TITLE");
    if (unitId == L"cloud:legacy-config-ini") return L("MIGRATION_UNIT_LEGACY_CLOUD");
    if (unitId.rfind(L"world:", 0) == 0) {
        const size_t nameStart = unitId.find(L':', 6);
        const std::string name = wstring_to_utf8(nameStart == std::wstring::npos
            ? unitId.substr(6) : unitId.substr(nameStart + 1));
        return wstring_to_utf8(MineFormatMessage("MIGRATION_UNIT_WORLD", name.c_str()));
    }
    if (unitId.rfind(L"cloud:", 0) == 0) {
        const std::string name = wstring_to_utf8(unitId.substr(6));
        return wstring_to_utf8(MineFormatMessage("MIGRATION_UNIT_CLOUD", name.c_str()));
    }
    return wstring_to_utf8(unitId);
}

void DrawSettings() {
    const auto report = MigrationCoordinator::GetMigrationReport();
    if (report.units.empty()) return;
    ImGui::SeparatorText(L("MIGRATION_SECTION_TITLE"));
    ImGui::TextWrapped("%s", L("MIGRATION_NOTICE"));
    for (const auto& unit : report.units) {
        ImGui::PushID(wstring_to_utf8(unit.unitId).c_str());
        ImGui::BulletText("%s: %s", UnitLabel(unit.unitId).c_str(), StatusLabel(unit.status));
        ImGui::Indent();
        if (!unit.message.empty()
            && (unit.status == MigrationStatus::Failed || unit.status == MigrationStatus::Degraded)) {
            ImGui::TextDisabled("%s:", L("MIGRATION_TECHNICAL_DETAIL"));
            ImGui::TextWrapped("%s", wstring_to_utf8(unit.message).c_str());
        }
        if (!unit.snapshotPath.empty()) {
            ImGui::TextWrapped(L("MIGRATION_RECOVERY_SNAPSHOT"), wstring_to_utf8(unit.snapshotPath).c_str());
            const std::filesystem::path snapshot(unit.snapshotPath);
            if (std::filesystem::exists(snapshot)) {
                if (ImGui::SmallButton(L("BUTTON_OPEN"))) {
                    (void)GetDesktopServices()->OpenFolder(snapshot.parent_path());
                }
            }
        }
        if (unit.status == MigrationStatus::Failed || unit.status == MigrationStatus::Degraded) {
            if (ImGui::Button(L("BUTTON_RETRY"))) MigrationCoordinator::RetryMigration(unit.unitId);
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
}

} // namespace MigrationReportUI
