# MineBackup 数据格式上下文

## 术语

- **Legacy v2**：MineBackup 1.15 的 camelCase `history.json`、`metadata.json` 和相邻 `<archive>.json` record。
- **FolderRewind v3**：1.16 的 PascalCase `HistoryItem`、`state.json` 和 `records/<archive>.json`。
- **迁移单元**：可独立检测、提交和重试的一份数据；当前包括启动配置、启动历史、单个世界和单个云配置。
- **恢复快照**：覆盖规范路径前保留的旧 JSON。它用于人工恢复，不代表运行期双写。
- **降级迁移**：部分 record 可保存，但不足以安全延续 Smart 链；下一次备份必须建立新 Full 链。

## 不变量

- 归档包在迁移中不移动、不重命名、不重新压缩。
- FolderRewind 核心流程不直接读取 Legacy v2；旧格式读取集中在兼容层。
- `state.json` 最后提交，是一个世界 metadata 迁移完成的标志。
- 迁移失败仅阻断受影响的危险操作，不阻止应用启动或其他配置工作。
