# MineBackup KnotLink v2 interoperability

MineBackup implements the FolderRewind v2 parameterized protocol only. There
is no negotiation, positional-command migration, old alias, or free-text
compatibility layer.

## Wire format

A request is a non-empty semicolon-separated map:

```text
key=value;key2=value2
```

Keys contain only ASCII letters, digits, and underscores. They are
case-insensitive and normalized to lowercase. `cmd` is required; its decoded
value contains only letters, digits, and underscores and is normalized to
uppercase for dispatch.

Values use RFC 3986 percent-encoding. Lists use a literal comma between items,
with each item encoded independently. For example, the list `["a,b", "主世界"]`
is encoded as `a%2Cb,%E4%B8%BB%E4%B8%96%E7%95%8C`.

MineBackup rejects empty segments, duplicate keys, multiple equals signs,
invalid keys, malformed percent escapes, raw reserved characters, an empty
`cmd`, and a missing `cmd`. A payload such as `BACKUP 0 0` receives only a v2
upgrade diagnostic and never executes.

Responses always begin with `status=ok` or `status=error`. Once a request is
parsed, its response and events inherit `from` and `request_id`. The mutating
commands `BACKUP`, `RESTORE`, `BACKUP_ALL`, `AUTO_BACKUP`,
`STOP_AUTO_BACKUP`, and `MARK_IMPORTANT` require both fields.

Query `data` is a single outer percent-encoded scalar and deliberately uses
the same command-specific payloads as FolderRewind:

- `LIST_CONFIGS`: `config-id,name;config-id,name`
- `LIST_FOLDERS`: `folder-name;folder-name`
- `LIST_BACKUPS`: `archive.7z;archive.zip`
- `GET_CONFIG`: `name=...;backup_mode=...;format=...;keep_count=...`
- `GET_STATUS`: `enabled=...;initialized=...;active_auto_backups=...;active_tasks=...`

The separators above are part of the decoded `data` value. They are encoded
as `%2C`, `%3B`, and `%3D` on the wire; `data` is not a JSON array or object.

```text
cmd=BACKUP;from=example.mod;request_id=req-42;config_id=primary;folder=0;comment=Before%20update
status=ok;from=example.mod;request_id=req-42;message=Command%20accepted.
event=command_started;from=example.mod;request_id=req-42;command=BACKUP
event=command_completed;from=example.mod;request_id=req-42;command=BACKUP;message=Backup%20created.
```

Background work emits `command_accepted`, `command_started`, then
`command_completed` or `command_failed`. Backup, restore, backup-all,
auto-backup, and importance-change business events carry the same correlation
metadata.

## Commands

Queries:

- `PING`
- `GET_CAPABILITIES`
- `GET_STATUS`
- `LIST_CONFIGS`
- `LIST_FOLDERS`
- `LIST_BACKUPS`
- `GET_CONFIG`

Operations:

- `BACKUP`
- `RESTORE`
- `BACKUP_ALL`
- `AUTO_BACKUP`
- `STOP_AUTO_BACKUP`
- `MARK_IMPORTANT`

Mod callbacks:

- `HANDSHAKE_RESPONSE`
- `WORLD_SAVED`
- `WORLD_SAVE_AND_EXIT_COMPLETE`
- `REJOIN_RESULT`

`config_id` resolves in this order: stable `ConfigId`, configuration name,
then numeric configuration key. `folder` accepts a zero-based index, world
name, or full path.

Current-world backup, listing, and restore reuse the normal commands with
`current_save=true`:

```text
cmd=LIST_BACKUPS;current_save=true
cmd=BACKUP;from=example.mod;request_id=req-43;current_save=true;comment=Live%20snapshot
cmd=RESTORE;from=example.mod;request_id=req-44;current_save=true
```

When `RESTORE` omits `file`, MineBackup selects the latest archive.

One-shot backup overrides are never persisted. `backup_mode` accepts `full`
or `incremental`. `compression_method` accepts `LZMA2`, `Deflate`, `BZip2`,
or `zstd`; levels are 0-9 for LZMA2/Deflate, 1-9 for BZip2, and 1-22 for
zstd. `backup_blacklist` is merged into a runtime configuration copy.

Restore `mode` defaults to `overwrite` and also accepts `clean`.
`restore_whitelist` applies only to that operation. Clean restore from a
partial backup requires `confirm_partial_clean=true`.

MineBackup does not implement regional scope, backup whitelist, or NBT player
data preservation. A non-empty `backup_whitelist`, `backup_scope`, or
`scope_*`, or `preserve_player_data=true`, receives a structured
`unsupported_parameter` error. These fields are absent from the capability
manifest. Other unknown extension keys are ignored.

Removed commands and aliases include `SET_CONFIG`, `BACKUP_MODS`, `ADD_TO_WE`,
`SEND`, `SHUTDOWN_WORLD_SUCCESS`, `LIST_WORLDS`, and every `*_CURRENT`
command. Local console business commands also use v2 payloads; `HELP`,
`CLEAR`, and `HISTORY` remain local controls.

## Capability manifest

`GET_CAPABILITIES` returns the funcList JSON embedded in the executable:

- `specVersion=1.0`
- `manifestVersion=2.0.0`
- response `encoding=percent`
- `appID=0x00000020`
- `openSocketID=0x00000010`
- `signalID=0x00000020`

The manifest advertises only the commands and parameters implemented by
MineBackup.

## Versions and server lifecycle

The companion mod minimum is 3.1.0. Handshake waits up to three seconds.
World save, save-and-exit, file release, and rejoin retain the FolderRewind
10/15/30-second workflow timeouts.

On Windows, KnotLinkService 3.0.0 or newer is required. MineBackup discovers
the executable and version from App Paths and both uninstall registry views,
then falls back to the PE file version. Unknown and older versions are blocked.
The `AutoStartKnotLinkServer` setting defaults to `1`; a compatible installed
service may be started and must expose loopback ports 6370 and 6378 within 10
seconds.

Linux and macOS are client-only until KnotLink publishes servers for those
platforms. If a future local server exposes the fixed ports, the client can
connect without a MineBackup update.

MineBackup never downloads or runs a KnotLink installer. Use the
[official KnotLink releases](https://github.com/KnotLink-Protocol/KnotLink/releases).

---

# MineBackup KnotLink v2 互联说明

MineBackup 仅实现与 FolderRewind 完全一致的 v2 参数化协议，不协商旧协议，
也不兼容位置参数、旧别名或自由文本。

请求固定为 `key=value;key2=value2`。键只允许 ASCII 字母、数字和下划线，
大小写不敏感；`cmd` 必填。值使用 RFC 3986 percent-encoding，列表以逗号
分隔并逐项编码。空段、重复键、多个等号、非法键、非法 `%` 和缺失 `cmd`
都会被拒绝。

响应统一为 `status=ok|error`。可变更状态的命令必须携带 `from` 与
`request_id`；响应、`command_accepted`、`command_started`、
`command_completed`/`command_failed` 以及业务事件都会继承这两个关联字段。

查询响应的 `data` 是一个整体进行外层 percent-encoding 的标量，并严格沿用
FolderRewind 的命令专属内部格式：`LIST_CONFIGS` 为
`配置ID,名称;配置ID,名称`，`LIST_FOLDERS` 为 `文件夹名;文件夹名`，
`LIST_BACKUPS` 为 `备份包;备份包`，`GET_CONFIG` 和 `GET_STATUS` 为内嵌的
分号分隔键值串。线上的逗号、分号和等号分别编码为 `%2C`、`%3B`、`%3D`；
这些 `data` 不是 JSON 数组或对象。

查询命令为 `PING`、`GET_CAPABILITIES`、`GET_STATUS`、`LIST_CONFIGS`、
`LIST_FOLDERS`、`LIST_BACKUPS`、`GET_CONFIG`。操作命令为 `BACKUP`、
`RESTORE`、`BACKUP_ALL`、`AUTO_BACKUP`、`STOP_AUTO_BACKUP`、
`MARK_IMPORTANT`。模组回调为 `HANDSHAKE_RESPONSE`、`WORLD_SAVED`、
`WORLD_SAVE_AND_EXIT_COMPLETE`、`REJOIN_RESULT`。

`current_save=true` 让 `BACKUP`、`LIST_BACKUPS`、`RESTORE` 操作当前世界；
`RESTORE` 不提供 `file` 时选择最新备份。一次性备份模式、压缩设置、黑名单
和还原白名单只作用于当前任务，不写回配置。

联动模组最低版本为 3.1.0。Windows KnotLinkService 最低版本为 3.0.0，
默认允许智能启动；未知或旧版本会被阻止。Linux/macOS 暂时只保留客户端。
MineBackup 不会自动下载安装器，只会打开
[KnotLink 官方发布页](https://github.com/KnotLink-Protocol/KnotLink/releases)。
