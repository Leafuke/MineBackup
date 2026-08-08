# MineBackup Runtime Separation 源码审计

本清单记录 ADR 0006 落地前 `MineBackup/src` 下全部 C++、Objective-C++ 源文件和头文件的目标归属。目标层含义：

- `data_core`：纯模型、格式、算法和可复用基础设施。
- `runtime`：配置、历史、备份、任务及无桌面集成。
- `desktop`：Application、GUI、托盘、弹框和桌面生命周期。
- `split`：必须在本里程碑拆出 runtime 与 desktop 两部分。
- `legacy-desktop`：只为旧数据或旧系统集成保留，CLI 不链接。
- `delete/legacy`：删除未使用实现，只保留明确需要的兼容能力。

## 依赖禁令

`minebackup_runtime` 的传递闭包不得包含 `Globals.h`、`AppState.h`、`DesktopServices.h`、`imgui.h`、GLFW、OpenGL、X11、Wayland、GTK、GIO 或 AppIndicator。GUI 允许依赖 runtime；runtime 不允许反向依赖 GUI。

## 逐文件清单

| 文件 | 目标 | 动作 |
|---|---|---|
| `MineBackup/src/app/AppearanceRuntime.cpp` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/AppearanceRuntime.h` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/Application.cpp` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/Application.h` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/ApplicationActions.cpp` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/ApplicationActions.h` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/ApplicationEventRouter.cpp` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/ApplicationEventRouter.h` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/AppState.cpp` | desktop | 保留为 GUI 兼容镜像，移除 runtime 访问 |
| `MineBackup/src/app/AppState.h` | desktop | 保留为 GUI 兼容镜像，移除 runtime 访问 |
| `MineBackup/src/app/ConfigSelection.cpp` | runtime | 迁移为稳定 ID/路径解析 |
| `MineBackup/src/app/ConfigSelection.h` | runtime | 迁移为稳定 ID/路径解析 |
| `MineBackup/src/app/DataModels.h` | data_core | 拆分为纯领域 DTO，移除 PlatformCompat |
| `MineBackup/src/app/DesktopUiLifecycle.cpp` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/DesktopUiLifecycle.h` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/DesktopUiSession.cpp` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/DesktopUiSession.h` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/Globals.cpp` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/Globals.h` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/ImGuiRuntime.cpp` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/ImGuiRuntime.h` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/LaunchOptions.cpp` | desktop | 保留 GUI 参数；CLI 使用独立解析器 |
| `MineBackup/src/app/LaunchOptions.h` | desktop | 保留 GUI 参数；CLI 使用独立解析器 |
| `MineBackup/src/app/legacy/LegacyServiceCleanup.cpp` | legacy-desktop | 仅检查和卸载已验证的旧 Windows Service |
| `MineBackup/src/app/legacy/LegacyServiceCleanup.h` | legacy-desktop | 不向 runtime 暴露服务控制 API |
| `MineBackup/src/app/MainUI.h` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/app/MineBackup.cpp` | desktop | 保留桌面生命周期或 UI 状态 |
| `MineBackup/src/cli/CliMain.cpp` | runtime | CLI console 入口；不进入 GUI MSBuild 工程 |
| `MineBackup/src/core/ArchiveRunner.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/ArchiveRunner.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/BackupAuxiliary.cpp` | split | 抽运行时引擎/端口，呈现与未纳入 CLI 的功能留 desktop |
| `MineBackup/src/core/BackupChangeDetector.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/BackupChangeDetector.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/BackupManager.cpp` | split | 抽运行时引擎/端口，呈现与未纳入 CLI 的功能留 desktop |
| `MineBackup/src/core/BackupManagerDesktop.cpp` | desktop | 将 GUI 全局状态、KnotLink 广播与异步云上传映射到 BackupService |
| `MineBackup/src/core/BackupManager.h` | split | 抽运行时引擎/端口，呈现与未纳入 CLI 的功能留 desktop |
| `MineBackup/src/core/BackupManagerInternal.h` | split | 抽运行时引擎/端口，呈现与未纳入 CLI 的功能留 desktop |
| `MineBackup/src/core/BackupService.h` | runtime | 显式备份请求、端口依赖与可取消运行时服务 |
| `MineBackup/src/core/BackupRestore.cpp` | split | 抽运行时引擎/端口，呈现与未纳入 CLI 的功能留 desktop |
| `MineBackup/src/core/BackupRetention.cpp` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/CloudHistoryAnalysis.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/CloudHistoryAnalysis.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/CloudHistorySync.cpp` | split | 抽运行时引擎/端口，呈现与未纳入 CLI 的功能留 desktop |
| `MineBackup/src/core/CloudPortableConfig.cpp` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/CloudSyncInternal.h` | split | 抽运行时引擎/端口，呈现与未纳入 CLI 的功能留 desktop |
| `MineBackup/src/core/CloudSyncService.cpp` | split | 抽运行时引擎/端口，呈现与未纳入 CLI 的功能留 desktop |
| `MineBackup/src/core/CloudSyncService.h` | split | 抽运行时引擎/端口，呈现与未纳入 CLI 的功能留 desktop |
| `MineBackup/src/core/CoreValidation.cpp` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/CoreValidation.h` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/ExternalToolManager.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/ExternalToolManager.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/FolderRewindFormat.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/FolderRewindFormat.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/FolderRewindHistoryStore.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/FolderRewindHistoryStore.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/FolderRewindMetadataStore.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/FolderRewindMetadataStore.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/GameSessionManager.cpp` | desktop | 保留桌面 watcher，调用 runtime 服务 |
| `MineBackup/src/core/GameSessionManager.h` | desktop | 保留桌面 watcher，调用 runtime 服务 |
| `MineBackup/src/core/HistoryManager.cpp` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/HistoryManager.h` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/HistoryRepository.cpp` | runtime | 线程安全历史存储与不可变快照 |
| `MineBackup/src/core/HistoryRepository.h` | runtime | 线程安全历史存储与不可变快照 |
| `MineBackup/src/core/LegacyMineBackup15Reader.cpp` | legacy-desktop | 仅 GUI 链接的 v1.15 adapter |
| `MineBackup/src/core/LegacyMineBackup15Reader.h` | legacy-desktop | 仅 GUI 链接的 v1.15 adapter |
| `MineBackup/src/core/LegacyServicePolicy.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/LegacyServicePolicy.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/MigrationCoordinator.cpp` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/MigrationCoordinator.h` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/OperationResult.cpp` | runtime | 固定操作码、聚合规则与进程退出码映射 |
| `MineBackup/src/core/OperationResult.h` | runtime | 纯运行时结果、诊断和任务结果契约 |
| `MineBackup/src/core/PathRuleSet.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/PathRuleSet.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/PortableConfigDocument.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/PortableConfigDocument.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/RcloneClient.cpp` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/RcloneClient.h` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/RemoteContentService.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/RemoteContentService.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/SpecialConfigPolicy.cpp` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/SpecialConfigPolicy.h` | data_core | 保留纯格式、策略或工具逻辑 |
| `MineBackup/src/core/SpecialTaskDocument.cpp` | runtime | 版本化特殊任务 codec、迁移与原子存储 |
| `MineBackup/src/core/SpecialTaskDocument.h` | runtime | 暴露无桌面任务文档契约 |
| `MineBackup/src/core/SpecialTaskModels.h` | data_core | 保留纯特殊任务领域 DTO |
| `MineBackup/src/core/SpecialMode.cpp` | split | 抽运行时引擎/端口，呈现与未纳入 CLI 的功能留 desktop |
| `MineBackup/src/core/TaskCoordinator.cpp` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/TaskCoordinator.h` | runtime | 迁移为显式依赖的运行时服务 |
| `MineBackup/src/core/V15MigrationAdapter.cpp` | legacy-desktop | 仅 GUI 链接的 v1.15 adapter |
| `MineBackup/src/core/V15MigrationAdapter.h` | legacy-desktop | 仅 GUI 链接的 v1.15 adapter |
| `MineBackup/src/infra/AppPaths.cpp` | data_core | 解耦 LaunchOptions 后保留 |
| `MineBackup/src/infra/AppPaths.h` | data_core | 解耦 LaunchOptions 后保留 |
| `MineBackup/src/infra/AtomicFileWriter.cpp` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/AtomicFileWriter.h` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/Broadcast.cpp` | split | 抽 runtime event sink，桌面广播留 adapter |
| `MineBackup/src/infra/Broadcast.h` | split | 抽 runtime event sink，桌面广播留 adapter |
| `MineBackup/src/infra/ConfigManager.cpp` | split | 拆为 INI codec/catalog 与 desktop adapter |
| `MineBackup/src/infra/ConfigManager.h` | split | 拆为 INI codec/catalog 与 desktop adapter |
| `MineBackup/src/infra/DesktopServices.cpp` | desktop | 保留桌面集成 |
| `MineBackup/src/infra/DiagnosticLogExporter.cpp` | desktop | 保留桌面集成 |
| `MineBackup/src/infra/DiagnosticLogExporter.h` | desktop | 保留桌面集成 |
| `MineBackup/src/infra/i18n.cpp` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/i18n.h` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/InterruptedTaskRecovery.cpp` | runtime | 迁移并改用显式 AppPaths |
| `MineBackup/src/infra/InterruptedTaskRecovery.h` | runtime | 迁移并改用显式 AppPaths |
| `MineBackup/src/infra/KnotLinkPackageManager.cpp` | desktop | 保留桌面集成 |
| `MineBackup/src/infra/KnotLinkPackageManager.h` | desktop | 保留桌面集成 |
| `MineBackup/src/infra/KnotLinkProtocol.cpp` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/KnotLinkProtocol.h` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/KnotLinkServerManager.cpp` | split | 监听/握手进 runtime，桌面状态适配留 desktop |
| `MineBackup/src/infra/KnotLinkServerManager.h` | split | 监听/握手进 runtime，桌面状态适配留 desktop |
| `MineBackup/src/infra/KnotLinkService.cpp` | split | 监听/握手进 runtime，桌面状态适配留 desktop |
| `MineBackup/src/infra/KnotLinkService.h` | split | 监听/握手进 runtime，桌面状态适配留 desktop |
| `MineBackup/src/infra/LegacyIniConfigCodec.cpp` | data_core | 安全解析旧 INI 标量并生成结构化诊断 |
| `MineBackup/src/infra/LegacyIniConfigCodec.h` | data_core | 安全解析旧 INI 标量并生成结构化诊断 |
| `MineBackup/src/infra/LegacyLocationDiscovery.cpp` | legacy-desktop | 仅 GUI 启动兼容路径使用 |
| `MineBackup/src/infra/LegacyLocationDiscovery.h` | legacy-desktop | 仅 GUI 启动兼容路径使用 |
| `MineBackup/src/infra/LegacyLocationMigration.cpp` | legacy-desktop | 仅 GUI 启动兼容路径使用 |
| `MineBackup/src/infra/LegacyLocationMigration.h` | legacy-desktop | 仅 GUI 启动兼容路径使用 |
| `MineBackup/src/infra/Logging.cpp` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/Logging.h` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/NetworkService.cpp` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/NetworkService.h` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/ProcessRunner.cpp` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/ProcessRunner.h` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/ReadOnlyMappedFile.cpp` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/ReadOnlyMappedFile.h` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/Sha256.cpp` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/Sha256.h` | data_core | 保留为纯基础设施 |
| `MineBackup/src/infra/SingleInstanceService.cpp` | split | 抽 ProfileLock 到 runtime，IPC 留 desktop |
| `MineBackup/src/infra/SingleInstanceService.h` | split | 抽 ProfileLock 到 runtime，IPC 留 desktop |
| `MineBackup/src/platform/DesktopServices.h` | desktop | 保留桌面服务、托盘或平台 UI bridge |
| `MineBackup/src/platform/LinuxDesktopPortal.cpp` | desktop | 保留桌面服务、托盘或平台 UI bridge |
| `MineBackup/src/platform/LinuxDesktopPortal.h` | desktop | 保留桌面服务、托盘或平台 UI bridge |
| `MineBackup/src/platform/MacDesktopBridge.h` | desktop | 保留桌面服务、托盘或平台 UI bridge |
| `MineBackup/src/platform/MacDesktopBridge.mm` | desktop | 保留桌面服务、托盘或平台 UI bridge |
| `MineBackup/src/platform/NativeDesktopServices.cpp` | desktop | 保留桌面服务、托盘或平台 UI bridge |
| `MineBackup/src/platform/NativeDesktopServices.h` | desktop | 保留桌面服务、托盘或平台 UI bridge |
| `MineBackup/src/platform/NetworkBackend_linux.cpp` | runtime | 保留为非桌面网络后端 |
| `MineBackup/src/platform/NetworkBackend_macos.mm` | runtime | 保留为非桌面网络后端 |
| `MineBackup/src/platform/NetworkBackend_win.cpp` | runtime | 保留为非桌面网络后端 |
| `MineBackup/src/platform/NetworkBackendFactory.h` | runtime | 保留为非桌面网络后端 |
| `MineBackup/src/platform/Platform_linux.cpp` | split | 拆分运行时 OS primitives 与桌面 API |
| `MineBackup/src/platform/Platform_linux.h` | split | 拆分运行时 OS primitives 与桌面 API |
| `MineBackup/src/platform/Platform_macos_tray.mm` | desktop | 保留桌面服务、托盘或平台 UI bridge |
| `MineBackup/src/platform/Platform_macos.cpp` | split | 拆分运行时 OS primitives 与桌面 API |
| `MineBackup/src/platform/Platform_macos.h` | split | 拆分运行时 OS primitives 与桌面 API |
| `MineBackup/src/platform/Platform_win.cpp` | split | 拆分运行时 OS primitives 与桌面 API |
| `MineBackup/src/platform/Platform_win.h` | split | 拆分运行时 OS primitives 与桌面 API |
| `MineBackup/src/platform/PlatformCompat.h` | split | 拆分运行时 OS primitives 与桌面 API |
| `MineBackup/src/ui/CommandConsole.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/CommandConsole.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/HistoryDialogs.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/HistoryDialogs.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/HistoryUI.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/HistoryViewModel.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/HistoryViewModel.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/IconsFontAwesome6.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/imgui-all.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/LogPanel.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/LogPanel.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/MainUI.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/MainUiController.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/MigrationReportUI.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/MigrationReportUI.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/SettingsAutoSave.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/SettingsUI.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/SettingsUI.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/SettingsUIAppearance.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/SettingsUIConfig.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/SettingsUIHotkeys.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/SettingsUIHotkeys.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/SettingsUIPrivate.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/SettingsUISpecial.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/UIHelpers.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/WizardUI.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/WorldListController.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/WorldListController.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/WorldListModel.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/WorldListModel.h` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/ui/WorldListUI.cpp` | desktop | 保留；仅依赖 runtime 公共接口 |
| `MineBackup/src/utils/FileName.cpp` | data_core | 保留为纯工具 |
| `MineBackup/src/utils/FileName.h` | data_core | 保留为纯工具 |
| `MineBackup/src/utils/text_to_text.cpp` | data_core | 保留为纯工具 |
| `MineBackup/src/utils/text_to_text.h` | data_core | 保留为纯工具 |
