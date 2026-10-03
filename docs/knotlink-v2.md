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
commands `BACKUP`, `RESTORE`, `BACKUP_ALL`, and `MARK_IMPORTANT` require both
fields.

Query `data` is a single outer percent-encoded scalar and deliberately uses
the same command-specific payloads as FolderRewind:

- `LIST_CONFIGS`: `config-id,name;config-id,name`
- `LIST_FOLDERS`: `folder-name;folder-name`
- `LIST_BACKUPS`: `archive.7z;archive.zip`
- `GET_CONFIG`: `name=...;backup_mode=...;format=...;keep_count=...`
- `GET_STATUS`: `enabled=...;initialized=...;active_tasks=...`

The separators above are part of the decoded `data` value. They are encoded
as `%2C`, `%3B`, and `%3D` on the wire; `data` is not a JSON array or object.

```text
cmd=BACKUP;from=example.mod;request_id=req-42;config_id=primary;folder=0;comment=Before%20update
status=ok;from=example.mod;request_id=req-42;message=Command%20accepted.
event=command_started;from=example.mod;request_id=req-42;command=BACKUP
event=command_completed;from=example.mod;request_id=req-42;command=BACKUP;message=Backup%20created.
```

Background work emits `command_accepted`, `command_started`, then
`command_completed` or `command_failed`. Backup, restore, backup-all, and
importance-change business events carry the same correlation metadata.

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

Hot-backup notifications are `backup_started`, optionally `backup_warning`,
then exactly one terminal `backup_success` or `backup_failed`. A no-change
backup uses `command_completed;command=BACKUP;result=no_changes` as its
terminal signal. These terminal signals release the companion mod's auto-save
freeze and therefore must be emitted even for GUI-initiated operations.

Integrated-server hot restore uses the ordered lifecycle below. Every event
after the handshake carries the same `world` (and `request_id` when present):

```text
handshake -> pre_hot_restore -> restore_finished(status=success)
          -> rejoin_world -> hot_restore_complete
```

The companion mod accepts `rejoin_world` only after a matching
`restore_finished` advanced its active-world session. MineBackup retains the
FolderRewind 100 ms post-restore delay and 3 second rejoin stabilization delay.

When `RESTORE` omits `file`, MineBackup selects the latest local archive from
history; it does not guess from filesystem timestamps or download a cloud chain.

One-shot backup overrides are never persisted. `backup_mode` accepts `full`
or `incremental`. `compression_method` accepts `LZMA2`, `Deflate`, `BZip2`,
or `zstd`; levels are 0-9 for LZMA2/Deflate, 1-9 for BZip2, and 1-22 for
zstd. `backup_blacklist` is merged into a runtime configuration copy.

`backup_whitelist` selects source-relative literal paths or `*`/`?` wildcards.
It overrides configured blacklist rules; mandatory lock/internal exclusions still
apply. Regex rules, absolute paths, and traversal are rejected. `backup_scope`
accepts `selected-regions` with `scope_dimensions` and `scope_areas`. Dimensions
are overworld, nether, and end (default overworld); comma, semicolon, and pipe
separators are accepted inside the encoded scalar. Each area line contains block
coordinates `x1,z1,x2,z2`; coordinates must be finite and within ±30 million.
Limits are 32 KiB, 128 area lines, and 4096 selected regions. Vanilla legacy and
post-26 layouts are supported; ambiguous layouts and Paper dimensions outside
the configured source are rejected.

Any whitelist or selected-region backup creates an independent `[Partial]`
archive and metadata record. It never advances the ordinary Full/Smart baseline
or runs ordinary count retention. This applies even if `backup_mode=incremental`
was requested. Empty/full/all/default/none scope values select the whole world;
nonempty scope dimensions or areas require `selected-regions`.

Restore `mode` defaults to `clean` and also accepts `overwrite`.
`restore_whitelist` applies only to that operation. Clean restore from a
partial backup requires `confirm_partial_clean=true`.

`preserve_player_data=true` preserves current Java player NBT during restore.
`restore_preserve_paths` accepts exact world-relative files and directories,
for example `ftbquests%2F,ftbteams%2F`. Their current state wins after extraction,
including deleting a backed-up path when it is absent in the current world.
Both options are operation-only and apply to clean and overwrite restores.
Validation or preparation failure fails the operation before committing changes.
Existing `restore_whitelist` semantics are retained separately: on clean restore,
matching existing files replace archive files and `session.lock` is always kept.
An omitted desktop whitelist uses the saved profile rules.

List parameters prefer independently percent-encoded items separated by literal
commas. For older callers, a single fully encoded CSV scalar is also accepted
and decoded exactly once. Consequently a one-item backup pattern containing a
literal comma is ambiguous with legacy CSV and is unsupported for whitelist
rules. A blacklist may use a regular expression matching that character instead. Exact preserve paths cannot contain commas, so the
fallback is unambiguous for them. Empty preserve-path list elements are rejected.

Unknown preservation, backup, compression, and scope parameters and parameters
used on the wrong operation receive a structured `unsupported_parameter` error.
Unrelated extension keys remain ignored. Capabilities advertise only implemented
operation parameters; callers should inspect `GET_CAPABILITIES`.

Mod callbacks are accepted only for the pending lifecycle phase. A supplied
world must match the pending world. Companion integrated-server releases create
a new `request_id` for each callback, while the dedicated sidecar may echo the
original request; both forms are accepted. Command responses and business events
still carry the original command's `from` and `request_id`. These identifiers
correlate messages and are not an authentication mechanism. Because old callbacks
may omit world identity, desktop GUI and remote backup/hot-restore operations
share one serialized mod conversation; nested safety backups remain supported.
The headless Profile Runtime likewise serializes operations.

Removed commands and aliases include `AUTO_BACKUP`, `STOP_AUTO_BACKUP`,
`SET_CONFIG`, `BACKUP_MODS`, `ADD_TO_WE`, `SEND`,
`SHUTDOWN_WORLD_SUCCESS`, `LIST_WORLDS`, and every `*_CURRENT` command.
Scheduling belongs to the operating system. Local console business commands
also use v2 payloads; `HELP`,
`CLEAR`, and `HISTORY` remain local controls.

## Capability manifest

`GET_CAPABILITIES` returns the funcList JSON embedded in the executable:

- `specVersion=1.0`
- `manifestVersion=2.1.0`
- response `encoding=percent`
- `appID=0x00000020`
- `openSocketID=0x00000010`
- `signalID=0x00000020`

The manifest advertises only the commands and parameters implemented by
MineBackup.

## Versions and server lifecycle

The companion mod minimum is 3.0.0. Handshake waits up to three seconds.
World save, save-and-exit, file release, and rejoin retain the FolderRewind
10/15/30-second workflow timeouts.

On Windows, KnotLinkService 3.2.0.0 or newer is required. MineBackup discovers
the executable and version from App Paths and both uninstall registry views,
then falls back to the PE file version. Installed unknown and older versions
are blocked and produce a dismissible reminder on every startup.
The `AutoStartKnotLinkServer` setting defaults to `1`; a compatible installed
service may be started and must expose loopback ports 6370 and 6378 within 10
seconds.

Linux discovers the `knotlinkservice` dpkg package and macOS discovers the
`com.knotlink.service` Installer receipt. The installed services are managed by
systemd and launchd. The first-run wizard and Settings can download the official
3.2.0.0 package, retry through `gh-proxy.org`, and open the platform installer;
MineBackup does not automate the remaining installer steps.

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
`RESTORE`、`BACKUP_ALL`、`MARK_IMPORTANT`。模组回调为
`HANDSHAKE_RESPONSE`、`WORLD_SAVED`、
`WORLD_SAVE_AND_EXIT_COMPLETE`、`REJOIN_RESULT`。

`current_save=true` 让 `BACKUP`、`LIST_BACKUPS`、`RESTORE` 操作当前世界；
`RESTORE` 不提供 `file` 时只按本地历史选择最新且存在的备份，默认使用
`clean`。一次性备份模式、压缩设置、黑名单和还原白名单只作用于当前任务，
不写回配置。

能力清单版本现为 `2.1.0`。`backup_whitelist` 支持相对路径和 `*`/`?`
通配符；`backup_scope=selected-regions` 配合 `scope_dimensions`、
`scope_areas` 选择维度和区域。受限备份生成独立 `[Partial]` 包，不推进普通
Full/Smart 链，不执行普通数量保留策略。对独立 Partial 包进行 clean 还原
必须显式提供 `confirm_partial_clean=true`；完整 Smart 链不按独立 Partial
处理。

`preserve_player_data=true` 在还原时保留当前 Java 玩家 NBT。
`restore_preserve_paths` 指定当前状态优先的精确世界相对路径，例如
`ftbquests%2F,ftbteams%2F`；当前不存在的选中路径会从还原结果中删除。
它们同时适用于 clean 和 overwrite，仅作用于单次请求。路径保留在玩家 NBT
处理后执行，失败时不能报告还原成功。原有 `restore_whitelist` 行为保持独立。

列表优先逐项 percent-encoding、以原始逗号分隔，也接受旧调用者把整个 CSV
值编码一次的形式。含逗号的单条 whitelist 规则有歧义，因此不支持。
危险选项拼写错误或用于错误命令时会被拒绝，不会静默忽略。
桌面 GUI 与远程热备份/热还原共用串行模组会话；模组回调按阶段和可选世界名
检查。已发布集成服务器模组会为每个回调生成新 request_id，因此回调 UUID
不能当成原操作 UUID 或认证凭据。

热备份依次发送 `backup_started`、可选的 `backup_warning`，以及唯一终态
`backup_success`/`backup_failed`；无变化时以
`command_completed;command=BACKUP;result=no_changes` 作为终态。这些终态
负责解除联动模组的自动保存冻结，因此 GUI 发起的备份也必须发送。

集成服务器热还原严格遵循
`handshake -> pre_hot_restore -> restore_finished(status=success) ->
rejoin_world -> hot_restore_complete`。握手后的每个事件都携带相同的 `world`
（有请求 ID 时也携带 `request_id`）；联动模组只有在匹配的
`restore_finished` 推进当前世界状态后才接受 `rejoin_world`。实现保留
FolderRewind 的还原后 100 ms 等待和重进前 3 秒稳定窗口。

`AUTO_BACKUP`、`STOP_AUTO_BACKUP` 已移除且不会出现在能力清单；调度由
systemd timer 或 Task Scheduler 持有。

联动模组最低版本为 3.0.0。KnotLinkService 推荐最低版本为 3.2.0.0。
Windows 读取注册表和文件版本，Linux 读取 dpkg 包信息，macOS 读取 Installer
收据；已安装的未知或旧版本会被阻止，并在每次启动时显示可关闭提醒。首次
向导和设置页可以从
[KnotLinkService 官方发布](https://github.com/KnotLink-Protocol/KnotLinkService/releases)
下载对应安装包，官方地址失败后尝试 `gh-proxy.org`，随后交给系统安装器。
