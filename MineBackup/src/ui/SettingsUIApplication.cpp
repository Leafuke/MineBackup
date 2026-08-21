#include "SettingsUIPrivate.h"

#include "AppPaths.h"
#include "KnownUserFolders.h"

using namespace std;

void DrawApplicationSettings() {
	ImGui::SeparatorText(L("DEFAULT_BACKUP_ROOT_TITLE"));
	ImGui::TextWrapped("%s", L("DEFAULT_BACKUP_ROOT_DESCRIPTION"));
	ImGui::Spacing();

	const filesystem::path recommended =
		KnownUserFolders::Resolver{}.ResolveRecommendedBackupRoot(GetAppPaths());
	if (g_defaultBackupRootPath.empty() && !recommended.empty()) {
		g_defaultBackupRootPath = recommended.wstring();
	}

	const string currentPath = wstring_to_utf8(g_defaultBackupRootPath);
	ImGui::TextWrapped("%s", currentPath.c_str());
	ImGui::Spacing();
	if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
		const auto selected = GetDesktopServices()->SelectFolder().path;
		if (!selected.empty() && selected.is_absolute()) {
			g_defaultBackupRootPath = selected.lexically_normal().wstring();
		}
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(recommended.empty());
	if (ImGui::Button(L("BUTTON_RESTORE_RECOMMENDED"))) {
		g_defaultBackupRootPath = recommended.wstring();
	}
	ImGui::EndDisabled();
}
