# MineBackup Headless CLI 使用与兼容性说明

`minebackup-cli` 是 MineBackup 的永久非交互前端，面向服务器、计划任务、容器和脚本集成。它与 GUI 调用同一个 `BackupService`，读取同一个配置档，并生成相同的 FolderRewind 归档、metadata 和 `history.json` 数据契约。

> 当前 CLI 是源码构建能力，尚未加入正式安装包、下载资产或版本发布承诺。不要假设现有 GUI 安装包中包含 `minebackup-cli`。

## 构建

CLI-only 配置不会查找或构建 GLFW、OpenGL、ImGui、X11、Wayland、GTK、GIO 或 AppIndicator：

```bash
# Windows（MSVC x64）
cmake --preset windows-msvc-x64-cli-only -DBUILD_TESTING=ON
cmake --build --preset windows-msvc-x64-cli-only-release

# Linux x86_64
cmake --preset linux-x64-cli-only -DBUILD_TESTING=ON
cmake --build build/linux-x64-cli-only --parallel

# Apple Silicon macOS 15+
cmake --preset macos-arm64-cli-only -DBUILD_TESTING=ON
cmake --build build/macos-arm64-cli-only --parallel
```

Windows CLI 独立嵌入并校验项目固定的 7-Zip 资源。Linux/macOS 运行真实备份前必须安装受支持且包含 7z、ZIP、LZMA2、Deflate、BZip2 和 Zstandard 能力的 `7zz`/`7z`。三平台 CI 的构建、真实归档、进程契约和二进制依赖审计见 `.github/workflows/headless-cli.yml`。

## 命令

```text
minebackup-cli [全局参数] doctor
minebackup-cli [全局参数] config list
minebackup-cli [全局参数] world list --config <ConfigId>
minebackup-cli [全局参数] history list --config <ConfigId> --world <relative-path>
minebackup-cli [全局参数] backup --config <ConfigId> --world <relative-path>
minebackup-cli [全局参数] run-special <SpecialConfigId>
```

示例：

```bash
minebackup-cli --data-dir /srv/minebackup-profile config list

minebackup-cli --data-dir /srv/minebackup-profile \
  world list --config 11111111-1111-4111-8111-111111111111

minebackup-cli --data-dir /srv/minebackup-profile --json --no-network \
  backup \
  --config 11111111-1111-4111-8111-111111111111 \
  --world survival/world

minebackup-cli --data-dir /srv/minebackup-profile \
  run-special 22222222-2222-4222-8222-222222222222
```

命令行为：

- `doctor`：检查配置/schema、迁移状态、配置路径、读写条件、7-Zip、按需检查 rclone，以及本地 KnotLink 端口；不访问更新或公告服务，也不迁移任务。
- `config list`：列出稳定 `ConfigId`、名称、存档根目录、备份目录和世界数量。
- `world list`：列出指定配置中的规范化相对路径、描述和目录存在状态。后续命令只接受这里返回的 `path`，不接受显示名称或数字索引。
- `history list`：列出指定世界的本地历史，不下载云端历史。
- `backup`：执行一次真实备份，等待历史提交及配置要求的云端后处理，然后退出。
- `run-special`：预检并执行指定特殊配置；有周期任务时保持前台运行，直到收到信号或发生不可恢复错误。

## 全局参数

| 参数 | 语义 |
|---|---|
| `--data-dir <path>` | 使用指定 MineBackup 配置档；建议服务器和自动化始终显式传入。 |
| `--json` | stdout 仅写入一个 schema v1 最终对象；识别该参数后，参数错误也使用相同 envelope。 |
| `--log-level off\|info\|debug` | 控制配置档 `logs` 目录中的文件日志等级。 |
| `--no-network` | 禁用 KnotLink 和云端后处理；本地成功即成功。 |
| `--non-interactive` | 显式声明非交互契约；幂等，不会开启另一种模式。 |
| `--help` | 显示帮助；不打开配置档，也不获取锁。与 `--json` 同用时返回 JSON envelope。 |
| `--version` | 显示版本；不打开配置档，也不获取锁。与 `--json` 同用时返回 JSON envelope。 |

CLI 永远不会弹框、打开浏览器或目录、启动更新/公告检查，也不会读取 stdin。`doctor`、四个 list/backup 相关命令和 `run-special` 都会独占配置档锁；同一 `data-dir` 已被 GUI 或另一个 CLI 使用时不会转发 GUI IPC，而是退出 3。

## JSON 契约

```json
{
  "schemaVersion": 1,
  "command": "backup",
  "ok": true,
  "code": "success",
  "data": {},
  "diagnostics": [
    {
      "eventId": "backup.completed",
      "severity": "info",
      "detail": ""
    }
  ]
}
```

- `code`、`eventId` 和 `severity` 是稳定、非本地化的机器字段。
- `ok` 仅在 `success` 或 `no_changes` 时为 `true`；`partial_success` 为 `false`，脚本应结合退出码处理。
- 日志和进度只能进入 stderr 或日志文件，不能混入 JSON stdout。
- 首个取消信号仍保证正常清理和最终 JSON；第二个信号允许立即结束，因此不保证 JSON。
- schema 版本变化必须按版本解析，脚本不应依赖未文档化字段。

## 退出码

| 退出码 | code | 语义 |
|---:|---|---|
| 0 | `success` / `no_changes` | 成功、没有变化，或特殊配置没有启用任务。 |
| 2 | `invalid_arguments` | 参数或命令错误。 |
| 3 | `profile_busy` | 配置档锁冲突。 |
| 4 | `target_not_found` | Config、World 或 SpecialConfig 不存在。 |
| 5 | `migration_required` / `invalid_profile` | 需要 GUI 迁移、配置损坏或任务 schema 不兼容。 |
| 6 | `backup_failed` / `task_failed` | 备份或特殊任务执行失败。 |
| 7 | — | 为未来 Restore 保留，本版不会返回。 |
| 8 | `tool_unavailable` | 必需外部工具不可用。 |
| 9 | `cancelled` | 操作被 SIGINT、SIGTERM、Ctrl+C 或 Ctrl+Break 取消。 |
| 10 | `partial_success` | 部分成功，例如本地归档成功但云端后处理失败。 |

`NoChanges` 不产生新归档，也不会触发云端上传。云同步失败不会删除已经成功提交的本地归档和历史记录。

## 网络与热备份

默认情况下，CLI 会启动不依赖桌面的 KnotLink listener，并等待配置要求的 rclone 后处理：

- 世界未锁定时不会进行热备份握手。
- KnotLink 端口不可用或握手不可用时会产生稳定 warning，并沿用现有 7-Zip `-ssw` 活文件降级；warning 本身不构成部分失败。
- 已握手但无法完成世界保存确认时，备份会被拒绝，避免把一次失败协调误报为成功。
- `--no-network` 同时关闭 KnotLink 与云同步；若锁定世界仍需备份，会明确报告降级 warning。
- 本地备份完成后，CLI 同步等待 rclone；上传失败返回 10，并保留本地产物。

`doctor --no-network` 不探测 KnotLink 或 rclone，但仍检查本地配置、路径和 7-Zip。

## 特殊任务与调度

特殊任务的权威文件是：

```text
<data-dir>/config/special-tasks.json
```

schema v1 使用 UUID `taskId`、稳定 `ConfigId` 和配置内 `worldPath`；数组顺序就是执行顺序。`worldPath` 不能是绝对路径、不能包含 `..`，并统一使用 `/`。

- Backup 支持 `once`、`interval` 和 `scheduled`。
- interval 启动后先等待一个完整间隔，不会立即运行。
- scheduled 等待下一个合法时间；month/day 为 `0` 表示通配。
- Command v1 只支持 `once`；Shell 非零返回会成为任务失败。
- Script 会无损保留，但本版不执行；`doctor` 报告问题，`run-special` 在启动任何任务前退出 5。
- 所有任务会先完成 schema、目标和 7-Zip 预检；无效配置不会先执行一部分任务。
- 一次性任务完成后进程总是退出，忽略旧 `exitAfterExecution=false` 的驻留行为。
- 多个一次性任务成功与失败并存时返回 10；全部失败返回 6。

## 信号与服务管理器

首个 SIGINT/SIGTERM（Windows 对应 Ctrl+C/Ctrl+Break）停止新调度，请求当前 `BackupService`、Shell 和子进程树取消，并等待临时产物清理后退出 9。第二个信号允许立即退出 9，不保证最终 JSON 或额外清理。

CLI 是前台进程，不安装系统服务。systemd、Task Scheduler、launchd 和容器入口应直接管理该进程及其退出码；本里程碑不提供正式服务模板。

## 迁移与兼容性

- `config.ini` 的普通配置和桌面设置保持现有格式；CLI 不创建或编辑配置。
- 缺少稳定 `ConfigId`/`SpecialConfigId` 的 v1.15 配置档必须先由普通 GUI 启动完成兼容迁移；CLI 返回 5，不隐式生成身份。
- `doctor` 和只读 list 命令只报告旧特殊任务待迁移，不写盘。普通 GUI 启动或 `run-special` 会按需迁移到 `special-tasks.json`。
- JSON 原子写入成功后立即成为权威来源；未来 schema 会被拒绝，不会回退解析旧 INI 任务。
- 无法解析的旧逗号行、无效/越界目标或重复身份会使迁移整体失败，不写半成品 JSON。
- 迁移或手工编辑前，应同时备份 `config/config.ini` 与 `config/special-tasks.json`；不要只备份其中一个。

桌面程序不再执行 `--run-special` 或 `-specialcfg`；这些参数会在任何桌面/网络初始化前写 stderr、提示等价的 `minebackup-cli run-special <id>` 命令并退出 2。`--autostart` 仅作为 GUI 登录启动项的内部标记，不执行特殊任务；GUI 设置决定是否创建登录项以及登录启动时是否隐藏到托盘。`--silent-startup` 和 `--select-config` 继续维持 GUI 语义。

本版 CLI 不支持名称选择、配置创建/编辑、Restore、浏览器/目录打开、Agent、Pack Mode、服务安装或正式发行打包。
