

#include "Application.h"
#include "ApplicationActions.h"
#include "ApplicationEventRouter.h"
#include "AppearanceRuntime.h"
#include "ConfigSelection.h"
#include "ImGuiRuntime.h"
#include "Broadcast.h"
#include "Globals.h"
#include "SettingsUI.h"
#include "MigrationReportUI.h"
#include "UIHelpers.h"
#include "MainUI.h"
#include "MainUiController.h"
#include "SettingsUIHotkeys.h"
#include "imgui-all.h"
#include "imgui_style.h"
#include "i18n.h"
#include "AppState.h"
#include "AppPaths.h"
#include "LaunchOptions.h"
#include "TaskSystem.h"
#include "TaskCoordinator.h"
#include "InterruptedTaskRecovery.h"
#include "KnotLinkPackageManager.h"
#include "KnotLinkServerManager.h"
#include "NetworkBackendFactory.h"
#include "NetworkService.h"
#include "RemoteContentService.h"
#include "PlatformCompat.h"
#include "DesktopServices.h"
#include "NativeDesktopServices.h"
#include "CommandConsole.h"
#include "ConfigManager.h"
#include "text_to_text.h"
#include "HistoryManager.h"
#include "BackupManager.h"
#include "CloudSyncService.h"
#include "CoreValidation.h"
#include "FileName.h"
#include "GameSessionManager.h"
#include "MigrationCoordinator.h"
#include "LogPanel.h"
#include "SingleInstanceService.h"
#include "LegacyLocationDiscovery.h"
#include "LegacyLocationMigration.h"
#include "Logging.h"
#include "Sha256.h"
#include "SpecialConfigPolicy.h"
#if MINEBACKUP_ENABLE_V15_MIGRATION
#include "V15MigrationAdapter.h"
#endif

#ifdef _WIN32
#include <conio.h>
#else
#include <cstdio>
#include <unistd.h>
inline int _getch() { return std::getchar(); }
#endif
#include <fstream>
#include <system_error>
#ifdef __APPLE__
#include "MacDesktopBridge.h"
#include <mach-o/dyld.h>
#include <limits.h>
#include <CoreText/CoreText.h>
#include <CoreFoundation/CoreFoundation.h>
#endif
#ifdef __linux__
#include <ProcessRunner.h>
#endif

using namespace std;

#define APP_PRINTF_INFO(eventId, ...) \
	MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Application, eventId, __VA_ARGS__)
#define APP_PRINTF_WARNING(eventId, ...) \
	MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Application, eventId, __VA_ARGS__)
#define APP_PRINTF_ERROR(eventId, ...) \
	MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Application, eventId, __VA_ARGS__)
#define PLATFORM_PRINTF_INFO(eventId, ...) \
	MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Platform, eventId, __VA_ARGS__)
#define PLATFORM_PRINTF_WARNING(eventId, ...) \
	MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Platform, eventId, __VA_ARGS__)
#define PLATFORM_PRINTF_ERROR(eventId, ...) \
	MB_LOG_PRINTF_ERROR(minebackup::logging::LogCategory::Platform, eventId, __VA_ARGS__)

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int RunApplication(const ApplicationEntryContext& entryContext)
{
	vector<wstring> launchArguments = entryContext.arguments;
#ifdef _WIN32
	HINSTANCE hInstance = reinterpret_cast<HINSTANCE>(entryContext.nativeInstance);
#endif
	#ifdef __APPLE__
	// Install before migration prompts or GLFW can pump the Cocoa launch event.
	MacBeginLaunchObservation();
	#endif
	// Use the host language for any pre-configuration native prompts. Loading an
	// existing profile below will still restore the user's explicit app language.
	GetUserDefaultUILanguageWin();
	const filesystem::path originalWorkingDirectory = filesystem::current_path();
	(void)originalWorkingDirectory;
	LaunchOptions launchOptions;
	wstring launchError;
	if (!ParseLaunchOptions(launchArguments, launchOptions, launchError)) {
		MessageBoxWin("MineBackup", wstring_to_utf8(launchError), 2);
		return 2;
	}
	if (!launchOptions.legacyServiceCleanup.empty()) {
		wstring cleanupError;
		if (!TaskSystem::RemoveLegacyServiceAfterValidation(
				launchOptions.legacyServiceCleanup, cleanupError)) {
			MessageBoxWin(L("LEGACY_SERVICE_CLEANUP_TITLE"),
				wstring_to_utf8(cleanupError), 2);
			return 7;
		}
		MessageBoxWin(L("LEGACY_SERVICE_CLEANUP_TITLE"), L("LEGACY_SERVICE_REMOVED"), 0);
		return 0;
	}
	if (launchOptions.legacyServiceMode) {
		#ifdef _WIN32
		OutputDebugStringW(L"MineBackup: --service is deprecated and disabled in 1.16.\n");
		#else
		fputs("MineBackup: --service is deprecated and disabled in 1.16.\n", stderr);
		#endif
		return 6;
	}
	AppPaths appPaths;
	if (!ResolveAppPaths(launchOptions, GetExecutablePath(), appPaths, launchError)) {
		MessageBoxWin("MineBackup", wstring_to_utf8(launchError), 2);
		return 2;
	}
	SetCurrentAppPaths(std::move(appPaths));
	bool launchSilentStartup = launchOptions.silentStartup || launchOptions.autostart;
	#ifdef __APPLE__
	const bool hasExplicitLaunchTarget = launchOptions.autostart
		|| !launchOptions.runSpecialId.empty()
		|| !launchOptions.selectConfigId.empty()
		|| launchOptions.legacySpecialConfigIndex.has_value();
	#endif
	const auto& paths = GetAppPaths();
	SingleInstanceService singleInstance;
	const auto instanceResult = singleInstance.Acquire(paths.profileIdentity, paths.runtimeRoot, launchError);
	if (instanceResult == InstanceAcquireResult::AlreadyRunning) {
		InstanceRequest request;
		if (!launchOptions.runSpecialId.empty()) {
			request = { InstanceRequestType::RunSpecial, launchOptions.runSpecialId };
		}
		else if (!launchOptions.selectConfigId.empty()) {
			request = { InstanceRequestType::SelectConfig, launchOptions.selectConfigId };
		}
		if (!singleInstance.Send(request, launchError)) {
			MessageBoxWin("MineBackup", wstring_to_utf8(launchError), 2);
			return 3;
		}
		return 0;
	}
	if (instanceResult == InstanceAcquireResult::Failed) {
		MessageBoxWin("MineBackup", wstring_to_utf8(launchError), 2);
		return 3;
	}
	const auto interruptedRecovery = RecoverInterruptedTaskArtifacts(
		paths.runtimeRoot, paths.stateRoot / L"task-recovery" / L"last-interrupted.json");
	if (!interruptedRecovery.removedPaths.empty() || !interruptedRecovery.errors.empty()) {
		APP_PRINTF_WARNING("application.recovery.interrupted_tasks",

			"Removed %zu interrupted task artifact(s), %llu byte(s).",
			interruptedRecovery.removedPaths.size(),
			static_cast<unsigned long long>(interruptedRecovery.removedBytes));
		if (!interruptedRecovery.reportPath.empty()) {
			APP_PRINTF_INFO("application.recovery.report",
				"Recovery report: %s",
				wstring_to_utf8(interruptedRecovery.reportPath.wstring()).c_str());
		}
		for (const auto& error : interruptedRecovery.errors) {
			APP_PRINTF_ERROR("application.recovery.failed",
				"Recovery failed: %s", wstring_to_utf8(error).c_str());
		}
	}
	vector<LegacyLocationProbe> locationProbes = {
		{GetExecutablePath().parent_path(), LegacyLocationOrigin::ExecutableDirectory},
		{originalWorkingDirectory, LegacyLocationOrigin::OriginalWorkingDirectory}
	};
#ifdef _WIN32

	wchar_t localAppData[MAX_PATH] = {};
	if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
		locationProbes.push_back({filesystem::path(localAppData) / L"MineBackup", LegacyLocationOrigin::KnownPlatformLocation});
	}
#else
	if (const char* home = getenv("HOME")) {
		locationProbes.push_back({filesystem::path(home) / ".minebackup", LegacyLocationOrigin::KnownPlatformLocation});
#ifdef __APPLE__
		locationProbes.push_back({filesystem::path(home) / "Library" / "Application Support" / "MineBackup",
			LegacyLocationOrigin::KnownPlatformLocation});

#else
		locationProbes.push_back({filesystem::path(home) / ".config" / "MineBackup", LegacyLocationOrigin::KnownPlatformLocation});
		locationProbes.push_back({filesystem::path(home) / ".local" / "share" / "MineBackup", LegacyLocationOrigin::KnownPlatformLocation});
#endif
	}
#endif
	const auto locationDiscovery = DiscoverLegacyLocations(paths.ConfigFile(), paths.HistoryFile(), locationProbes);
	optional<LegacyLocationCandidate> importedLegacyLocation;
	if (!locationDiscovery.targetInitialized) {
		for (const auto& candidate : locationDiscovery.candidates) {
			if (candidate.configFile.empty()) continue;
			const string source = wstring_to_utf8(candidate.root.wstring());
			const string configRoot = wstring_to_utf8(paths.configRoot.wstring());
			const string dataRoot = wstring_to_utf8(paths.dataRoot.wstring());
			const string prompt = wstring_to_utf8(MineFormatMessage(
				"LEGACY_LOCATION_PROMPT", source.c_str(), configRoot.c_str(), dataRoot.c_str()));
			if (!ConfirmMessageBox(L("LEGACY_LOCATION_TITLE"), prompt)) continue;
			const auto migration = ImportLegacyLocation(candidate, paths.ConfigFile(), paths.HistoryFile());
			if (!migration.success) {
				MessageBoxWin("MineBackup", wstring_to_utf8(migration.error), 2);
				return 5;
			}
			importedLegacyLocation = candidate;
			break;
		}
	}
	MigrationCoordinator::ConfigurePaths({
		paths.ConfigFile(),
		paths.HistoryFile(),
		paths.stateRoot / L"migration" / L"1.15-to-1.16.json",
		paths.dataRoot / L"migration-snapshots" / L"1.15"
	});
	if (importedLegacyLocation) {
		MigrationUnitResult locationUnit;
		locationUnit.unitId = L"startup:legacy-location";
		locationUnit.status = MigrationStatus::Succeeded;
		locationUnit.message = L"Imported legacy data from " + importedLegacyLocation->root.wstring()
			+ L". The source files were retained.";
		locationUnit.migratedItems = importedLegacyLocation->historyFile.empty() ? 1 : 2;
		MigrationCoordinator::RecordUnit(locationUnit);
	}
#if MINEBACKUP_ENABLE_V15_MIGRATION
	V15MigrationAdapter::Install();
#endif
	LoadConfigs();
	minebackup::logging::Initialize({
		paths.logsRoot,
		g_logFileLevel,
		false,
		CURRENT_VERSION
	});
	struct LoggingShutdownGuard {
		~LoggingShutdownGuard() { minebackup::logging::Shutdown(); }
	} loggingShutdownGuard;
	MigrationCoordinator::RunStartupMigration();
	CheckForConfigConflicts();
	LoadHistory();
	auto selectAutostartSpecial = [&]() {
		const auto autostartIndex = FindSpecialRunOnStartup(g_appState.specialConfigs);
		if (!autostartIndex) return false;
		// Resolve through the persisted stable identity instead of treating the map
		// index as an external launch contract.
		const auto& stableId = g_appState.specialConfigs.at(*autostartIndex).specialConfigId;
		const int index = FindSpecialConfigByStableId(g_appState.specialConfigs, stableId);
		if (index < 0) return false;
		g_appState.currentConfigIndex = index;
		g_appState.specialConfigMode = true;
		return true;
	};
	if (!launchOptions.runSpecialId.empty()) {
		const int index = FindSpecialConfigByStableId(
			g_appState.specialConfigs,
			launchOptions.runSpecialId);
		if (index < 0) {
			MessageBoxWin("MineBackup", L("REQUESTED_SPECIAL_CONFIG_MISSING"), 2);
			return 4;
		}
		g_appState.currentConfigIndex = index;
		g_appState.specialConfigMode = true;
	}
	else if (!launchOptions.selectConfigId.empty()) {
		const int index = FindConfigByStableId(
			g_appState.configs,
			launchOptions.selectConfigId);
		if (index < 0) {
			MessageBoxWin("MineBackup", L("REQUESTED_CONFIG_MISSING"), 2);
			return 4;
		}
		g_appState.currentConfigIndex = index;
		g_appState.specialConfigMode = false;
	}
	else if (launchOptions.legacySpecialConfigIndex
		&& g_appState.specialConfigs.count(*launchOptions.legacySpecialConfigIndex)) {
		g_appState.currentConfigIndex = *launchOptions.legacySpecialConfigIndex;
		g_appState.specialConfigMode = true;
	}
	else if (launchOptions.autostart) {
		(void)selectAutostartSpecial();
	}
	auto runSelectedSpecialMode = [&]() {
		bool hide = false;
		if (g_appState.specialConfigs.count(g_appState.currentConfigIndex)) {
			hide = g_appState.specialConfigs[g_appState.currentConfigIndex].hideWindow;
		}

		#ifdef _WIN32
		if (!hide) {
			AllocConsole(); // Create a console window
			FILE* pCout, * pCerr, * pCin;
			freopen_s(&pCout, "CONOUT$", "w", stdout);
			freopen_s(&pCerr, "CONOUT$", "w", stderr);
			freopen_s(&pCin, "CONIN$", "r", stdin);
			SetConsoleOutputCP(CP_UTF8);
		}
		#endif
		minebackup::logging::SetConsoleEnabled(!hide);

		RunSpecialMode(g_appState.currentConfigIndex);
		minebackup::logging::SetConsoleEnabled(false);

		#ifdef _WIN32
		if (!hide) {
			FreeConsole();
		}
		#endif
		Sleep(3000);
		return 0;
	};

#ifdef _WIN32
	HWND hwnd_hidden = CreateHiddenWindow(hInstance);
	auto desktopServices = CreateNativeDesktopServices({
		reinterpret_cast<void*>(hInstance), reinterpret_cast<void*>(hwnd_hidden),
		nullptr, GetExecutablePath(), paths.mode != AppPathMode::Explicit});
#else
	auto desktopServices = CreateNativeDesktopServices({
		nullptr, nullptr, nullptr, GetExecutablePath(), paths.mode != AppPathMode::Explicit});
#endif
	InstallDesktopServices(desktopServices);
	if (desktopServices->Capabilities().autostart.IsAvailable()) {
		const bool autostartEnabled = g_RunOnStartup
			|| FindSpecialRunOnStartup(g_appState.specialConfigs).has_value();
		const auto autostartStatus = desktopServices->SetAutostart(autostartEnabled);
		if (!autostartStatus.IsAvailable() && !autostartStatus.diagnostic.empty()) {
			PLATFORM_PRINTF_WARNING("platform.autostart.reconcile_failed",
				"Autostart reconciliation failed: %s",
				wstring_to_utf8(autostartStatus.diagnostic).c_str());
		}
		else if (!autostartStatus.diagnostic.empty()) {
			PLATFORM_PRINTF_INFO("platform.autostart.reconciled",
				"Autostart reconciliation: %s",
				wstring_to_utf8(autostartStatus.diagnostic).c_str());
			if (!launchSilentStartup) {
				MessageBoxWin(L("AUTOSTART_ENTRY_TITLE"),
					wstring_to_utf8(autostartStatus.diagnostic), 0);
			}
		}
	}

	ImGuiRuntime imguiRuntime;
	bool glfwInitialized = false;
	#ifdef __APPLE__
	// The login-item marker is delivered while GLFW pumps Cocoa's launch event.
	// Probe only when a previously selected explicit/special launch does not need
	// the window system, preserving the headless special-mode path.
	if (!g_appState.specialConfigMode) {
		glfwSetErrorCallback(glfw_error_callback);
		if (!glfwInit()) {
			MessageBoxWin(L("FATAL_ERROR_TITLE"), L("GRAPHICS_INIT_ERROR"), 2);
			return 1;
		}
		glfwInitialized = true;
		imguiRuntime.MarkGlfwInitialized();
		if (!hasExplicitLaunchTarget && MacWasLaunchedAsLoginItem()) {
			launchOptions.autostart = true;
			launchSilentStartup = true;
			(void)selectAutostartSpecial();
		}
	}
	#endif


	wstring g_7zTempPath, g_FontTempPath;
	const void* bundledIconFontData = nullptr;
	size_t bundledIconFontSize = 0;
	bool sevenZipExtracted = Extract7zToTempFile(g_7zTempPath);
#ifdef _WIN32
	bool fontExtracted = GetBundledIconFontResource(bundledIconFontData, bundledIconFontSize);
#else

	bool fontExtracted = ExtractFontToTempFile(g_FontTempPath);
#endif

	if (!sevenZipExtracted || !fontExtracted) {
		MessageBoxWin("Error", L("LOG_ERROR_7Z_NOT_FOUND"), 2);
	}

	const auto networkBackend = CreatePlatformNetworkBackend();
	if (g_CheckForUpdates) {
		g_UpdateCheckDone = false;
		g_NewVersionAvailable = false;
		const string currentVersion = CURRENT_VERSION;
		const string language = g_CurrentLang;
		TaskCoordinator::Instance().Submit(L"update-check", {L"network:update"},
			[networkBackend, currentVersion, language](stop_token token) {
				NetworkService network(networkBackend);
				const auto result = CheckMineBackupUpdate(network, currentVersion, language, token);
				TaskEvent event{L"update-check-complete", result.error};
				event.values[L"success"] = result.success ? L"1" : L"0";
				event.values[L"available"] = result.updateAvailable ? L"1" : L"0";
				event.values[L"tag"] = utf8_to_wstring(result.latestTag);
				event.values[L"notes"] = utf8_to_wstring(result.releaseNotes);
				TaskCoordinator::Instance().PostEvent(std::move(event));
			});
	}
	if (g_ReceiveNotices) {
		g_NoticeCheckDone = false;
		g_NewNoticeAvailable = false;
		const string language = g_CurrentLang;
		const string lastSeen = g_NoticeLastSeenVersion;
		TaskCoordinator::Instance().Submit(L"notice-check", {L"network:notice"},
			[networkBackend, language, lastSeen](stop_token token) {
				NetworkService network(networkBackend);
				const auto result = CheckMineBackupNotice(network, language, lastSeen, token);
				TaskEvent event{L"notice-check-complete", result.error};
				event.values[L"success"] = result.success ? L"1" : L"0";
				event.values[L"available"] = result.noticeAvailable ? L"1" : L"0";
				event.values[L"content"] = utf8_to_wstring(result.content);
				event.values[L"content-id"] = utf8_to_wstring(result.contentId);
				TaskCoordinator::Instance().PostEvent(std::move(event));
			});
	}
	TaskCoordinator::Instance().Submit(L"game-session-watcher", {},
		[](stop_token token) { GameSessionWatcherThread(token); });
	const auto knotLinkStartupStatus =
		minebackup::knotlink::GetKnotLinkServerManager().Refresh(true);
	const bool knotLinkStartupNeedsUpdate =
		knotLinkStartupStatus.state ==
		minebackup::knotlink::KnotLinkServerState::Incompatible;
	if (g_enableKnotLink && !knotLinkStartupNeedsUpdate) {
		TaskCoordinator::Instance().Submit(L"knotlink-loader", {L"service:knotlink"}, [](stop_token) {
			if (InitKnotLink()) {
				BroadcastEvent("app_startup", {{"version", CURRENT_VERSION}});
			}
		});
	}

	if (g_appState.specialConfigMode) {
		const int result = runSelectedSpecialMode();
		return result;
	}

	if (!glfwInitialized) {
		glfwSetErrorCallback(glfw_error_callback);
		#ifdef __linux__
		// GLFW 3.4 chooses Wayland or X11 from the current desktop environment.
		// Keep this automatic; capability fallbacks must use the selected backend below.
		glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
		#endif
		if (!glfwInit()) {
			MessageBoxWin(L("FATAL_ERROR_TITLE"), L("GRAPHICS_INIT_ERROR"), 2);
			return 1;
		}
		glfwInitialized = true;
		imguiRuntime.MarkGlfwInitialized();
	}

#if defined(IMGUI_IMPL_OPENGL_ES2)
	const char* glsl_version = "#version 100";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(IMGUI_IMPL_OPENGL_ES3)
	const char* glsl_version = "#version 300 es";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
	const char* glsl_version = "#version 150";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

	auto applyLinuxWindowIdentity = []() {
#ifdef __linux__
		glfwWindowHintString(GLFW_WAYLAND_APP_ID, "io.github.leafuke.MineBackup");
		glfwWindowHintString(GLFW_X11_CLASS_NAME, "MineBackup");
		glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "minebackup");
#endif
	};
	applyLinuxWindowIdentity();

	float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor()); // Valid on GLFW 3.3+ only
	FinalizeUiScaleMigration(main_scale);
	bool errorShow = false;
	bool isFirstRun = !filesystem::exists(paths.ConfigFile());
	static bool showConfigWizard = isFirstRun;
	bool showKnotLinkUpdateReminder =
		!isFirstRun && knotLinkStartupNeedsUpdate;
	bool knotLinkUpdateReminderOpened = false;
	const bool requestedHiddenToTray = launchSilentStartup && !isFirstRun;
#ifdef __linux__
	// Probe the actual AppIndicator/GTK session before deciding to create an
	// invisible window. A compiled-in tray backend may still fail at runtime.
	const auto startupTrayStatus = desktopServices->SetTrayVisible(true);
#else
	const auto startupTrayStatus = desktopServices->Capabilities().tray;
#endif
	const bool shouldStartHiddenToTray = requestedHiddenToTray && startupTrayStatus.IsAvailable();
	if (requestedHiddenToTray && !shouldStartHiddenToTray) {
		const wstring detail = startupTrayStatus.diagnostic.empty()
			? L"The system tray is unavailable in this desktop session."
			: startupTrayStatus.diagnostic;
		PLATFORM_PRINTF_WARNING("platform.tray.startup_fallback",
			"Startup-to-tray was disabled for this run: %s",
			wstring_to_utf8(detail).c_str());
		MessageBoxWin("MineBackup", L("TRAY_FALLBACK_MESSAGE"), 1);
	}
	g_appState.showMainApp = !isFirstRun && !shouldStartHiddenToTray;
	if (isFirstRun) {
		g_windowWidth *= main_scale, g_windowHeight *= main_scale;
		// The monitor DPI is already applied by ImGui's per-viewport font
		// scaling. Keep the persisted value as a user preference only.
		g_uiScale = 1.0f;
	}
	if (shouldStartHiddenToTray) {
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	}

#ifndef _WIN32
	if (isFirstRun) {
#ifdef GLFW_FOCUS_ON_SHOW
		glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
#endif
	}
#endif

	wc = glfwCreateWindow(g_windowWidth, g_windowHeight, "MineBackup", nullptr, nullptr);

#if !defined(IMGUI_IMPL_OPENGL_ES2) && !defined(IMGUI_IMPL_OPENGL_ES3) && !defined(__APPLE__)
	if (wc == nullptr) {
		fprintf(stderr, "OpenGL 3.0 context creation failed, trying OpenGL 2.1 fallback...\n");
		glfwDefaultWindowHints();
		glfwSetErrorCallback(glfw_error_callback);
		applyLinuxWindowIdentity();
		glsl_version = "#version 120";
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
		wc = glfwCreateWindow(g_windowWidth, g_windowHeight, "MineBackup", nullptr, nullptr);
	}
	if (wc == nullptr) {
		fprintf(stderr, "OpenGL 2.1 context creation failed, trying OpenGL 2.0 fallback...\n");
		glfwDefaultWindowHints();
		glfwSetErrorCallback(glfw_error_callback);
		applyLinuxWindowIdentity();
		glsl_version = "#version 110";
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
		wc = glfwCreateWindow(g_windowWidth, g_windowHeight, "MineBackup", nullptr, nullptr);
	}
	if (wc == nullptr) {
		fprintf(stderr, "OpenGL 2.0 context creation failed, trying default version...\n");
		glfwDefaultWindowHints();
		glfwSetErrorCallback(glfw_error_callback);
		applyLinuxWindowIdentity();
		glsl_version = "#version 110";
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 1);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
		wc = glfwCreateWindow(g_windowWidth, g_windowHeight, "MineBackup", nullptr, nullptr);
	}
#endif

	if (wc == nullptr) {
		MessageBoxWin(L("FATAL_ERROR_TITLE"), L("WINDOW_CREATE_ERROR"), 2);
		return 1;

	}
	imguiRuntime.AdoptWindow(wc);
	glfwMakeContextCurrent(wc);
	glfwSwapInterval(1); // Enable vsync

	desktopServices->SetNativeWindow(wc);
#ifdef __linux__
	const int selectedGlfwPlatform = glfwGetPlatform();
	const char* selectedGlfwPlatformName = selectedGlfwPlatform == GLFW_PLATFORM_WAYLAND ? "Wayland"
		: selectedGlfwPlatform == GLFW_PLATFORM_X11 ? "X11" : "Other";
	fprintf(stderr, "[Desktop] GLFW selected platform: %s\n", selectedGlfwPlatformName);
	PLATFORM_PRINTF_INFO("platform.glfw.selected",

		"GLFW selected platform: %s", selectedGlfwPlatformName);
#endif
	const auto traySetup = desktopServices->SetTrayVisible(true);
	if (!traySetup.IsAvailable() && !traySetup.diagnostic.empty()) {
		PLATFORM_PRINTF_WARNING("platform.tray.unavailable",
			"Tray unavailable: %s", wstring_to_utf8(traySetup.diagnostic).c_str());
	}
	auto currentGlobalHotkeys = []() {
		return vector<GlobalHotkeyBinding>{
			{MINEBACKUP_HOTKEY_ID, g_hotKeyBackupId,
				utf8_to_wstring(L("HOTKEY_BACKUP_DESCRIPTION"))},
			{MINERESTORE_HOTKEY_ID, g_hotKeyRestoreId,
				utf8_to_wstring(L("HOTKEY_RESTORE_DESCRIPTION"))}
		};
	};
	const auto hotkeySetup = desktopServices->ConfigureGlobalHotkeys(currentGlobalHotkeys());
	if (!hotkeySetup.IsAvailable() && !hotkeySetup.diagnostic.empty()) {
		PLATFORM_PRINTF_WARNING("platform.hotkey.unavailable",
			"Global hotkeys unavailable: %s",
			wstring_to_utf8(hotkeySetup.diagnostic).c_str());
	}

	glfwSetWindowCloseCallback(wc, [](GLFWwindow* window) {
		if (g_closeAction == 1) {
			glfwSetWindowShouldClose(window, GLFW_FALSE);
			auto services = GetDesktopServices();
			if (CanHideToTray(services->Capabilities())) {
				(void)services->SetTrayVisible(true);
				g_appState.showMainApp = false;
				glfwHideWindow(window);
			}
			else {
				// Preserve the user's preference, but do not make the window unreachable
				// in a session without a tray host.
				glfwIconifyWindow(window);
			}
		} else if (g_closeAction == 2) {
			SaveConfigs();
			g_appState.done = true;
		} else {
			glfwSetWindowShouldClose(window, GLFW_FALSE);
			g_showCloseConfirmDialog = true;
		}
	});

#ifndef _WIN32
	if (isFirstRun) {
		glfwShowWindow(wc);
		glfwFocusWindow(wc);
	}
#endif

#ifdef _WIN32
	int width, height, channels;
	HRSRC hRes = FindResourceW(hInstance, MAKEINTRESOURCEW(102), (LPCWSTR)RT_GROUP_ICON);
	HGLOBAL hMem = LoadResource(hInstance, hRes);
	void* pMem = LockResource(hMem);
	int nId = LookupIconIdFromDirectoryEx((PBYTE)pMem, TRUE, 0, 0, LR_DEFAULTCOLOR);
	hRes = FindResourceW(hInstance, MAKEINTRESOURCEW(nId), (LPCWSTR)RT_ICON);
	hMem = LoadResource(hInstance, hRes);;
	pMem = LockResource(hMem);

	unsigned char* pixels = stbi_load_from_memory((const stbi_uc*)pMem, SizeofResource(hInstance, hRes), &width, &height, &channels, 4);

	if (pixels) {
		GLFWimage images[1];
		images[0].width = width;
		images[0].height = height;
		images[0].pixels = pixels;
		glfwSetWindowIcon(wc, 1, images);
		stbi_image_free(pixels);
	}
#endif

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	imguiRuntime.MarkImGuiContextCreated();

	ImGuiIO& io = ImGui::GetIO();
	const string imguiIniPath = wstring_to_utf8((paths.stateRoot / L"imgui.ini").wstring());
	const string imguiLogPath = wstring_to_utf8((paths.logsRoot / L"imgui_log.txt").wstring());
	io.IniFilename = imguiIniPath.c_str();
	io.LogFilename = imguiLogPath.c_str();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
	bool enableMultiViewport = true;
#ifdef __linux__
	// GLFW intentionally does not expose ImGui platform-window handlers on
	// Wayland. Keeping ViewportsEnable set would make the application call
	// UpdatePlatformWindows with an unavailable backend and can crash on the
	// first frame (notably with a headless Weston compositor).
	enableMultiViewport = selectedGlfwPlatform != GLFW_PLATFORM_WAYLAND;
#endif
	if (enableMultiViewport) {
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;   // Enable Multi-Viewports
		io.ConfigViewportsNoAutoMerge = true;                 // 不自动合并视口
	}

	io.ConfigErrorRecoveryEnableAssert = true;
	io.ConfigErrorRecoveryEnableDebugLog = true;
	io.ConfigErrorRecoveryEnableTooltip = true;

	// Let the platform backend update per-monitor font density. The user scale
	// is applied together with the selected theme by ApplyTheme().
	io.ConfigDpiScaleFonts = true;

	ImGui_ImplGlfw_InitForOpenGL(wc, true);
	imguiRuntime.MarkPlatformBackendInitialized();
#ifdef __EMSCRIPTEN__
	ImGui_ImplGlfw_InstallEmscriptenCallbacks(wc, "#canvas");
#endif
	ImGui_ImplOpenGL3_Init(glsl_version);
	imguiRuntime.MarkRendererBackendInitialized();
	imguiRuntime.SetUiResourceReleaser(ReleaseMainUiResources);

	// Load font sources. The 1.92 renderer texture protocol rasterizes glyphs
	// incrementally, so glyph ranges and an eager atlas Build() are unnecessary.

	ApplyTheme();

	if (isFirstRun) {
		GetUserDefaultUILanguageWin();
		Fontss = GetDefaultUIFontPath();
	}

	static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };

	if (!Fontss.empty() && filesystem::exists(Fontss)) {
		ImFontConfig fontCfg;
		fontCfg.PixelSnapH = true;
		if (fontExtracted) {
			// The 1.92 dynamic font system queries merged sources in order.
			// Reserve Font Awesome's private-use range for the icon source.
			fontCfg.GlyphExcludeRanges = icon_ranges;
		}
		ImFont* mainFont = nullptr;
		mainFont = io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f, &fontCfg);

		if (!mainFont) {
			io.Fonts->AddFontDefaultVector();
		}
	} else {
		io.Fonts->AddFontDefaultVector();
	}

	if (fontExtracted) {
		ImFontConfig config2;
		config2.MergeMode = true;
		config2.PixelSnapH = true;
		config2.GlyphMinAdvanceX = 20.0f; // 图标的宽度

#ifdef _WIN32
		config2.FontDataOwnedByAtlas = false;
		io.Fonts->AddFontFromMemoryTTF(
			const_cast<void*>(bundledIconFontData), static_cast<int>(bundledIconFontSize), 20.0f, &config2);
#else
		io.Fonts->AddFontFromFileTTF(wstring_to_utf8(g_FontTempPath).c_str(), 20.0f, &config2);
#endif
	}

	APP_PRINTF_INFO("application.welcome", L("CONSOLE_WELCOME"));

	AutoDiscoverWorldConfigurations();

	if (isFirstRun)
	{
		ImGuiTheme::ApplyNord(false);
	}

	struct FpsIdling {
		float fpsIdle = 10.0f;        // 空闲时的 FPS
		float fpsActive = 60.0f;      // 活跃时的 FPS
		bool  enableIdling = true;     // 是否启用空闲节能
		bool  isIdling = false;        // 输出：当前是否处于空闲状态
		double lastActivityTime = 0.0; // 上次交互的时间
		double idleTimeout = 3.0;      // 多少秒无交互后进入空闲
	} fpsIdling;

	auto ClockSeconds = []() -> double {
		static const auto start = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		std::chrono::duration<double> elapsed = now - start;
		return elapsed.count();
	};

	fpsIdling.lastActivityTime = ClockSeconds();

	ApplicationEventRouter eventRouter;

	while (!g_appState.done && !glfwWindowShouldClose(wc))
	{
#ifdef __linux__
		PumpLinuxDesktopEvents();
#endif
		wstring instanceError;
		for (const auto& request : singleInstance.PollRequests(instanceError)) {
			g_appState.showMainApp = true;
			const auto activation = desktopServices->ActivateWindow();
			if (!activation.IsAvailable() && !activation.diagnostic.empty()) {
				PLATFORM_PRINTF_WARNING("platform.window.activation_failed",
					"Window activation failed: %s",
					wstring_to_utf8(activation.diagnostic).c_str());
			}
			if (request.type == InstanceRequestType::SelectConfig) {
				const int index = FindConfigByStableId(
					g_appState.configs,
					request.stableId);
				if (index >= 0) {
					g_appState.currentConfigIndex = index;
					g_appState.specialConfigMode = false;
				}
			}
			else if (request.type == InstanceRequestType::RunSpecial) {
				const int index = FindSpecialConfigByStableId(
					g_appState.specialConfigs,
					request.stableId);
				if (index >= 0) {
					g_appState.currentConfigIndex = index;
					RunSpecialMode(index);
				}
			}
		}
		if (!instanceError.empty()) {
			PLATFORM_PRINTF_WARNING("platform.single_instance.poll_failed",
				"Single-instance request failed: %s",
				wstring_to_utf8(instanceError).c_str());
		}
		eventRouter.Dispatch(TaskCoordinator::Instance().PollEvents());
		if (glfwGetWindowAttrib(wc, GLFW_ICONIFIED) != 0 || (!g_appState.showMainApp && !showConfigWizard)) {
			glfwWaitEventsTimeout(1.0);
			continue;
		}

		double nowTime = ClockSeconds();
		{
			ImGuiContext& g = *GImGui;
			bool hasInputEvent = !g.InputEventsQueue.empty();
			if (hasInputEvent || g.ActiveId != 0 || g.MovingWindow != nullptr) {
				fpsIdling.lastActivityTime = nowTime;
			}
		}

		bool shouldIdle = fpsIdling.enableIdling &&
			((nowTime - fpsIdling.lastActivityTime) > fpsIdling.idleTimeout);
		fpsIdling.isIdling = shouldIdle;

		if (shouldIdle) {
			double waitTimeout = 1.0 / (double)fpsIdling.fpsIdle;
			glfwWaitEventsTimeout(waitTimeout);
		} else {
			double waitTimeout = 1.0 / (double)fpsIdling.fpsActive;
			glfwWaitEventsTimeout(waitTimeout);
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		const string migrationPopupTitle = string(L("MIGRATION_STARTUP_TITLE")) + "###MigrationSummary";
		if (MigrationCoordinator::ShouldShowStartupSummary()) {
			ImGui::OpenPopup(migrationPopupTitle.c_str());
		}
		if (ImGui::BeginPopupModal(migrationPopupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 600.0f);
			ImGui::TextWrapped("%s", L("MIGRATION_SUMMARY"));
			const auto migrationReport = MigrationCoordinator::GetMigrationReport();
			for (const auto& unit : migrationReport.units) {
				ImGui::BulletText("%s: %s", MigrationReportUI::UnitLabel(unit.unitId).c_str(),
					MigrationReportUI::StatusLabel(unit.status));
			}
			ImGui::PopTextWrapPos();
			if (ImGui::Button(L("BUTTON_OK"), ImVec2(CalcButtonWidth(L("BUTTON_OK")), 0))) {
				MigrationCoordinator::DismissStartupSummary();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		const string knotLinkUpdatePopupTitle =
			string(L("KNOTLINK_UPDATE_REMINDER_TITLE")) +
			"###KnotLinkUpdateReminder";
		if (showKnotLinkUpdateReminder &&
			!knotLinkUpdateReminderOpened &&
			!MigrationCoordinator::ShouldShowStartupSummary()) {
			ImGui::OpenPopup(knotLinkUpdatePopupTitle.c_str());
			knotLinkUpdateReminderOpened = true;
		}
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		if (ImGui::BeginPopupModal(
				knotLinkUpdatePopupTitle.c_str(),
				&showKnotLinkUpdateReminder,
				ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::PushTextWrapPos(
				ImGui::GetCursorPosX() + GetUiMetrics().Em(30.0f));
			ImGui::TextWrapped("%s", L("KNOTLINK_UPDATE_REMINDER_DESC"));
			ImGui::Spacing();
			ImGui::Text(
				"%s: %s",
				L("KNOTLINK_SERVER_VERSION"),
				knotLinkStartupStatus.version.empty()
					? L("KNOTLINK_VERSION_UNKNOWN")
					: knotLinkStartupStatus.version.c_str());
			ImGui::PopTextWrapPos();
			ImGui::Separator();
			ImGui::BeginDisabled(g_KnotLinkInstallRunning);
			if (ImGui::Button(
				L("KNOTLINK_DOWNLOAD_INSTALLER"),
				ImVec2(-1, 0))) {
				(void)StartKnotLinkInstallerDownload();
			}
			ImGui::EndDisabled();
			if (!g_KnotLinkInstallMessage.empty()) {
				ImGui::TextWrapped(
					"%s",
					wstring_to_utf8(g_KnotLinkInstallMessage).c_str());
			}
			if (ImGui::Button(
				L("BUTTON_OK"),
				ImVec2(CalcButtonWidth(L("BUTTON_OK")), 0))) {
				showKnotLinkUpdateReminder = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (!showConfigWizard && g_appState.showMainApp && g_CoreValidationPending.load() && !g_CoreValidationRunning.load()) {
			StartCoreValidationAsync(true);
		}

		if (showConfigWizard) {
			ShowConfigWizard(showConfigWizard, errorShow, sevenZipExtracted, g_7zTempPath);
		}
		else if (g_appState.showMainApp) {
			DrawMainUiFrame({desktopServices.get(), wc, &paths, currentGlobalHotkeys});
		}
		ImGui::Render();
		int display_w, display_h;
		glfwGetFramebufferSize(wc, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);
		glClearColor(clear_color.x* clear_color.w, clear_color.y* clear_color.w, clear_color.z* clear_color.w, clear_color.w);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			GLFWwindow* backup_current_context = glfwGetCurrentContext();
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
			glfwMakeContextCurrent(backup_current_context);
		}

		glfwSwapBuffers(wc);
	}

	// 清理
	BroadcastEvent("app_shutdown", {});
	TaskCoordinator::Instance().StopAndJoin();
	{
		lock_guard<mutex> lock(g_appState.task_mutex);
		g_appState.g_active_auto_backups.clear();
	}
	glfwGetWindowSize(wc, &g_windowWidth, &g_windowHeight);

	if (filesystem::exists(paths.ConfigFile()))
		SaveConfigs();

	(void)desktopServices->ConfigureGlobalHotkeys({});
	(void)desktopServices->SetTrayVisible(false);
	ResetDesktopServices();
#ifdef _WIN32
	DestroyWindow(hwnd_hidden);
#endif
	imguiRuntime.Shutdown();

	CleanupKnotLink();

	return 0;
}
