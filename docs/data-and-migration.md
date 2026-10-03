# Data locations, portable mode and 1.15 migration

MineBackup does not depend on the current working directory and does not require
`config.ini` beside the executable. A profile owns separate `config`, `data`,
`state`, `cache`, `runtime`, `tools` and `logs` roots. `--data-dir <absolute
path>` selects the complete profile root and takes precedence over every other
mode; an invalid explicit path is an error rather than a silent fallback.

Default locations:

- Windows: `%LOCALAPPDATA%\MineBackup\{config,data,state,cache,runtime,tools,logs}`.
- Linux: the matching XDG config/data/state/cache/runtime roots. If
  `XDG_RUNTIME_DIR` is absent or unsafe, MineBackup uses a private mode-0700
  runtime directory below the state root.
- macOS: config/data/state/tools under `~/Library/Application Support/MineBackup`,
  cache/runtime under `~/Library/Caches/MineBackup`, and logs under the standard
  Library Logs location.

On Windows and AppImage only, an adjacent `portable.flag` selects
`MineBackupData/{config,data,state,cache,runtime,tools,logs}` beside the
executable/AppImage. macOS applications never write inside `.app`. AppImage
uses normal XDG locations unless the marker exists.

## Migrating from 1.15

Startup order is parameters → AppPaths → per-profile single-instance lock → old
location discovery and confirmation → transactional 1.15 conversion → 1.16
data load → desktop, task and network services. Source files are not deleted,
moved, renamed or recompressed. Recovery snapshots are retained below
`state/migration-snapshots/1.15/<transaction-id>` and the UI reports their
location and size.

The permanent Migration Coordinator owns transactions, reports and write gates.
The 1.16-only V15 Migration Adapter and read-only legacy reader interpret old
formats. Failed dependent units remain pending and block only their dangerous
writes. Run 1.16 to migrate old data before upgrading to a future 1.17 release,
where the v1.15 reader and adapter are removed.

## Portable cloud configuration and rclone

Cloud configuration exchange uses `portable-config.json`, keyed by stable
ConfigId and restricted to an explicit portable-field whitelist. Paths, tool
locations, credentials, commands, scripts, automation, special configurations
and legacy Service Mode fields are excluded. New remote configurations remain
pending until local world and backup paths are bound.

rclone is not included in MineBackup packages. MineBackup installs only the
version pinned by its release manifest, only after user confirmation, from the
official rclone source and after SHA-256 plus `rclone version` validation.
MineBackup does not copy, parse or upload the user's rclone credential file.

## Windows Service Mode

Service Mode is deprecated in 1.16. MineBackup cannot install or start a
service. The local legacy fields are preserved read-only so the settings page
can identify an older service. Removal requires user confirmation, UAC, an
absolute `MineBackup.exe ... --service` ImagePath and MineBackup resource
validation; otherwise the program leaves the service untouched and gives manual
inspection guidance. The compatibility fields are removed in 1.17.

## 配置档事务与恢复

桌面保存将 config.ini 与 jobs.json 一起提交；服务器 manifest apply 必要时同时提交 history.json。
共享事务持久化原文件快照和 prepared 日志，再替换目标，最后写 committed 标记。
失败后完整回滚才返回 NotCommitted；回滚失败保留快照并返回 RecoveryRequired，禁止后续依赖写入。
CommittedNotDurable 表示已提交但持久性未完全确认，不允许回滚内存。恢复兼容原有 v1 事务日志。
GUI 和独占写入入口在加载前恢复；只读 CLI 只检查日志，不修改配置档。
此事务整理不改变云端 v1.15 元数据迁移流程。
