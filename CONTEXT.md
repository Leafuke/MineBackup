# MineBackup 领域术语

## 术语

- **配置档（Profile）**：一组相互隔离、共享持久化根目录和单实例身份的 MineBackup 配置、历史、状态、缓存、运行时文件与工具。
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
