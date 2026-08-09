# ADR 0006：以 Headless CLI 牵引 MineBackup Runtime 分层

- 状态：Accepted
- 日期：2026-08-07

## 背景

MineBackup 已有可在 GLFW 初始化前执行特殊配置的路径，但构建目标、启动流程和业务服务仍与桌面依赖及全局状态耦合。并行备份还会并发修改全局历史容器，特殊任务使用未转义的逗号格式和数字索引，无法形成可靠的服务器自动化契约。

## 决策

1. 建立单向依赖：`minebackup_data_core -> minebackup_runtime -> desktop/CLI`。
2. 用无 GLFW、OpenGL、ImGui、X11、Wayland、GTK、GIO、AppIndicator 的 `minebackup-cli` 作为边界验收。
3. GUI 与 CLI 共享同一个 BackupService；runtime 不显示弹框、不打开桌面资源、不读取 `Globals.h`。
4. 历史由线程安全的 HistoryRepository 管理；持久身份使用 ConfigId。
5. 特殊任务迁移到带 schemaVersion 的 JSON，任务使用 UUID，目标使用 ConfigId 与配置内相对路径。
6. CLI 是永久非交互前端，使用稳定结果码、诊断 ID、JSON envelope 和进程退出码。
7. v1.15 adapter 继续由 GUI 承担；CLI 遇到未迁移配置档时明确失败。
8. 旧 GUI `--run-special`、`-specialcfg` 在 CLI 可用后停用；`--autostart` 保留为 GUI 登录启动项的内部标记，不再触发特殊任务。
9. Pack Mode、Restore CLI、配置创建、Agent 和正式 CLI 打包不属于本里程碑。

## 兼容策略

- 保持现有 FolderRewind 归档、metadata 和 history.json 线格式。
- config.ini 暂时继续保存普通配置和桌面设置；只有特殊任务迁移到 special-tasks.json。
- CMake 默认继续构建 GUI；GUI-only 发行流程不在本 ADR 中改变。
- 每个迁移提交都必须保持 GUI 可构建，并为行为变化增加自动化测试。

## 结果

runtime 的边界以可执行构建和依赖审计验证，而不是仅以目录命名判断。任何新增 runtime 源文件若包含桌面头文件，CI 应直接失败。

