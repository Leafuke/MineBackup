# Authoritative explicit source manifest. Keep paths grouped by architectural role.
set(MINEBACKUP_MAIN_SOURCES ${MINEBACKUP_APP_DIR}/MineBackup.cpp)
set(MINEBACKUP_CLI_SOURCES ${MINEBACKUP_SRC_DIR}/cli/CliMain.cpp)

set(MINEBACKUP_DATA_CORE_SOURCES
    ${MINEBACKUP_INFRA_DIR}/AppPaths.cpp
    ${MINEBACKUP_INFRA_DIR}/AtomicFileWriter.cpp
    ${MINEBACKUP_INFRA_DIR}/DiagnosticLogExporter.cpp
    ${MINEBACKUP_INFRA_DIR}/LegacyLocationDiscovery.cpp
    ${MINEBACKUP_INFRA_DIR}/LegacyLocationMigration.cpp
    ${MINEBACKUP_INFRA_DIR}/Logging.cpp
    ${MINEBACKUP_INFRA_DIR}/ProcessRunner.cpp
    ${MINEBACKUP_INFRA_DIR}/ReadOnlyMappedFile.cpp
    ${MINEBACKUP_INFRA_DIR}/KnotLinkPackageManager.cpp
    ${MINEBACKUP_INFRA_DIR}/KnotLinkProtocol.cpp
    ${MINEBACKUP_INFRA_DIR}/NetworkService.cpp
    ${MINEBACKUP_INFRA_DIR}/Sha256.cpp
    ${MINEBACKUP_CORE_DIR}/FolderRewindFormat.cpp
    ${MINEBACKUP_CORE_DIR}/FolderRewindHistoryStore.cpp
    ${MINEBACKUP_CORE_DIR}/FolderRewindMetadataStore.cpp
    ${MINEBACKUP_CORE_DIR}/ArchiveRunner.cpp
    ${MINEBACKUP_CORE_DIR}/BackupChangeDetector.cpp
    ${MINEBACKUP_CORE_DIR}/CloudHistoryAnalysis.cpp
    ${MINEBACKUP_CORE_DIR}/PathRuleSet.cpp
    ${MINEBACKUP_CORE_DIR}/RcloneClient.cpp
    ${MINEBACKUP_CORE_DIR}/RemoteContentService.cpp
    ${MINEBACKUP_CORE_DIR}/ExternalToolManager.cpp
    ${MINEBACKUP_CORE_DIR}/PortableConfigDocument.cpp
    ${MINEBACKUP_CORE_DIR}/SpecialConfigPolicy.cpp
    ${MINEBACKUP_CORE_DIR}/LegacyServicePolicy.cpp
    ${MINEBACKUP_UTILS_DIR}/FileName.cpp
    ${MINEBACKUP_UTILS_DIR}/text_to_text.cpp
)

set(MINEBACKUP_RUNTIME_SOURCES
	${MINEBACKUP_APP_DIR}/ConfigSelection.cpp
	${MINEBACKUP_CORE_DIR}/HistoryRepository.cpp
	${MINEBACKUP_CORE_DIR}/MigrationCoordinator.cpp
	${MINEBACKUP_CORE_DIR}/SpecialTaskDocument.cpp
	${MINEBACKUP_CORE_DIR}/TaskCoordinator.cpp
	${MINEBACKUP_INFRA_DIR}/InterruptedTaskRecovery.cpp
	${MINEBACKUP_INFRA_DIR}/LegacyIniConfigCodec.cpp
	${MINEBACKUP_INFRA_DIR}/SingleInstanceService.cpp
)

set(MINEBACKUP_V15_DATA_SOURCES
    ${MINEBACKUP_CORE_DIR}/LegacyMineBackup15Reader.cpp
)

set(MINEBACKUP_V15_DESKTOP_SOURCES
    ${MINEBACKUP_CORE_DIR}/V15MigrationAdapter.cpp
)

set(MINEBACKUP_APPLICATION_SOURCES
    ${MINEBACKUP_APP_DIR}/Application.cpp
    ${MINEBACKUP_APP_DIR}/ApplicationActions.cpp
    ${MINEBACKUP_APP_DIR}/ApplicationEventRouter.cpp
    ${MINEBACKUP_APP_DIR}/AppearanceRuntime.cpp
	${MINEBACKUP_APP_DIR}/DesktopUiSession.cpp
	${MINEBACKUP_APP_DIR}/DesktopUiLifecycle.cpp
    ${MINEBACKUP_APP_DIR}/AppState.cpp
    ${MINEBACKUP_APP_DIR}/Globals.cpp
    ${MINEBACKUP_APP_DIR}/ImGuiRuntime.cpp
	${MINEBACKUP_APP_DIR}/LaunchOptions.cpp
	${MINEBACKUP_APP_DIR}/legacy/LegacyServiceCleanup.cpp
    ${MINEBACKUP_CORE_DIR}/BackupManager.cpp
    ${MINEBACKUP_CORE_DIR}/BackupAuxiliary.cpp
    ${MINEBACKUP_CORE_DIR}/BackupRestore.cpp
    ${MINEBACKUP_CORE_DIR}/BackupRetention.cpp
    ${MINEBACKUP_CORE_DIR}/CloudHistorySync.cpp
    ${MINEBACKUP_CORE_DIR}/CloudPortableConfig.cpp
    ${MINEBACKUP_CORE_DIR}/CloudSyncService.cpp
    ${MINEBACKUP_CORE_DIR}/CoreValidation.cpp
    ${MINEBACKUP_CORE_DIR}/GameSessionManager.cpp
    ${MINEBACKUP_CORE_DIR}/HistoryManager.cpp
    ${MINEBACKUP_CORE_DIR}/SpecialMode.cpp
    ${MINEBACKUP_INFRA_DIR}/Broadcast.cpp
    ${MINEBACKUP_INFRA_DIR}/ConfigManager.cpp
    ${MINEBACKUP_INFRA_DIR}/KnotLinkServerManager.cpp
    ${MINEBACKUP_INFRA_DIR}/KnotLinkService.cpp
    ${MINEBACKUP_INFRA_DIR}/i18n.cpp
	${MINEBACKUP_INFRA_DIR}/DesktopServices.cpp
)

set(MINEBACKUP_UI_SOURCES
    ${MINEBACKUP_UI_DIR}/CommandConsole.cpp
    ${MINEBACKUP_UI_DIR}/HistoryDialogs.cpp
    ${MINEBACKUP_UI_DIR}/HistoryUI.cpp
	${MINEBACKUP_UI_DIR}/HistoryViewModel.cpp
    ${MINEBACKUP_UI_DIR}/LogPanel.cpp
    ${MINEBACKUP_UI_DIR}/MigrationReportUI.cpp
    ${MINEBACKUP_UI_DIR}/SettingsUI.cpp
    ${MINEBACKUP_UI_DIR}/SettingsUIAppearance.cpp
    ${MINEBACKUP_UI_DIR}/SettingsUIConfig.cpp
    ${MINEBACKUP_UI_DIR}/SettingsUISpecial.cpp
    ${MINEBACKUP_UI_DIR}/SettingsUIHotkeys.cpp
    ${MINEBACKUP_UI_DIR}/WizardUI.cpp
    ${MINEBACKUP_UI_DIR}/MainUI.cpp
    ${MINEBACKUP_UI_DIR}/WorldListController.cpp
	${MINEBACKUP_UI_DIR}/WorldListModel.cpp
    ${MINEBACKUP_UI_DIR}/WorldListUI.cpp
)

set(MINEBACKUP_WINDOWS_SOURCES
    ${MINEBACKUP_PLATFORM_DIR}/Platform_win.cpp
    ${MINEBACKUP_PLATFORM_DIR}/NativeDesktopServices.cpp
    ${MINEBACKUP_PLATFORM_DIR}/NetworkBackend_win.cpp
)
set(MINEBACKUP_LINUX_SOURCES
    ${MINEBACKUP_PLATFORM_DIR}/Platform_linux.cpp
    ${MINEBACKUP_PLATFORM_DIR}/NativeDesktopServices.cpp
    ${MINEBACKUP_PLATFORM_DIR}/LinuxDesktopPortal.cpp
    ${MINEBACKUP_PLATFORM_DIR}/LinuxDesktopPortal.h
    ${MINEBACKUP_PLATFORM_DIR}/NetworkBackend_linux.cpp)
set(MINEBACKUP_MACOS_SOURCES
    ${MINEBACKUP_PLATFORM_DIR}/Platform_macos.cpp
    ${MINEBACKUP_PLATFORM_DIR}/Platform_macos_tray.mm
    ${MINEBACKUP_PLATFORM_DIR}/MacDesktopBridge.mm
    ${MINEBACKUP_PLATFORM_DIR}/NativeDesktopServices.cpp
    ${MINEBACKUP_PLATFORM_DIR}/NetworkBackend_macos.mm)

set(MINEBACKUP_IMGUI_SOURCES
    ${MINEBACKUP_IMGUI_DIR}/imgui.cpp
    ${MINEBACKUP_IMGUI_DIR}/imgui_draw.cpp
    ${MINEBACKUP_IMGUI_DIR}/imgui_tables.cpp
    ${MINEBACKUP_IMGUI_DIR}/imgui_widgets.cpp
    ${MINEBACKUP_IMGUI_DIR}/imgui_impl_glfw.cpp
    ${MINEBACKUP_IMGUI_DIR}/imgui_impl_opengl3.cpp
)

set(MINEBACKUP_SPDLOG_SOURCES
    ${MINEBACKUP_SPDLOG_DIR}/src/async.cpp
    ${MINEBACKUP_SPDLOG_DIR}/src/bundled_fmtlib_format.cpp
    ${MINEBACKUP_SPDLOG_DIR}/src/cfg.cpp
    ${MINEBACKUP_SPDLOG_DIR}/src/color_sinks.cpp
    ${MINEBACKUP_SPDLOG_DIR}/src/file_sinks.cpp
    ${MINEBACKUP_SPDLOG_DIR}/src/spdlog.cpp
    ${MINEBACKUP_SPDLOG_DIR}/src/stdout_sinks.cpp
)

set(MINEBACKUP_PUBLIC_HEADERS
    ${MINEBACKUP_APP_DIR}/Application.h ${MINEBACKUP_APP_DIR}/ApplicationActions.h ${MINEBACKUP_APP_DIR}/ApplicationEventRouter.h ${MINEBACKUP_APP_DIR}/AppearanceRuntime.h ${MINEBACKUP_APP_DIR}/DesktopUiLifecycle.h ${MINEBACKUP_APP_DIR}/DesktopUiSession.h ${MINEBACKUP_APP_DIR}/AppState.h ${MINEBACKUP_APP_DIR}/ConfigSelection.h ${MINEBACKUP_APP_DIR}/DataModels.h ${MINEBACKUP_APP_DIR}/Globals.h ${MINEBACKUP_APP_DIR}/ImGuiRuntime.h ${MINEBACKUP_APP_DIR}/LaunchOptions.h ${MINEBACKUP_APP_DIR}/MainUI.h ${MINEBACKUP_APP_DIR}/legacy/LegacyServiceCleanup.h
    ${MINEBACKUP_CORE_DIR}/ArchiveRunner.h ${MINEBACKUP_CORE_DIR}/BackupChangeDetector.h ${MINEBACKUP_CORE_DIR}/BackupManager.h ${MINEBACKUP_CORE_DIR}/BackupManagerInternal.h ${MINEBACKUP_CORE_DIR}/CloudHistoryAnalysis.h ${MINEBACKUP_CORE_DIR}/CloudSyncInternal.h ${MINEBACKUP_CORE_DIR}/CloudSyncService.h
    ${MINEBACKUP_CORE_DIR}/CoreValidation.h ${MINEBACKUP_CORE_DIR}/FolderRewindFormat.h ${MINEBACKUP_CORE_DIR}/FolderRewindHistoryStore.h ${MINEBACKUP_CORE_DIR}/HistoryRepository.h
    ${MINEBACKUP_CORE_DIR}/FolderRewindMetadataStore.h ${MINEBACKUP_CORE_DIR}/GameSessionManager.h ${MINEBACKUP_CORE_DIR}/HistoryManager.h ${MINEBACKUP_CORE_DIR}/LegacyMineBackup15Reader.h ${MINEBACKUP_CORE_DIR}/PathRuleSet.h
    ${MINEBACKUP_CORE_DIR}/MigrationCoordinator.h ${MINEBACKUP_CORE_DIR}/V15MigrationAdapter.h ${MINEBACKUP_CORE_DIR}/TaskCoordinator.h ${MINEBACKUP_CORE_DIR}/RemoteContentService.h ${MINEBACKUP_CORE_DIR}/ExternalToolManager.h ${MINEBACKUP_CORE_DIR}/PortableConfigDocument.h ${MINEBACKUP_CORE_DIR}/RcloneClient.h ${MINEBACKUP_CORE_DIR}/SpecialConfigPolicy.h ${MINEBACKUP_CORE_DIR}/SpecialTaskDocument.h ${MINEBACKUP_CORE_DIR}/SpecialTaskModels.h ${MINEBACKUP_CORE_DIR}/LegacyServicePolicy.h
    ${MINEBACKUP_INFRA_DIR}/AppPaths.h ${MINEBACKUP_INFRA_DIR}/AtomicFileWriter.h ${MINEBACKUP_INFRA_DIR}/DiagnosticLogExporter.h ${MINEBACKUP_INFRA_DIR}/Logging.h ${MINEBACKUP_INFRA_DIR}/SingleInstanceService.h ${MINEBACKUP_INFRA_DIR}/LegacyIniConfigCodec.h ${MINEBACKUP_INFRA_DIR}/LegacyLocationDiscovery.h ${MINEBACKUP_INFRA_DIR}/LegacyLocationMigration.h ${MINEBACKUP_INFRA_DIR}/ProcessRunner.h ${MINEBACKUP_INFRA_DIR}/ReadOnlyMappedFile.h ${MINEBACKUP_INFRA_DIR}/InterruptedTaskRecovery.h ${MINEBACKUP_INFRA_DIR}/KnotLinkPackageManager.h ${MINEBACKUP_INFRA_DIR}/KnotLinkProtocol.h ${MINEBACKUP_INFRA_DIR}/KnotLinkServerManager.h ${MINEBACKUP_INFRA_DIR}/KnotLinkService.h ${MINEBACKUP_INFRA_DIR}/NetworkService.h ${MINEBACKUP_INFRA_DIR}/Sha256.h ${MINEBACKUP_INFRA_DIR}/Broadcast.h ${MINEBACKUP_INFRA_DIR}/ConfigManager.h ${MINEBACKUP_INFRA_DIR}/i18n.h
    ${MINEBACKUP_PLATFORM_DIR}/DesktopServices.h ${MINEBACKUP_PLATFORM_DIR}/NativeDesktopServices.h ${MINEBACKUP_PLATFORM_DIR}/MacDesktopBridge.h ${MINEBACKUP_PLATFORM_DIR}/PlatformCompat.h ${MINEBACKUP_PLATFORM_DIR}/Platform_linux.h ${MINEBACKUP_PLATFORM_DIR}/Platform_macos.h ${MINEBACKUP_PLATFORM_DIR}/Platform_win.h ${MINEBACKUP_PLATFORM_DIR}/NetworkBackendFactory.h
    ${MINEBACKUP_UI_DIR}/CommandConsole.h ${MINEBACKUP_UI_DIR}/HistoryDialogs.h ${MINEBACKUP_UI_DIR}/HistoryViewModel.h ${MINEBACKUP_UI_DIR}/IconsFontAwesome6.h ${MINEBACKUP_UI_DIR}/LogPanel.h ${MINEBACKUP_UI_DIR}/MainUiController.h ${MINEBACKUP_UI_DIR}/MigrationReportUI.h ${MINEBACKUP_UI_DIR}/SettingsUI.h ${MINEBACKUP_UI_DIR}/SettingsUIHotkeys.h ${MINEBACKUP_UI_DIR}/SettingsUIPrivate.h
    ${MINEBACKUP_UI_DIR}/UIHelpers.h ${MINEBACKUP_UI_DIR}/WorldListController.h ${MINEBACKUP_UI_DIR}/WorldListModel.h ${MINEBACKUP_UI_DIR}/imgui-all.h ${MINEBACKUP_UTILS_DIR}/FileName.h ${MINEBACKUP_UTILS_DIR}/text_to_text.h
)
