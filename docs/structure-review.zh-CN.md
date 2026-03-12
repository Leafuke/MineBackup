# MineBackup 代码结构审计

## 本次目录重排

当前自研源码已经从项目根目录迁移到以下结构：

```text
MineBackup/
├─ src/
│  ├─ app/        # 应用入口、全局运行态、数据模型
│  ├─ core/       # 备份、历史、任务、特殊模式等核心业务
│  ├─ infra/      # 配置、日志、广播、国际化等基础设施
│  ├─ platform/   # Windows/Linux/macOS 平台适配
│  ├─ ui/         # ImGui 窗口与 UI 辅助代码
│  └─ utils/      # 通用文本/路径/零散工具
├─ third_party/
│  ├─ imgui/
│  ├─ KnotLink/
│  ├─ json/
│  └─ stb/
```

### 模块归类

- app
  - AppState.cpp/.h
  - DataModels.h
  - Globals.h
  - MainUI.h
  - MineBackup.cpp
- core
  - BackupManager.cpp/.h
  - GameSessionManager.cpp
  - HistoryManager.cpp/.h
  - SpecialMode.cpp
  - TaskSystem.cpp/.h
- infra
  - Broadcast.cpp/.h
  - ConfigManager.cpp/.h
  - Console.cpp/.h
  - i18n.cpp/.h
- platform
  - PlatformCompat.h
  - Platform_win.cpp/.h
  - Platform_linux.cpp/.h
  - Platform_macos.cpp/.h
  - Platform_macos_tray.mm
- ui
  - HistoryUI.cpp
  - SettingsUI.cpp
  - WizardUI.cpp
  - UIHelpers.h
  - imgui-all.h
  - IconsFontAwesome6.h
- utils
  - basic_func.cpp
  - text_to_text.cpp/.h

## 最适合优先拆分的文件

### 1. src/app/MineBackup.cpp

当前同时承担了：

- main 启动入口
- 全局变量定义
- 字体与资源初始化
- 主窗口循环
- 若干业务弹窗和导入导出逻辑

建议拆为：

- Main.cpp：只保留程序入口
- AppBootstrap.cpp：初始化、窗口和字体加载
- AppRuntime.cpp：主循环与生命周期
- UpdateState.cpp 或 NoticeState.cpp：版本与通知检查状态

### 2. src/app/Globals.h

这是典型的“全局状态桶”。现在把窗口尺寸、更新状态、设置项、热键、历史窗口状态和线程控制都暴露成 extern。建议按职责拆成：

- AppUiState
- UpdateCheckState
- WindowState
- RestoreState

更进一步，应尽量并回 AppState，而不是继续扩大全局变量表。

### 3. src/core/BackupManager.cpp

当前混合了：

- 备份执行
- 还原执行
- 元数据读写
- 智能备份链规划
- 文件变化分析
- 并发保护

建议至少拆为：

- BackupMetadataStore
- BackupPlanner
- RestorePlanner
- RestoreExecutor
- ChangeDetector

### 4. src/ui/SettingsUI.cpp

文件体积已经超过 1100 行，而且一个文件里同时处理配置管理、普通配置、特殊模式、路径选择、主题、热键、程序设置。适合按标签页或面板拆分：

- SettingsConfigPanel
- SettingsTaskPanel
- SettingsAppearancePanel
- SettingsAdvancedPanel

### 5. src/infra/ConfigManager.cpp

当前同时包含默认值计算、配置解析、配置保存、字体路径兜底、配置项迁移。建议拆为：

- ConfigDefaults
- ConfigSerializer
- ConfigValidator
- ConfigMigration

### 6. src/infra/Console.h 和 src/infra/Console.cpp

Console 结构体同时承担了：命令注册、日志缓存、绘制、输入处理、历史记录、导出。建议拆为：

- ConsoleModel
- ConsoleView
- ConsoleCommandDispatcher

### 7. src/infra/i18n.cpp

当前是超大内嵌字典文件。维护成本高，diff 噪声大，也不利于翻译校对。建议改成：

- zh_CN.json / en_US.json 或 ini 资源文件
- 或最少拆成 zh/en 两个翻译表

### 8. src/ui/HistoryUI.cpp

当前文件虽然没有前几项那么大，但窗口结构已经比较明确，适合提前拆成：

- HistoryListPane
- HistoryDetailsPane
- HistoryRestoreDialog

## 其他结构问题

### 1. 构建文件存在双重维护和漂移风险

CMakeLists.txt 和 MineBackup.vcxproj 同时各自维护一套源文件路径。此次整理过程中已经发现 CMake 之前漏掉了 TaskSystem.cpp。后续如果继续人工维护两份清单，这类问题还会重复出现。

建议：

- 用按模块分组的 target_sources 统一组织 CMake
- 将 VS 工程更多作为 IDE 工程视图，而不是单独维护一份“真实文件清单”

### 2. 平台层向上渗透过多

目前不少非平台模块直接包含 Platform_win.h、Platform_linux.h、Platform_macos.h。这样会让 UI、配置和工具层都带着平台条件编译扩散。

建议：

- 只暴露 PlatformCompat.h 中的稳定接口
- 把平台差异下沉到 platform 目录内实现文件

### 3. 聚合头文件加剧编译耦合

imgui-all.h 把 ImGui、OpenGL 后端、stb、json、图标头一次性拉进来，编译依赖面过大。

建议：

- 在 UI 代码中只按需包含所需头文件
- 将资源加载与样式定义从聚合头中拆出

### 4. 资源脚本包含绝对路径

MineBackup.rc 里图标和字体资源仍然使用 D 盘绝对路径。这会直接影响其他机器上的 Windows 构建可复现性。

建议：

- 改成相对于仓库根目录或项目目录的相对路径
- 将图标和字体统一收敛到 Assets 或 resources 子目录

### 5. 构建产物和运行态文件仍在源码树中混放

当前项目里同时存在：

- x64/
- Release/
- MineBackup.aps
- MineBackup.vcxproj.user
- imgui.ini
- history.dat
- config.ini

这些文件和源码放在同一层，会持续制造噪声。

建议：

- 强制使用 out-of-source CMake 构建目录
- 将运行时配置和历史文件迁到程序数据目录或可配置数据目录
- 继续完善 .gitignore

### 6. 第三方代码与业务代码目前仍挂在同一个最终可执行目标下

目录已经分开，但构建目标还是一个大目标。更长期的可维护方案是：

- minebackup_core
- minebackup_ui
- minebackup_platform
- third_party_imgui

把业务层和第三方层拆成独立 target，链接关系会更清晰。

## 下一阶段建议顺序

1. 先处理 Globals.h 和 MineBackup.cpp 的全局状态收敛。
2. 再拆 BackupManager.cpp 和 SettingsUI.cpp，这两个文件的维护收益最高。
3. 随后处理 MineBackup.rc 的绝对路径与运行时数据目录。
4. 最后把平台接口收敛到 PlatformCompat.h，并考虑拆分多个静态库 target。