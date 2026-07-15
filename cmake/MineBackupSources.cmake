# Authoritative explicit source manifest. Keep paths grouped by architectural role.
set(MINEBACKUP_MAIN_SOURCES ${MINEBACKUP_APP_DIR}/MineBackup.cpp)

set(MINEBACKUP_DATA_CORE_SOURCES
    ${MINEBACKUP_APP_DIR}/LaunchOptions.cpp
    ${MINEBACKUP_INFRA_DIR}/AppPaths.cpp
    ${MINEBACKUP_INFRA_DIR}/AtomicFileWriter.cpp
    ${MINEBACKUP_INFRA_DIR}/RotatingFileLog.cpp
    ${MINEBACKUP_INFRA_DIR}/SingleInstanceService.cpp
    ${MINEBACKUP_INFRA_DIR}/LegacyLocationDiscovery.cpp
    ${MINEBACKUP_INFRA_DIR}/LegacyLocationMigration.cpp
    ${MINEBACKUP_INFRA_DIR}/ProcessRunner.cpp
    ${MINEBACKUP_INFRA_DIR}/InterruptedTaskRecovery.cpp
    ${MINEBACKUP_INFRA_DIR}/NetworkService.cpp
    ${MINEBACKUP_INFRA_DIR}/Sha256.cpp
    ${MINEBACKUP_INFRA_DIR}/DesktopServices.cpp
    ${MINEBACKUP_CORE_DIR}/FolderRewindFormat.cpp
    ${MINEBACKUP_CORE_DIR}/FolderRewindHistoryStore.cpp
    ${MINEBACKUP_CORE_DIR}/FolderRewindMetadataStore.cpp
    ${MINEBACKUP_CORE_DIR}/MigrationCoordinator.cpp
    ${MINEBACKUP_CORE_DIR}/TaskCoordinator.cpp
    ${MINEBACKUP_CORE_DIR}/RemoteContentService.cpp
    ${MINEBACKUP_CORE_DIR}/ExternalToolManager.cpp
    ${MINEBACKUP_CORE_DIR}/PortableConfigDocument.cpp
    ${MINEBACKUP_CORE_DIR}/SpecialConfigPolicy.cpp
    ${MINEBACKUP_CORE_DIR}/LegacyServicePolicy.cpp
    ${MINEBACKUP_UTILS_DIR}/text_to_text.cpp
)

set(MINEBACKUP_V15_DATA_SOURCES
    ${MINEBACKUP_CORE_DIR}/LegacyMineBackup15Reader.cpp
)

set(MINEBACKUP_V15_DESKTOP_SOURCES
    ${MINEBACKUP_CORE_DIR}/V15MigrationAdapter.cpp
)

set(MINEBACKUP_APPLICATION_SOURCES
    ${MINEBACKUP_APP_DIR}/AppState.cpp
    ${MINEBACKUP_APP_DIR}/Globals.cpp
    ${MINEBACKUP_CORE_DIR}/BackupManager.cpp
    ${MINEBACKUP_CORE_DIR}/CloudSyncService.cpp
    ${MINEBACKUP_CORE_DIR}/CoreValidation.cpp
    ${MINEBACKUP_CORE_DIR}/GameSessionManager.cpp
    ${MINEBACKUP_CORE_DIR}/HistoryManager.cpp
    ${MINEBACKUP_CORE_DIR}/SpecialMode.cpp
    ${MINEBACKUP_CORE_DIR}/TaskSystem.cpp
    ${MINEBACKUP_INFRA_DIR}/Broadcast.cpp
    ${MINEBACKUP_INFRA_DIR}/ConfigManager.cpp
    ${MINEBACKUP_INFRA_DIR}/Console.cpp
    ${MINEBACKUP_INFRA_DIR}/i18n.cpp
    ${MINEBACKUP_UTILS_DIR}/basic_func.cpp
)

set(MINEBACKUP_UI_SOURCES
    ${MINEBACKUP_UI_DIR}/HistoryUI.cpp
    ${MINEBACKUP_UI_DIR}/MigrationReportUI.cpp
    ${MINEBACKUP_UI_DIR}/SettingsUI.cpp
    ${MINEBACKUP_UI_DIR}/SettingsUIAppearance.cpp
    ${MINEBACKUP_UI_DIR}/SettingsUIConfig.cpp
    ${MINEBACKUP_UI_DIR}/SettingsUISpecial.cpp
    ${MINEBACKUP_UI_DIR}/WizardUI.cpp
)

set(MINEBACKUP_WINDOWS_SOURCES
    ${MINEBACKUP_PLATFORM_DIR}/Platform_win.cpp
    ${MINEBACKUP_PLATFORM_DIR}/NativeDesktopServices.cpp
    ${MINEBACKUP_PLATFORM_DIR}/NetworkBackend_win.cpp
    ${MINEBACKUP_KNOTLINK_DIR}/OpenSocketResponser.cpp
    ${MINEBACKUP_KNOTLINK_DIR}/SignalSender.cpp
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

set(MINEBACKUP_PUBLIC_HEADERS
    ${MINEBACKUP_APP_DIR}/AppState.h ${MINEBACKUP_APP_DIR}/DataModels.h ${MINEBACKUP_APP_DIR}/Globals.h ${MINEBACKUP_APP_DIR}/LaunchOptions.h ${MINEBACKUP_APP_DIR}/MainUI.h
    ${MINEBACKUP_CORE_DIR}/BackupManager.h ${MINEBACKUP_CORE_DIR}/BackupManagerRestore.inl ${MINEBACKUP_CORE_DIR}/CloudSyncService.h
    ${MINEBACKUP_CORE_DIR}/CoreValidation.h ${MINEBACKUP_CORE_DIR}/FolderRewindFormat.h ${MINEBACKUP_CORE_DIR}/FolderRewindHistoryStore.h
    ${MINEBACKUP_CORE_DIR}/FolderRewindMetadataStore.h ${MINEBACKUP_CORE_DIR}/HistoryManager.h ${MINEBACKUP_CORE_DIR}/LegacyMineBackup15Reader.h
    ${MINEBACKUP_CORE_DIR}/MigrationCoordinator.h ${MINEBACKUP_CORE_DIR}/V15MigrationAdapter.h ${MINEBACKUP_CORE_DIR}/TaskSystem.h ${MINEBACKUP_CORE_DIR}/TaskCoordinator.h ${MINEBACKUP_CORE_DIR}/RemoteContentService.h ${MINEBACKUP_CORE_DIR}/ExternalToolManager.h ${MINEBACKUP_CORE_DIR}/PortableConfigDocument.h ${MINEBACKUP_CORE_DIR}/SpecialConfigPolicy.h ${MINEBACKUP_CORE_DIR}/LegacyServicePolicy.h
    ${MINEBACKUP_INFRA_DIR}/AppPaths.h ${MINEBACKUP_INFRA_DIR}/AtomicFileWriter.h ${MINEBACKUP_INFRA_DIR}/RotatingFileLog.h ${MINEBACKUP_INFRA_DIR}/SingleInstanceService.h ${MINEBACKUP_INFRA_DIR}/LegacyLocationDiscovery.h ${MINEBACKUP_INFRA_DIR}/LegacyLocationMigration.h ${MINEBACKUP_INFRA_DIR}/ProcessRunner.h ${MINEBACKUP_INFRA_DIR}/InterruptedTaskRecovery.h ${MINEBACKUP_INFRA_DIR}/NetworkService.h ${MINEBACKUP_INFRA_DIR}/Sha256.h ${MINEBACKUP_INFRA_DIR}/Broadcast.h ${MINEBACKUP_INFRA_DIR}/ConfigManager.h ${MINEBACKUP_INFRA_DIR}/Console.h ${MINEBACKUP_INFRA_DIR}/i18n.h
    ${MINEBACKUP_PLATFORM_DIR}/DesktopServices.h ${MINEBACKUP_PLATFORM_DIR}/NativeDesktopServices.h ${MINEBACKUP_PLATFORM_DIR}/MacDesktopBridge.h ${MINEBACKUP_PLATFORM_DIR}/PlatformCompat.h ${MINEBACKUP_PLATFORM_DIR}/Platform_linux.h ${MINEBACKUP_PLATFORM_DIR}/Platform_macos.h ${MINEBACKUP_PLATFORM_DIR}/Platform_win.h ${MINEBACKUP_PLATFORM_DIR}/NetworkBackendFactory.h
    ${MINEBACKUP_UI_DIR}/IconsFontAwesome6.h ${MINEBACKUP_UI_DIR}/MigrationReportUI.h ${MINEBACKUP_UI_DIR}/SettingsUI.h ${MINEBACKUP_UI_DIR}/SettingsUIPrivate.h
    ${MINEBACKUP_UI_DIR}/UIHelpers.h ${MINEBACKUP_UI_DIR}/imgui-all.h ${MINEBACKUP_UTILS_DIR}/text_to_text.h
)
