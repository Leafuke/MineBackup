# MineBackup 领域术语

## 术语

- **配置档（Profile）**：一组相互隔离、共享持久化根目录和单实例身份的 MineBackup 配置、历史、状态、缓存、运行时文件与工具。
- **配置档根（Profile Root）**：由 `--data-dir` 或便携模式显式选择的完整配置档目录；其下按 config、data、state、cache、runtime、tools 和 logs 分区。
- **AppPaths**：启动参数解析后建立的绝对路径集合，是配置、数据、状态、缓存、临时任务、工具、只读资源和日志位置的唯一运行时来源。
- **便携模式（Portable Mode）**：Windows 或 AppImage 在应用相邻 `portable.flag` 明确存在时使用相邻 `MineBackupData` 配置档根的运行方式；macOS 不支持该标记。
- **资源根（Resources Root）**：安装包或应用包内只读资源的位置；不得作为用户数据、缓存或下载组件的写入目标。
- **Legacy v2**：MineBackup 1.15 的 camelCase `history.json`、`metadata.json` 和相邻 `<archive>.json` record；只允许由 v1.15 reader 解释。
- **FolderRewind v3**：1.16 的 PascalCase `HistoryItem`、`state.json` 和 `records/<archive>.json`。
- **迁移协调器（Migration Coordinator）**：永久存在的迁移流程边界，负责显式路径、迁移报告、恢复快照索引和受影响操作的写门禁；不解释旧格式，也不绘制 UI。
- **v1.15 迁移适配器（V15 Migration Adapter）**：仅在 1.16 启用的兼容组件，把协调器的迁移单元映射到 MineBackup 1.15 数据转换；可在 1.17 连同 v1.15 reader 删除。
- **v1.15 reader**：只读 MineBackup 1.15 格式且不接触运行时 UI 或目标持久化的解析组件。
- **迁移单元**：可独立检测、提交和重试的一份数据；当前包括启动配置、启动历史、单个世界和单个云配置。
- **迁移报告**：按迁移单元记录状态、诊断、迁移/跳过数量和恢复快照位置的持久化模型；设置 UI 只消费该模型。
- **写门禁**：某个迁移单元未安全完成时，对依赖该单元的破坏性写操作施加的限制；不影响无关配置档或迁移单元。
- **恢复快照**：覆盖规范路径前保留的旧 JSON。它用于人工恢复，不代表运行期双写。
- **降级迁移**：部分 record 可保存，但不足以安全延续 Smart 链；下一次备份必须建立新 Full 链。
- **配置档身份（Profile Identity）**：由规范化配置档根派生的稳定身份；单实例锁和 IPC 端点按该身份隔离，而不是按可执行文件路径隔离。
- **单实例请求（Instance Request）**：第二个进程交给同配置档主实例的有界、版本化 IPC 消息；当前包括激活窗口、按 ConfigId 选择普通配置和按 SpecialConfigId 运行特殊配置。
- **SpecialConfigId**：特殊配置的稳定 UUID；进程间请求和 autostart 不使用可被重排的配置序号定位特殊配置。
- **启动位置迁移（Startup Location Migration）**：在目标配置档尚未初始化时，对旧位置进行规范化发现、用户选择和事务复制的启动阶段；源文件保留不动，完成后才进入格式迁移。
- **ProcessSpec**：内部外部进程调用的无 shell 契约；可执行文件、参数向量、工作目录、超时、输出上限和优先级分别表达。
- **ShellTaskSpec**：唯一允许保存并执行原始 shell 命令字符串的用户任务契约；Windows 固定使用 `cmd.exe`，Linux/macOS 固定使用 `/bin/sh`，且不承诺跨平台可移植。
- **任务协调器（Task Coordinator）**：应用内后台工作的统一生命周期边界；负责接收新任务、持有 `jthread`、传播停止令牌、隔离异常、串行化资源冲突并在退出时拒绝新工作后完成收尾。
- **世界资源键（World Resource Key）**：由 ConfigId 与规范化绝对世界路径组成的互斥身份；同一世界的备份、恢复、删除、验证和迁移提交不得并行修改数据。
- **配置档云队列（Profile Cloud Queue）**：按 Profile Identity 建立的单队列资源；同一配置档的 rclone 上传、下载、历史同步与分析按提交顺序互斥执行。
- **任务结果事件（Task Result Event）**：工作线程投递给主线程的不可变结果；当前用于协调器异常与自动任务完成通知，并作为后续运行时状态全面收口到主线程的传递边界。
