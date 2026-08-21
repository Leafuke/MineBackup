#include "ApplicationActions.h"

#include "AppPaths.h"
#include "Globals.h"
#include "KnotLinkPackageManager.h"
#include "NetworkBackendFactory.h"
#include "NetworkService.h"
#include "TaskCoordinator.h"
#include "i18n.h"
#include "text_to_text.h"

#include <stop_token>
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
