# MineBackup 与 FolderRewind 备份/云存档格式统一设计

日期：2026-06-07
状态：2026-07-10 兼容性修订；原“直接替换”策略已撤销

> 2026-07-10 修订优先于本文后续仍保留的历史分析。MineBackup 1.16 将对 1.15 数据执行一次性、单向、事务化迁移，而不是直接舍弃旧 history/metadata。详细取舍见 `docs/adr/0001-staged-v15-to-v16-migration.md`。

## 背景

当前仓库是第一代存档时光机 MineBackup（C++20 + ImGui）。`References/FolderRewind` 中包含第二代存档时光机 FolderRewind（C# + WinUI 3）。MineBackup 仍在维护，因此需要让 MineBackup 后续生成的本地备份、元数据、历史记录和云存档尽可能与 FolderRewind 通用。

本设计以 FolderRewind 现有格式为目标格式。MineBackup 不再定义另一套新格式，也不把旧 MineBackup 格式作为长期兼容目标。

## 已确认决策

1. **兼容策略：分阶段迁移。** MineBackup 新生成的数据全部采用 FolderRewind 格式；1.16 启动时迁移 1.15 配置/历史，并在世界或云端首次使用时惰性迁移 metadata。旧格式只读、不双写，兼容层在 1.17 移除。
2. **配置身份：新增 `ConfigId`。** MineBackup 每个配置持久化一个 GUID，对齐 FolderRewind `BackupConfig.Id` / `HistoryItem.ConfigId`。
3. **存储目录名：使用世界文件夹名。** 本地备份子目录、元数据目录、云端 `FolderName` 都使用 Minecraft 世界文件夹名。
4. **归档文件名主体：使用世界文件夹名。** MineBackup 的 `desc` 不再影响归档文件名主体，也不参与路径身份。
5. **改造边界：替换格式边界，不重写整个备份系统。** 保留 MineBackup 现有备份、还原、删除、KnotLink、WorldEdit、UI 流程，集中替换路径、元数据、历史、云端清单的读写格式。

## 现状摘要

### MineBackup 当前格式

- 本地备份目录当前为 `config.backupPath / worldName`，元数据目录为 `config.backupPath / _metadata / worldName`。
- 归档文件名当前优先使用 `folder.desc`，没有 `desc` 才使用世界名；Full/Smart 文件名前缀为 `[Full][timestamp]...` 和 `[Smart][timestamp]...`。
- 元数据当前以 `_metadata/<world>/metadata.json` 保存最新状态和记录索引，并在同一目录写 `<archive>.json` 记录文件。
- 本地 `history.json` 当前使用 MineBackup 自有字段：`configIndex`、`worldPath`、`worldName`、`backupFile`、`backupType`、云端路径字段等。
- 云端路径已接近 FolderRewind：归档在 `<remote>/<configName>/<world>/<archive>`，元数据目标路径已经类似 `_metadata/state.json` 与 `_metadata/records/<archive>.json`，但实际本地 state 仍来自旧 `metadata.json`，JSON 字段也不是 FolderRewind 的 PascalCase schema。

### FolderRewind 目标格式

- 本地路径：`<DestinationPath>/<StorageFolderName>` 与 `<DestinationPath>/_metadata/<StorageFolderName>`。
- 归档文件名：`[Full|Smart|Overwrite][yyyy-MM-dd_HH-mm-ss]<FolderName> [Comment].7z`。
- 元数据：`_metadata/<FolderName>/state.json` 与 `_metadata/<FolderName>/records/<ArchiveFileName>.json`。
- JSON 字段使用 System.Text.Json 默认属性名，即 PascalCase。
- 本地/云端 history 使用 `HistoryItem` schema：`ConfigId`、`FolderPath`、`FolderName`、`FileName`、`Timestamp`、`BackupType`、`Comment`、`IsImportant`、`IsCloudArchived`、云端路径字段等。
- 云端路径：`<RemoteBase>/history.json`、`<RemoteBase>/<ConfigName>/_folderrewind/active-history.json`、`<RemoteBase>/<ConfigName>/<FolderName>/<ArchiveFileName>`、以及同目录下 `_metadata/state.json` 和 `_metadata/records/<ArchiveFileName>.json`。

## 目标

1. MineBackup 新生成的本地备份目录、归档文件名、元数据和历史记录与 FolderRewind schema 对齐。
2. MineBackup 新上传的云端归档、元数据、全局历史和 active-history manifest 可被 FolderRewind 识别。
3. MineBackup 能读取和还原 FolderRewind 风格 history、metadata、archive。
4. 保持 MineBackup 现有功能流程稳定，避免把本次工作扩大为架构重写。
5. 对旧 MineBackup 格式采用明确边界：不长期维护双格式，但在危险操作中给出保护性失败或提示。

## 非目标

1. 不为 1.15 以前版本承诺完整 Smart 链迁移；1.15 metadata v2 属于 1.16 的正式迁移范围。
2. 不保证旧 MineBackup Smart 链在没有 FolderRewind `records/` 的情况下可精确 Clean Restore。
3. 不重写 MineBackup UI、备份调度、KnotLink、WorldEdit 集成或安全删除整体架构。
4. 不引入强制文件哈希扫描；`Hash` 字段先保持为空字符串以对齐 FolderRewind schema。

## 架构设计

### 0. 1.15 迁移边界

- `LegacyMineBackup15Reader` 是唯一允许读取 camelCase history、`metadata.json` 和相邻 record 的组件。
- `MigrationService` 在启动时迁移配置/历史，在备份、还原、删除或云操作前迁移对应世界。
- 迁移不重命名或重传归档；新 state/records 继续引用原文件名。
- 新格式有效时优先，旧数据只补缺；损坏或无法唯一映射时保留快照并局部阻断危险操作。
- 配置身份对旧云配置由规范化的 `RemoteBase + ConfigName` 确定性派生，新建配置仍使用随机 GUID。
- `MINEBACKUP_ENABLE_V15_MIGRATION=0` 必须仍可构建，为 1.17 删除兼容层提供验证。

### 1. FolderRewindIdentity

在 MineBackup `Config` 中新增持久化字段：

```cpp
std::wstring configId;
```

语义：

- 对齐 FolderRewind `BackupConfig.Id`。
- 本地 history 和云端 history 使用 `ConfigId` 作为配置身份。
- MineBackup 内部 `configIndex` 继续作为运行时索引，但不再作为 FolderRewind 格式历史的主身份字段。

读写规则：

- `ConfigManager` 读取配置时，如果 `ConfigId` 缺失或为空，则生成 GUID。
- 保存配置时写出 `ConfigId`。
- `Config.name` 继续作为云端路径中的 `ConfigName`。

### 2. FolderRewindStoragePaths

新增或抽取统一路径 helper，负责本地备份和元数据路径解析。

输入：

- `backupRoot`：MineBackup `config.backupPath`。
- `folderName`：Minecraft 世界文件夹名。
- `fallbackPath`：世界完整路径，用于必要时从路径推导名称。

输出：

```text
storageFolderName = <worldFolderName>
backupSubDir      = <backupRoot>/<storageFolderName>
metadataDir       = <backupRoot>/_metadata/<storageFolderName>
recordsDir        = <metadataDir>/records
statePath         = <metadataDir>/state.json
```

安全规则：

- `storageFolderName` 必须是单段文件名。
- 不允许空字符串、`.`、`..`。
- 不允许路径分隔符 `/`、`\`。
- 替换 Windows 非法文件名字符。
- `backupSubDir` 必须在 `backupRoot` 内。
- `metadataDir` 必须在 `backupRoot/_metadata` 内。

### 3. FolderRewindMetadataStore

把 MineBackup 当前嵌在 `BackupManager.cpp` 中的元数据读写替换为 FolderRewind 风格 store。

主要职责：

- 读取 `state.json`。
- 读取 `records/*.json`，按 archive 文件名查 record。
- 写 `state.json`。
- 写 `records/<ArchiveFileName>.json`。
- 安全删除后同步或修复 record。
- 校验 Smart restore chain。

C++ 内部结构可以继续使用 MineBackup 当前类似结构，但序列化字段必须是 FolderRewind PascalCase。

### 4. FolderRewindHistoryStore

MineBackup 可以保留现有 `g_appState.g_history` 缓存，但本地 `history.json` 的读写层改成 FolderRewind `HistoryItem` schema。

目标字段：

```text
ConfigId
FolderPath
FolderName
FileName
Timestamp
BackupType
IsPartialBackup
Comment
IsImportant
IsCloudArchived
CloudArchivedAtUtc
CloudArchiveRemotePath
CloudMetadataRecordRemotePath
CloudMetadataStateRemotePath
```

字段映射：

```text
Config.configId          -> ConfigId
world full path          -> FolderPath
world folder name        -> FolderName
archive file name        -> FileName
backup local time        -> Timestamp
Full/Smart/Overwrite     -> BackupType
MineBackup comment       -> Comment
isImportant              -> IsImportant
cloud remote path fields -> Cloud*RemotePath
```

### 5. CloudSync boundary

CloudSyncService 继续负责 rclone 命令组织、重试、状态记录和 UI 日志，但它构造和上传的路径/schema 改为 FolderRewind 格式。

远端布局：

```text
<RemoteBase>/
  history.json

  <ConfigName>/
    _folderrewind/
      active-history.json

    <FolderName>/
      [Full][...].7z
      [Smart][...].7z

      _metadata/
        state.json
        records/
          [Full][...].7z.json
          [Smart][...].7z.json
```

常量变化：

- MineBackup 当前 `_minebackup` 应替换为 FolderRewind 的 `_folderrewind`。
- active manifest 文件名保持 `active-history.json`。
- 全局 history 路径为 `<RemoteBase>/history.json`。

## 数据格式

### 1. 归档文件名

格式：

```text
[<BackupType>][yyyy-MM-dd_HH-mm-ss]<WorldFolderName>[ <CommentPart>].<format>
```

示例：

```text
[Full][2026-06-07_12-30-00]SurvivalWorld.7z
[Smart][2026-06-07_12-35-00]SurvivalWorld [BeforeBossFight].7z
[Overwrite][2026-06-07_12-40-00]SurvivalWorld.7z
```

规则：

- `BackupType` 为 `Full`、`Smart`、`Overwrite`。
- 文件名主体始终为世界文件夹名。
- comment 可追加为 `[comment]`，但需要过滤非法文件名字符和 `[`、`]`。
- comment 同时写入 `HistoryItem.Comment`。

### 2. state.json

路径：

```text
_metadata/<WorldFolderName>/state.json
```

示例：

```json
{
  "Version": "3.0",
  "LastBackupTime": "2026-06-07T12:35:00",
  "LastBackupFileName": "[Smart][2026-06-07_12-35-00]SurvivalWorld.7z",
  "BasedOnFullBackup": "[Full][2026-06-07_12-30-00]SurvivalWorld.7z",
  "FileStates": {
    "level.dat": {
      "Size": 12345,
      "LastWriteTimeUtc": "2026-06-07T04:34:59Z",
      "Hash": ""
    },
    "region/r.0.0.mca": {
      "Size": 987654,
      "LastWriteTimeUtc": "2026-06-07T04:34:50Z",
      "Hash": ""
    }
  }
}
```

规则：

- `Version` 固定写 `"3.0"`。
- `LastBackupTime` 表示本次备份完成时间。
- `FileStates` 的 key 使用正斜杠 `/` 作为相对路径分隔符。
- `LastWriteTimeUtc` 使用 UTC 时间字符串，替代 MineBackup 当前 `lastWriteTimeTicks`。
- `Hash` 先写空字符串。

### 3. records

路径：

```text
_metadata/<WorldFolderName>/records/<ArchiveFileName>.json
```

示例：

```json
{
  "ArchiveFileName": "[Smart][2026-06-07_12-35-00]SurvivalWorld.7z",
  "BackupType": "Smart",
  "BasedOnFullBackup": "[Full][2026-06-07_12-30-00]SurvivalWorld.7z",
  "PreviousBackupFileName": "[Full][2026-06-07_12-30-00]SurvivalWorld.7z",
  "CreatedAtUtc": "2026-06-07T04:35:00Z",
  "AddedFiles": ["data/new_file.dat"],
  "ModifiedFiles": ["level.dat"],
  "DeletedFiles": ["old/file.dat"],
  "FullFileList": ["level.dat", "region/r.0.0.mca", "data/new_file.dat"]
}
```

Full 记录规则：

- `BackupType = "Full"`。
- `BasedOnFullBackup = ArchiveFileName`。
- `PreviousBackupFileName = ""`。
- `AddedFiles = FullFileList`。
- `ModifiedFiles = []`。
- `DeletedFiles = []`。

Smart 记录规则：

- `BackupType = "Smart"`。
- `BasedOnFullBackup` 指向链起点 Full。
- `PreviousBackupFileName` 指向上一个备份文件。
- `AddedFiles`、`ModifiedFiles`、`DeletedFiles` 是相对上一次 state 的变化。
- `FullFileList` 是应用本次变化后的完整文件集合。

Overwrite 记录规则：

- `BackupType = "Overwrite"`。
- 按 Full-like 处理。
- `BasedOnFullBackup = ArchiveFileName`。
- `PreviousBackupFileName = ""`。

### 4. history.json

MineBackup 本地 `history.json` 改为 FolderRewind `List<HistoryItem>` 风格数组。

示例：

```json
[
  {
    "ConfigId": "6f9619ff-8b86-d011-b42d-00cf4fc964ff",
    "FolderPath": "D:\\Minecraft\\.minecraft\\saves\\SurvivalWorld",
    "FolderName": "SurvivalWorld",
    "FileName": "[Full][2026-06-07_12-30-00]SurvivalWorld.7z",
    "Timestamp": "2026-06-07T12:30:00",
    "BackupType": "Full",
    "IsPartialBackup": false,
    "Comment": "",
    "IsImportant": false,
    "IsCloudArchived": true,
    "CloudArchivedAtUtc": "2026-06-07T04:31:00Z",
    "CloudArchiveRemotePath": "remote:FolderRewind/Default/SurvivalWorld/[Full][2026-06-07_12-30-00]SurvivalWorld.7z",
    "CloudMetadataRecordRemotePath": "remote:FolderRewind/Default/SurvivalWorld/_metadata/records/[Full][2026-06-07_12-30-00]SurvivalWorld.7z.json",
    "CloudMetadataStateRemotePath": "remote:FolderRewind/Default/SurvivalWorld/_metadata/state.json"
  }
]
```

## 核心流程

### 1. 备份流程

MineBackup 当前 `DoBackup` 的主业务流程保留，但替换路径、文件名、元数据和历史写入。

```text
DoBackup(folder)
  -> ResolveFolderRewindStoragePaths(config, folder)
  -> Load state.json + records/
  -> Scan current FileStates
  -> Compare current FileStates vs previous state
  -> Decide Full / Smart / Overwrite
  -> Generate FolderRewind archive file name
  -> Run 7z
  -> Save state.json
  -> Save records/<archive>.json
  -> Add FolderRewind-style HistoryItem
  -> Queue cloud upload
```

Smart 模式规则：

- `state.json` 缺失、损坏、引用的 `LastBackupFileName` 不存在、或引用的 `BasedOnFullBackup` 不存在时，强制 Full。
- 无变化且 `skipIfUnchanged` 为 true 时跳过。
- 只有删除变化时保留删除标记归档思想，但内部标记目录改为 `__FolderRewind_Internal/__DeletedOnly.marker`。

### 2. 还原流程

```text
DoRestore(target)
  -> 如果云端补链启用：EnsureRestoreChainAvailable
  -> 读取 metadataDir/state.json + records/
  -> 判断目标是否 Smart
     -> Full/Overwrite：直接单包还原
     -> Smart：用 records 追溯 PreviousBackupFileName 到 Full
  -> 校验归档完整性
  -> Clean Restore 时构建 SmartRestorePlan
  -> ApplySmartRestorePlan 或按链顺序解压
  -> 清理内部删除标记目录
```

Smart Clean Restore 必须有完整 records。缺少目标 record、中间 record、链首 Full-like record、或 record 内容不一致时，拒绝执行 Clean Restore。

### 3. 删除与安全删除流程

删除归档时同步更新：

```text
Delete archive
  -> 删除 records/<deleted>.json
  -> 如有 successor：合并归档内容，必要时 Smart -> Full 重命名，重写 successor record
  -> 更新后续 records 的 PreviousBackupFileName / BasedOnFullBackup
  -> 更新或删除 state.json
  -> 更新 history.json / active-history.json
```

保留 MineBackup 现有删除模式：仅历史、仅本地归档、本地归档 + 历史。所有会影响当前配置有效历史的删除，都应在云同步启用时重新上传全局 `history.json` 和配置级 `active-history.json`。

### 4. 云上传流程

单条历史上传顺序：

```text
UploadHistoryEntry
  -> archive:       local backupPath/<FolderName>/<FileName>
                    remote <RemoteBase>/<ConfigName>/<FolderName>/<FileName>
  -> metadata state local backupPath/_metadata/<FolderName>/state.json
                    remote <RemoteBase>/<ConfigName>/<FolderName>/_metadata/state.json
  -> metadata record local backupPath/_metadata/<FolderName>/records/<FileName>.json
                     remote <RemoteBase>/<ConfigName>/<FolderName>/_metadata/records/<FileName>.json
  -> Update history item cloud fields
  -> optionally UploadConfigurationHistorySnapshot
```

配置历史快照上传两个文件：

1. `<RemoteBase>/history.json`：FolderRewind `List<HistoryItem>`。
2. `<RemoteBase>/<ConfigName>/_folderrewind/active-history.json`：只包含当前配置仍有效的历史条目。

### 5. 云下载 / 补链流程

```text
EnsureRestoreChainAvailable
  -> 下载 <RemoteBase>/history.json
  -> 下载 active-history.json，如存在则过滤无效历史
  -> 按 ConfigId 匹配当前配置
  -> 按 FolderPath 精确匹配世界
  -> 如果路径不同，按唯一 FolderName 匹配
  -> 计算目标 restore chain
  -> 下载缺失 archive
  -> 下载 state.json
  -> 下载对应 records/<archive>.json
```

### 6. DoOthersBackup

`DoOthersBackup` 同样纳入 FolderRewind schema：

- `backupName = backupWhat.filename()` 作为 `FolderName`。
- 本地目录：`backupPath/<backupName>`。
- 元数据目录：`backupPath/_metadata/<backupName>`。
- 归档文件名：`[Full][timestamp]<backupName> [comment].7z`。
- history 写 `FolderPath = othersPath`，`FolderName = backupName`。

## 错误处理与兼容边界

### 1. 旧格式边界

- 新版本 MineBackup 只承诺生成 FolderRewind 格式。
- 旧 `_metadata/<world>/metadata.json` 不再作为主元数据。
- 旧 MineBackup `history.json` 字段格式不再作为主历史格式。
- 旧备份包本体仍可能作为普通 7z 包手动还原。
- 旧 Smart 链如果缺少 FolderRewind `records/`，不保证精确 Clean Restore。

保护策略：

- 启动或备份前发现旧 `metadata.json` 而没有新 `state.json` 时，日志提示当前版本使用 FolderRewind 元数据格式，旧 Smart 链可能需要重新执行 Full 备份。
- 目标 Smart 备份没有新 records 时，Clean Restore 拒绝执行。

### 2. 元数据错误处理

备份时：

- `state.json` 不存在、JSON 解析失败、`FileStates` 不是对象、或引用的 `LastBackupFileName` 不存在时，Smart 模式强制 Full。

还原时：

- Full/Overwrite 不依赖 state，可以单包还原。
- Smart Clean Restore 必须有完整 records。
- records 缺失、循环引用、链首不是 Full-like、或 `FullFileList` 不一致时拒绝 Clean Restore。

### 3. 云同步错误处理

- archive 上传失败：整条上传失败，不标记 `IsCloudArchived`。
- archive 上传成功但 state/record 上传失败：保留云端归档，history 只填成功上传的远端路径，并提示元数据部分同步失败。
- `history.json` 上传失败：不回滚 archive，本地历史保留云端路径，下次同步可补传。
- `active-history.json` 上传失败：不视为归档上传失败，但记录 warning。
- 下载 archive 失败：该条失败。
- 下载 state/record 失败：归档可保留，但 Smart Clean Restore 可能不可用。

### 4. 时间字段规则

- 文件名时间：本地时间，格式 `yyyy-MM-dd_HH-mm-ss`。
- `HistoryItem.Timestamp`：本地时间语义，ISO 风格字符串。
- `CloudArchivedAtUtc`：UTC，带 `Z`。
- `CreatedAtUtc`：UTC，带 `Z`。
- `UpdatedAtUtc`：UTC，带 `Z`。
- `FileState.LastWriteTimeUtc`：UTC，带 `Z`。

## 测试策略

### A. 路径与命名测试

验证：

- 世界名 `SurvivalWorld` 生成 `backupRoot/SurvivalWorld` 和 `backupRoot/_metadata/SurvivalWorld`。
- comment 中非法字符、`[`、`]` 被过滤。
- `desc` 不影响目录名和归档主体名。
- 路径穿越输入被拒绝或安全化。

### B. 元数据 JSON 快照测试

Full + Smart 备份后检查：

- 写出 `state.json` 而不是旧 `metadata.json`。
- 写出 `records/<archive>.json`。
- JSON 字段是 PascalCase。
- `Version = "3.0"`。
- `FileStates` 使用 `/` 分隔。
- Full record 的 `AddedFiles == FullFileList`。
- Smart record 的 `PreviousBackupFileName` 指向前一个备份。

### C. history.json 格式测试

检查：

- 不再写 `configIndex` 作为主字段。
- 写 `ConfigId`、`FolderPath`、`FolderName`、`FileName`、`Timestamp`。
- 云端字段名与 FolderRewind `HistoryItem` 一致。
- `ConfigId` 在保存/重新加载配置后保持不变。

### D. Smart Restore 链测试

构造：

1. Full：包含 A、B。
2. Smart1：修改 A，新增 C。
3. Smart2：删除 B，修改 C。

验证 Clean Restore 到 Smart2 后：

- A 是 Smart1 的版本。
- B 不存在。
- C 是 Smart2 的版本。
- 没有内部删除标记目录残留。

### E. 安全删除测试

构造 Full + Smart1 + Smart2，删除 Smart1：

- Smart2 record 被重写。
- `PreviousBackupFileName` 正确跳过 Smart1。
- 如果删除 Full 并把下一个 Smart 合并为 Full，文件名和 record `BackupType` 都更新。
- history 中对应项同步更新或删除。

### F. 云路径构造测试

对配置：

```text
ConfigName = Default
ConfigId = <guid>
FolderName = SurvivalWorld
RemoteBase = remote:FolderRewind
```

验证远端路径：

```text
remote:FolderRewind/history.json
remote:FolderRewind/Default/_folderrewind/active-history.json
remote:FolderRewind/Default/SurvivalWorld/[Full][...].7z
remote:FolderRewind/Default/SurvivalWorld/_metadata/state.json
remote:FolderRewind/Default/SurvivalWorld/_metadata/records/[Full][...].7z.json
```

### G. FolderRewind 互通验收

最终手动/集成验收：

1. 用 MineBackup 创建 Full + Smart 备份。
2. 上传到测试 rclone remote 或本地 fake remote。
3. 用 FolderRewind 指向同一 remote base。
4. FolderRewind 能分析到对应历史。
5. FolderRewind 能下载 archive + metadata。
6. FolderRewind 能还原目标 Smart 备份。
7. 反向验证：FolderRewind 生成的备份，MineBackup 能导入 history 并还原。

## 实施顺序建议

1. 新增 `ConfigId` 字段和配置读写。
2. 抽取 FolderRewind 路径和文件名 helper。
3. 实现 FolderRewind metadata store。
4. 改造备份写入 state/records。
5. 改造 history 本地读写为 FolderRewind schema。
6. 改造还原链读取 records。
7. 改造安全删除 metadata 同步。
8. 改造云同步路径、history、active-history。
9. 增加测试和手动互通验收。

## 风险与缓解

- **旧 Smart 链不可精确还原。** 通过日志提示和 Clean Restore 拒绝策略保护数据。
- **FolderRewind 与 MineBackup 时间格式差异。** 统一用 ISO 风格字符串和 UTC `Z` 后缀。
- **世界名或配置名包含特殊字符。** 本地路径使用安全化规则；远端路径统一 `/` 并 trim 空段。
- **云端 history 合并覆盖错误。** 优先用 `ConfigId` 匹配本配置，远端路径前缀仅作为旧数据兜底。
- **实现范围过大。** 分阶段改造，每阶段保持 MineBackup 现有流程可运行。
