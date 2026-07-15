#include "MigrationReportUI.h"

#include "MigrationCoordinator.h"
#include "PlatformCompat.h"
#include "DesktopServices.h"
#include "imgui-all.h"
#include "text_to_text.h"

#include <filesystem>

namespace MigrationReportUI {

void DrawSettings() {
    const auto report = MigrationCoordinator::GetMigrationReport();
    ImGui::SeparatorText("MineBackup 1.15 -> 1.16 migration");
    ImGui::TextWrapped("Migration is one-way. Recovery snapshots are retained and archive files are never renamed.");
    for (const auto& unit : report.units) {
        const char* state = unit.status == MigrationStatus::Succeeded ? "Succeeded"
            : unit.status == MigrationStatus::Degraded ? "Degraded"
            : unit.status == MigrationStatus::Failed ? "Failed"
            : unit.status == MigrationStatus::Pending ? "Pending" : "Not needed";
        ImGui::BulletText("%s: %s", wstring_to_utf8(unit.unitId).c_str(), state);
        if (!unit.message.empty()) ImGui::TextWrapped("%s", wstring_to_utf8(unit.message).c_str());
        if (!unit.snapshotPath.empty()) {
            ImGui::TextWrapped("Recovery snapshot: %s", wstring_to_utf8(unit.snapshotPath).c_str());
            const std::filesystem::path snapshot(unit.snapshotPath);
            if (std::filesystem::exists(snapshot)) {
                ImGui::SameLine();
                ImGui::PushID((wstring_to_utf8(unit.unitId) + "_snapshot").c_str());
                if (ImGui::SmallButton("Open")) {
                    (void)GetDesktopServices()->OpenFolder(snapshot.parent_path());
                }
                ImGui::PopID();
            }
        }
        if (unit.status == MigrationStatus::Failed || unit.status == MigrationStatus::Degraded) {
            ImGui::PushID(wstring_to_utf8(unit.unitId).c_str());
            if (ImGui::Button("Retry")) MigrationCoordinator::RetryMigration(unit.unitId);
            ImGui::PopID();
        }
    }
}

} // namespace MigrationReportUI
