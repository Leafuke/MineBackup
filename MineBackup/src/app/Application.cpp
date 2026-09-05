

#include "Application.h"
#include "ApplicationActions.h"
#include "ApplicationEventRouter.h"
#include "AppearanceRuntime.h"
#include "ConfigSelection.h"
#include "DesktopUiSession.h"
#include "DesktopUiLifecycle.h"
#include "Broadcast.h"
#include "Globals.h"
#include "SettingsUI.h"
#include "MigrationReportUI.h"
#include "UIHelpers.h"
#include "MainUI.h"
#include "MainUiController.h"
#include "SettingsUIHotkeys.h"
#include "imgui-all.h"
#include "i18n.h"
#include "AppState.h"
#include "AppPaths.h"
#include "LaunchOptions.h"
#include "legacy/LegacyServiceCleanup.h"
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
#if MINEBACKUP_ENABLE_V15_MIGRATION
#include "V15MigrationAdapter.h"
#endif

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#ifdef __APPLE__
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

static void main_window_size_callback(GLFWwindow* window, int width, int height)
{
	if (window != nullptr && glfwGetWindowAttrib(window, GLFW_ICONIFIED) == 0 && width > 0 && height > 0) {
		g_windowWidth = width;
		g_windowHeight = height;
	}
}

static void main_window_close_callback(GLFWwindow* window)
{
	if (g_OnboardingActive) {
		// 首次引导关闭时直接退出；提交前不得因通用关闭逻辑生成 config.ini。
		g_appState.done = true;
		return;
	}
	if (g_closeAction == 1) {
		glfwSetWindowShouldClose(window, GLFW_FALSE);
		auto services = GetDesktopServices();
		if (CanHideToTray(services->Capabilities())) {
			(void)services->SetTrayVisible(true);
			g_appState.showMainApp = false;
			glfwHideWindow(window);
		}
		else {
			glfwIconifyWindow(window);
		}
	}
	else if (g_closeAction == 2) {
		if (window != nullptr && glfwGetWindowAttrib(window, GLFW_ICONIFIED) == 0) {
			int w = 0, h = 0;
			glfwGetWindowSize(window, &w, &h);
			if (w > 0 && h > 0) {
				g_windowWidth = w;
				g_windowHeight = h;
			}
		}
		if (window != nullptr) {
			glfwHideWindow(window);
		}
		g_appState.done = true;
	}
	else {
		glfwSetWindowShouldClose(window, GLFW_FALSE);
		g_showCloseConfirmDialog = true;
	}
}

namespace {
	void WriteEarlyLaunchError(const wstring& message) {
		const string utf8 = wstring_to_utf8(message);
	#ifdef _WIN32
		HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
		if (output != nullptr && output != INVALID_HANDLE_VALUE) {
			DWORD written = 0;
			(void)WriteFile(output, utf8.data(),
				static_cast<DWORD>(utf8.size()), &written, nullptr);
		}
	#else
		fputs(utf8.c_str(), stderr);
	#endif
	}

	class GlfwProcessLifetime {
	public:
		~GlfwProcessLifetime() {
			if (initialized_) glfwTerminate();
		}
		void MarkInitialized() noexcept { initialized_ = true; }
		[[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
	private:
		bool initialized_ = false;
	};
}

int RunApplication(const ApplicationEntryContext& entryContext)
{
	vector<wstring> launchArguments = entryContext.arguments;
#ifdef _WIN32
	HINSTANCE hInstance = reinterpret_cast<HINSTANCE>(entryContext.nativeInstance);
#endif
	const filesystem::path originalWorkingDirectory = filesystem::current_path();
	(void)originalWorkingDirectory;
	LaunchOptions launchOptions;
	wstring launchError;
	if (!ParseLaunchOptions(launchArguments, launchOptions, launchError)) {
		const bool deprecatedSpecialRequest = any_of(
			launchArguments.begin(), launchArguments.end(), [](const wstring& argument) {
				return argument == L"--run-special"
					|| argument == L"-specialcfg";
			});
		if (deprecatedSpecialRequest) {
			WriteEarlyLaunchError(
				L"MineBackup: SpecialConfig and run-special have been removed.\n"
				L"Create a Job and run it with minebackup-cli job run --job <JobId>.\n");
			return 2;
		}
		MessageBoxWin("MineBackup", wstring_to_utf8(launchError), 2);
		return 2;
	}
	// Use the host language for any pre-configuration native prompts. Loading an
	// existing profile below will still restore the user's explicit app language.
	GetUserDefaultUILanguageWin();
	if (!launchOptions.legacyServiceCleanup.empty()) {
		wstring cleanupError;
		if (!LegacyServiceCleanup::RemoveAfterValidation(
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
	if (!ResolveAppPaths(
			AppPathRequest{launchOptions.dataDirectory},
			GetExecutablePath(), appPaths, launchError)) {
		MessageBoxWin("MineBackup", wstring_to_utf8(launchError), 2);
		return 2;
	}
	SetCurrentAppPaths(std::move(appPaths));
	// --autostart 只是系统登录启动项传入的内部标记，不再触发特殊任务执行。
	// 是否隐藏到托盘要等配置加载后结合 GUI 设置决定；显式 --silent-startup
	// 仍然可以直接强制使用静默启动。
	bool launchSilentStartup = launchOptions.silentStartup;
	const auto& paths = GetAppPaths();
	SingleInstanceService singleInstance;
	const auto instanceResult = singleInstance.Acquire(paths.profileIdentity, paths.runtimeRoot, launchError);
	if (instanceResult == InstanceAcquireResult::AlreadyRunning) {
		// 登录启动不应把已经打开的 GUI 窗口抢到前台；普通重复启动仍保持
		// 原有的激活行为。
		if (launchOptions.autostart && launchOptions.selectConfigId.empty()) return 0;
		InstanceRequest request;
		if (!launchOptions.selectConfigId.empty()) {
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
	if (launchOptions.autostart && g_SilentStartupToTray) {
		launchSilentStartup = true;
	}
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
	if (!launchOptions.selectConfigId.empty()) {
		const int index = FindConfigByStableId(
			g_appState.configs,
			launchOptions.selectConfigId);
		if (index < 0) {
			MessageBoxWin("MineBackup", L("REQUESTED_CONFIG_MISSING"), 2);
			return 4;
		}
		g_appState.currentConfigIndex = index;
	}

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
		// 每次 GUI 启动都校正一次登录启动项：既能修复程序路径变化，
		// 也能清理旧版按特殊任务创建的启动项。特殊配置的 runOnStartup
		// 字段不再参与这里的决策，避免与 GUI 自启动概念混淆。
		const auto autostartStatus = desktopServices->SetAutostart(g_RunOnStartup);
		if (!autostartStatus.IsAvailable() && !autostartStatus.diagnostic.empty()) {
			PLATFORM_PRINTF_WARNING("platform.autostart.reconcile_failed",
				"Autostart reconciliation failed: %s",
				wstring_to_utf8(autostartStatus.diagnostic).c_str());
		}
		else if (!autostartStatus.diagnostic.empty()) {
			PLATFORM_PRINTF_INFO("platform.autostart.reconciled",
				"GUI login startup entry synchronized: %s",
				wstring_to_utf8(autostartStatus.diagnostic).c_str());
		}
	}

	GlfwProcessLifetime glfwLifetime;
	#ifdef __APPLE__
	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit()) {
		MessageBoxWin(L("FATAL_ERROR_TITLE"), L("GRAPHICS_INIT_ERROR"), 2);
		return 1;
	}
	glfwLifetime.MarkInitialized();
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
	// KnotLink 状态检测包含两次回环 TCP 连接;即使每次有 250ms 上限,防火墙
	// 丢弃 SYN 时仍会推迟首帧。检测与初始化全部放入后台任务,状态经
	// knotlink-startup-status 事件回传,主循环再决定是否弹更新提醒。
	TaskCoordinator::Instance().Submit(L"knotlink-loader", {L"service:knotlink"},
		[](stop_token) {
			const auto status =
				minebackup::knotlink::GetKnotLinkServerManager().Refresh(true);
			TaskEvent event{L"knotlink-startup-status", L""};
			event.values[L"needs-update"] =
				status.state ==
						minebackup::knotlink::KnotLinkServerState::Incompatible
					? L"1"
					: L"0";
			event.values[L"version"] = utf8_to_wstring(status.version);
			TaskCoordinator::Instance().PostEvent(std::move(event));
			if (g_enableKnotLink &&
				status.state !=
					minebackup::knotlink::KnotLinkServerState::Incompatible) {
				if (InitKnotLink()) {
					BroadcastEvent("app_startup", {{"version", CURRENT_VERSION}});
				}
			}
		});

	if (!glfwLifetime.IsInitialized()) {
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
		glfwLifetime.MarkInitialized();
	}

	float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor()); // Valid on GLFW 3.3+ only
	FinalizeUiScaleMigration(main_scale);
	bool isFirstRun = !filesystem::exists(paths.ConfigFile());
	static bool showConfigWizard = isFirstRun;
	g_OnboardingActive = isFirstRun;
	bool showKnotLinkUpdateReminder = false;
	bool knotLinkUpdateReminderOpened = false;
	bool knotLinkStartupStatusHandled = false;
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
	DesktopUiSession uiSession;
	DesktopUiSessionOptions uiOptions;
	uiOptions.paths = &paths;
	uiOptions.width = g_windowWidth;
	uiOptions.height = g_windowHeight;
	uiOptions.iconFontAvailable = fontExtracted;
	uiOptions.bundledIconFontData = bundledIconFontData;
	uiOptions.bundledIconFontSize = bundledIconFontSize;
	uiOptions.extractedIconFontPath = g_FontTempPath;
#ifdef _WIN32
	uiOptions.nativeInstance = hInstance;
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

	bool uiSessionCreatedOnce = false;
	auto createUiSession = [&](bool initiallyVisible) {
		uiOptions.width = g_windowWidth;
		uiOptions.height = g_windowHeight;
		uiOptions.initiallyVisible = initiallyVisible;
		uiOptions.firstRun = isFirstRun && !uiSessionCreatedOnce;
		wstring uiCreateError;
		if (!uiSession.Create(uiOptions, uiCreateError)) {
			PLATFORM_PRINTF_ERROR("ui.session.create_failed",
				"Desktop UI session creation failed: %s",
				wstring_to_utf8(uiCreateError).c_str());
			return false;
		}
		uiSessionCreatedOnce = true;
		wc = uiSession.Window();
		desktopServices->SetNativeWindow(wc);
		glfwSetWindowCloseCallback(wc, main_window_close_callback);
		glfwSetWindowSizeCallback(wc, main_window_size_callback);
		return true;
	};

	bool startUiCold = false;
#ifdef _WIN32
	startUiCold = shouldStartHiddenToTray;
#endif
	if (!startUiCold && !createUiSession(!shouldStartHiddenToTray)) {
		MessageBoxWin(L("FATAL_ERROR_TITLE"), L("WINDOW_CREATE_ERROR"), 2);
		return 1;
	}
	DesktopUiLifecycle uiLifecycle(startUiCold
		? DesktopUiState::HiddenCold : DesktopUiState::Visible);
	const bool canUnloadForTray =
#ifdef _WIN32
		traySetup.IsAvailable() && CanHideToTray(desktopServices->Capabilities());
#else
		false;
#endif

	APP_PRINTF_INFO("application.welcome", L("CONSOLE_WELCOME"));

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
	auto activateUiWindow = [&]() {
		if (wc == nullptr) return;
		glfwShowWindow(wc);
		if (glfwGetWindowAttrib(wc, GLFW_ICONIFIED) != 0) glfwRestoreWindow(wc);
		glfwFocusWindow(wc);
		const auto activation = desktopServices->ActivateWindow();
		if (!activation.IsAvailable() && !activation.diagnostic.empty()) {
			PLATFORM_PRINTF_WARNING("platform.window.activation_failed",
				"Window activation failed: %s",
				wstring_to_utf8(activation.diagnostic).c_str());
		}
	};
	auto unloadUiSession = [&]() {
		if (!uiSession.IsActive()) return;
		uiSession.SaveWindowState(g_windowWidth, g_windowHeight);
		const ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
		const ImTextureData* atlasTexture = ImGui::GetIO().Fonts->TexData;
		APP_PRINTF_INFO("ui.session.unloading",
			"Unloading desktop UI session: mapped_font_bytes=%zu atlas=%dx%d "
			"atlas_format=%d viewports=%d",
			uiSession.MappedFontBytes(),
			atlasTexture ? atlasTexture->Width : 0,
			atlasTexture ? atlasTexture->Height : 0,
			atlasTexture ? static_cast<int>(atlasTexture->Format) : -1,
			platformIo.Viewports.Size);
		desktopServices->SetNativeWindow(nullptr);
		uiSession.Shutdown();
		ReleaseHistoryWindowCaches();
		wc = nullptr;
	};

	while (!g_appState.done)
	{
#ifdef __linux__
		PumpLinuxDesktopEvents();
#endif
		wstring instanceError;
		bool activationRequested = false;
		for (const auto& request : singleInstance.PollRequests(instanceError)) {
			g_appState.showMainApp = true;
			activationRequested = true;
			if (request.type == InstanceRequestType::SelectConfig) {
				const int index = FindConfigByStableId(
					g_appState.configs,
					request.stableId);
				if (index >= 0) {
					g_appState.currentConfigIndex = index;
				}
			}
		}
		if (!instanceError.empty()) {
			PLATFORM_PRINTF_WARNING("platform.single_instance.poll_failed",
				"Single-instance request failed: %s",
				wstring_to_utf8(instanceError).c_str());
		}
		eventRouter.Dispatch(TaskCoordinator::Instance().PollEvents());
		if (!knotLinkStartupStatusHandled && g_KnotLinkStartupStatusReady) {
			knotLinkStartupStatusHandled = true;
			showKnotLinkUpdateReminder =
				!isFirstRun && g_KnotLinkStartupNeedsUpdate;
		}

#ifdef _WIN32
		if (canUnloadForTray && !showConfigWizard) {
			if (!g_appState.showMainApp
				&& uiLifecycle.State() == DesktopUiState::Visible) {
				if (uiLifecycle.HideToTray(DesktopUiLifecycle::Clock::now())
					== DesktopUiAction::HideWarm) {
					uiSession.DestroySecondaryPlatformWindows();
					APP_PRINTF_INFO("ui.session.hidden_warm",
						"Desktop UI hidden to tray; cold unload scheduled in 10 seconds");
				}
			}
			if (g_appState.showMainApp
				&& uiLifecycle.State() != DesktopUiState::Visible) {
				const DesktopUiAction showAction = uiLifecycle.RequestShow();
				if (showAction == DesktopUiAction::ShowExisting) {
					activateUiWindow();
				}
				else if (showAction == DesktopUiAction::CreateAndShow) {
					const bool restored = createUiSession(false);
					uiLifecycle.CompleteColdRestore(restored);
					if (!restored) {
						MessageBoxWin(L("FATAL_ERROR_TITLE"), L("WINDOW_CREATE_ERROR"), 2);
						g_appState.done = true;
						break;
					}
					knotLinkUpdateReminderOpened = false;
					APP_PRINTF_INFO("ui.session.cold_restored",
						"Desktop UI session restored in %.2f ms",
						uiSession.LastCreateMilliseconds());
					activateUiWindow();
				}
			}
			if (!g_appState.showMainApp
				&& uiLifecycle.Tick(DesktopUiLifecycle::Clock::now())
					== DesktopUiAction::UnloadSession) {
				unloadUiSession();
			}
		}
#endif
		if (activationRequested && uiSession.IsActive()
			&& g_appState.showMainApp) {
			activateUiWindow();
		}
		if (!uiSession.IsActive()) {
			glfwWaitEventsTimeout(0.25);
			continue;
		}
		if (glfwWindowShouldClose(wc)) {
			g_appState.done = true;
			break;
		}
		if (glfwGetWindowAttrib(wc, GLFW_ICONIFIED) != 0
			|| (!g_appState.showMainApp && !showConfigWizard)) {
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
        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        if (ImGui::BeginPopupModal(migrationPopupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(
                ImGui::GetCursorPosX() + GetUiMetrics().Em(30.0f));
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
				g_KnotLinkStartupVersion.empty()
					? L("KNOTLINK_VERSION_UNKNOWN")
					: g_KnotLinkStartupVersion.c_str());
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
			ShowConfigWizard(showConfigWizard);
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

		if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
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
	const auto shutdownStart = std::chrono::steady_clock::now();

	TaskCoordinator::Instance().StopAndJoin();
	{
		lock_guard<mutex> lock(g_appState.task_mutex);
		g_appState.g_active_auto_backups.clear();
	}
	const auto tasksStopped = std::chrono::steady_clock::now();
	APP_PRINTF_INFO("application.shutdown.tasks_stopped", "Tasks stopped in %lld ms",
		static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(tasksStopped - shutdownStart).count()));

	uiSession.SaveWindowState(g_windowWidth, g_windowHeight);
	if (filesystem::exists(paths.ConfigFile()))
		SaveConfigs();
	const auto configSaved = std::chrono::steady_clock::now();
	APP_PRINTF_INFO("application.shutdown.config_saved", "Configs saved in %lld ms",
		static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(configSaved - tasksStopped).count()));

	(void)desktopServices->ConfigureGlobalHotkeys({});
	(void)desktopServices->SetTrayVisible(false);
	unloadUiSession();
	ReleaseMainUiResources();
	ResetDesktopServices();
#ifdef _WIN32
	DestroyWindow(hwnd_hidden);
#endif
	const auto uiReleased = std::chrono::steady_clock::now();
	APP_PRINTF_INFO("application.shutdown.ui_released", "UI resources released in %lld ms",
		static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(uiReleased - configSaved).count()));

	CleanupKnotLink();
	const auto shutdownCompleted = std::chrono::steady_clock::now();
	APP_PRINTF_INFO("application.shutdown.completed", "Shutdown completed in %lld ms",
		static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(shutdownCompleted - shutdownStart).count()));

	return 0;
}
