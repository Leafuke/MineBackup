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
- **受限网络服务（Network Service）**：只提供 HTTPS GET 文本和带 SHA-256 的流式文件下载，不暴露任意方法、请求体或通用 HTTP 客户端；统一施加超时、取消、大小和重定向限制。
- **网络状态（Network Status）**：可区分取消、超时、超限、截断、重定向越界、HTTPS 降级、HTTP、TLS、I/O 和哈希不匹配的稳定结果枚举。
- **文本镜像回退（Text Mirror Fallback）**：直连官方文本失败时使用的硬编码镜像请求；响应只作为公告文本、版本和发布说明解析，不能提供应用要打开的 URL、命令或可执行内容。
- **便携配置文档（Portable Config Document）**：云端 `portable-config.json` 的版本化白名单模型，以 ConfigId 为唯一键，仅包含名称、逻辑世界定义、备份/压缩/保留/黑名单和可迁移云策略；不包含任何本机路径、工具、凭据、命令、脚本、自动化或运行结果。
- **方向性合并（Directional Merge）**：上传时本地白名单字段覆盖同 ConfigId 的远端字段并保留远端独有配置；导入时远端白名单字段覆盖同 ConfigId 的本地字段并保留本地独有配置。两个方向都必须先生成预览并再次确认。
- **待绑定配置（Pending Local Binding）**：从远端新增到本机、尚未绑定存档根、备份根和压缩工具的配置；绑定完成前禁止备份、恢复、删除、云写入和自动任务，且不继承远端的启用状态或本机路径。
- **能力状态（Capability Status）**：桌面服务返回的 Available、Unavailable、PermissionRequired 或 Failed 及其诊断；运行期降级不得悄悄覆盖用户保存的偏好。
- **旧服务清理（Legacy Service Cleanup）**：1.16 对旧 Windows Service Mode 的唯一支持边界；只能读取并验证旧服务，在用户确认和 UAC 后删除，不能安装或启动服务。
- **发布候选（Release Candidate Artifact）**：平台工作流为同一提交生成、尚未对公众发布的固定名称资产；平台工作流无权查找或修改 GitHub Release。
- **发布清单（Release Manifest）**：汇合任务生成的版本化 JSON，记录版本、提交、各资产大小/SHA-256、平台门禁和经审批的平台缺失豁免；所有正式资产必须可追溯到同一提交。
- **原子发布门禁（Atomic Release Gate）**：所有非豁免平台成功后，唯一发布任务把精确标签的草稿 Release 连同全部候选上传完整，再转为公开；失败时草稿保持未发布，绝不上传到“最新 Release”。
