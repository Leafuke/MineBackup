#include "ApplicationActions.h"

#include "AppPaths.h"
#include "AppState.h"
#include "ConfigManager.h"
#include "Globals.h"
#include "KnotLinkPackageManager.h"
#include "NetworkBackendFactory.h"
#include "NetworkService.h"
#include "TaskCoordinator.h"
#include "i18n.h"
#include "text_to_text.h"

#include <algorithm>
#include <stop_token>
#include <filesystem>
#include <system_error>
#include <utility>

using namespace std;

bool StartKnotLinkInstallerDownload() {
	if (g_KnotLinkInstallRunning) {
		return false;
	}
	g_KnotLinkInstallRunning = true;
	g_KnotLinkInstallSucceeded = false;
	g_KnotLinkInstallMessage =
		utf8_to_wstring(L("KNOTLINK_INSTALL_DOWNLOADING"));
	const auto backend = CreatePlatformNetworkBackend();
	const auto paths = GetAppPaths();
	const bool submitted = TaskCoordinator::Instance().Submit(
		L"install-knotlink-service",
		{L"network:knotlink-installer"},
		[backend, paths](stop_token token) {
			NetworkService network(backend);
			const auto result =
				minebackup::knotlink::DownloadAndOpenCurrentKnotLinkPackage(
					network, paths, token);
			TaskEvent event{L"knotlink-installer-complete", result.error};
			event.values[L"success"] = result.success ? L"1" : L"0";
			event.values[L"path"] = result.packagePath.wstring();
			event.values[L"source"] = utf8_to_wstring(result.sourceUrl);
			TaskCoordinator::Instance().PostEvent(std::move(event));
		});
	if (!submitted) {
		g_KnotLinkInstallRunning = false;
		g_KnotLinkInstallMessage =
			utf8_to_wstring(L("KNOTLINK_INSTALL_BUSY"));
	}
	return submitted;
}

void AutoDiscoverWorldConfigurations()
{
	if (!g_AutoScanForWorlds) {
		return;
	}

	for (auto& [sourceIndex, sourceConfig] : g_appState.configs) {
		(void)sourceIndex;
		if (sourceConfig.saveRoot.empty()) {
			continue;
		}
		const filesystem::path parent = filesystem::path(sourceConfig.saveRoot)
			.lexically_normal().parent_path().parent_path();
		std::error_code error;
		if (parent.empty() || !filesystem::exists(parent, error) || error) {
			continue;
		}

		for (filesystem::directory_iterator it(
				parent, filesystem::directory_options::skip_permission_denied, error);
			!error && it != filesystem::directory_iterator(); ++it) {
			const auto& entry = *it;
			const filesystem::path savesPath = entry.path() / "saves";
			if (!entry.is_directory() || !filesystem::exists(savesPath, error) || error) {
				continue;
			}
			const bool alreadyExists = any_of(
				g_appState.configs.begin(), g_appState.configs.end(),
				[&](const auto& item) { return item.second.saveRoot == savesPath.wstring(); });
			if (alreadyExists) {
				continue;
			}

			const int index = CreateNewNormalConfig();
			g_appState.configs[index] = sourceConfig;
			AssignFreshNormalConfigId(index);
			g_appState.configs[index].name = wstring_to_utf8(entry.path().filename().wstring());
			g_appState.configs[index].saveRoot = savesPath.wstring();
			g_appState.configs[index].worlds.clear();
			EnsureDefaultBackupBlacklist(g_appState.configs[index].blacklist);
		}
	}
}
