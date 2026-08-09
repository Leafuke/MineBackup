# MineBackup 无头服务器 CLI

`minebackup-cli` 是可独立部署的服务器产品。它可以从空配置档完成声明式配置、诊断、作业执行、Full/Smart 备份、归档校验和本地冷还原，无需先启动 GUI。CLI 与桌面端复用 `BackupService`、`RestoreService`、FolderRewind metadata 和 `history.json` 格式。

## 安装与资产

正式发行提供：

- `MineBackup-CLI-<version>-windows-x64.zip`：`minebackup-cli.exe` 内嵌并校验固定 7-Zip；
- `MineBackup-CLI-<version>-linux-x64.tar.gz`：便携目录，捆绑固定并校验的 `7zz`；
- `minebackup-cli_<version>_amd64.deb`：安装 CLI、固定 `7zz`、示例 manifest 和 systemd timer。

源码构建不会引入 GLFW、OpenGL、ImGui、X11、Wayland、GTK、GIO 或 AppIndicator：

```bash
cmake --preset linux-x64-cli-only -DBUILD_TESTING=ON
cmake --build build/linux-x64-cli-only --parallel
ctest --preset linux-x64-cli-only --output-on-failure
```

Windows 可使用 `windows-msvc-x64-cli-only` preset；macOS CLI-only 继续构建和回归，但不发布正式资产。自行构建的 Linux/macOS CLI 需要可用且支持 7z、ZIP、LZMA2、Deflate、BZip2 和 Zstandard 的 `7zz`/`7z`。

## 配置档与首次部署

`--data-dir` 指完整 profile 根，不是 `config` 子目录：

```text
/var/lib/minebackup/server/
├── config/config.ini
├── config/jobs.json
├── data/history.json
├── logs/
├── runtime/
└── tools/
```

服务器和自动化应始终传绝对 `--data-dir`。从空目录部署：

```bash
CLI=/opt/minebackup-cli/bin/minebackup-cli
PROFILE=/var/lib/minebackup/server
MANIFEST=/etc/minebackup/server.json

# 生成带规范 UUID 的模板，再编辑存档根、备份根和世界。
"$CLI" --json profile init --output "$MANIFEST"
"$CLI" --json profile validate --file "$MANIFEST"

# 先审计差异，再以事务方式提交 config.ini 和 jobs.json。
"$CLI" --data-dir "$PROFILE" --json profile diff --file "$MANIFEST"
"$CLI" --data-dir "$PROFILE" --json profile apply --file "$MANIFEST" --dry-run
"$CLI" --data-dir "$PROFILE" --json profile apply --file "$MANIFEST"
"$CLI" --data-dir "$PROFILE" --json --no-network doctor
```

manifest 中的本地相对路径以 manifest 所在目录为基准解析，apply 后写为绝对路径。工具路径可留空以自动发现；rclone 的 `remote:path` 不按本地路径解析。apply 不要求世界已存在，`doctor` 会把缺失世界标为未就绪。

`profile apply` 按 `ConfigId`/`JobId` 合并，保留 manifest 未涉及的配置、Job、GUI 字段和未知扩展字段。只有 `--prune --confirm-prune` 会移除未声明 Config/Job；不会删除历史、归档或 metadata。`profile diff --prune` 会报告受影响历史数量和可能成为孤立数据的 ConfigId。跨引用全部验证成功后才同时提交两个文件；失败会恢复旧快照。

已有 GUI 配置可导出后在服务器重新绑定路径：

```powershell
minebackup-cli.exe --data-dir "D:\MineBackupProfile" --json `
  profile export --output "D:\transfer\server.json"
```

上传后修改 manifest 的 `saveRoot`、`backupRoot`、工具路径和云工作目录，再对新的服务器 profile 执行 validate/diff/apply。不要直接复制客户端绝对路径并跳过 `doctor`。

## 命令概览

```text
profile init|validate|diff|apply|export
serve
serve status
serve stop
doctor
config list
config show --config <ConfigId>
world list --config <ConfigId>
history list --config <ConfigId> --world <relative-path>
job list
job show --job <JobId>
job run --job <JobId>
backup --config <ConfigId> --world <relative-path> [--comment <text>]
verify --config <ConfigId> --world <relative-path> (--backup <file> | --latest)
restore --config <ConfigId> --world <relative-path> (--backup <file> | --latest)
        [--mode clean|overwrite] (--dry-run | --confirm)
```

所有 Config、Job、Stage 和 Step 都由 manifest 中必填的规范 UUID 标识。世界参数是配置中的规范相对路径，不接受显示名称或数字索引。

### Job

Job 是一次性工作流，不包含时间触发器。Stage 按数组顺序执行；同一 Stage 内的 Backup/Process Step 并行，必须全部结束后才进入下一 Stage。Process 使用 `executable + arguments[]` 直接启动，不经过 shell；确需 shell 时显式调用 `/bin/sh -c` 或 `cmd.exe /C`。

当前 Stage 失败会跳过后续 Stage。成功与失败混合返回 `partial_success`/10，全部失败返回 `job_failed`/6。Ctrl+C、SIGTERM 和 Ctrl+Break 会请求已启动备份及进程树取消。

### 常驻 Profile Runtime

需要长期监听 KnotLink 或避免每次命令重新载入配置时，启动：

```bash
"$CLI" --data-dir "$PROFILE" --json serve
"$CLI" --data-dir "$PROFILE" --json serve status
"$CLI" --data-dir "$PROFILE" --json serve stop
```

`serve` 独占 profile，并长期持有配置、Job、历史、备份、还原和 KnotLink 运行时。同一用户随后执行原有 `doctor`、查询、apply、backup、job、verify 或 restore 命令时，客户端会通过本机 IPC 提交请求并等待，stdout envelope 与退出码保持不变；系统定时任务不需要改命令。Windows IPC 使用当前用户 ACL，Unix socket 权限为 `0600`，不会开放网络控制端口。若占用者是 GUI 或普通一次性 CLI，仍返回 `profile_busy`。

客户端 Ctrl+C 会把对应 operationId 的取消请求发给服务端。`serve stop` 或服务端 SIGTERM 会停止接单、取消活动 IPC/KnotLink 操作、等待其收尾后以 0 退出；第二个控制信号仍可立即终止。`serve status` 报告 IPC 与 KnotLink 活动操作、网络/KnotLink 状态和运行时间。

### 备份与本地历史

```bash
"$CLI" --data-dir "$PROFILE" --json backup \
  --config 11111111-1111-4111-8111-111111111111 \
  --world world --comment "before upgrade"

"$CLI" --data-dir "$PROFILE" --json history list \
  --config 11111111-1111-4111-8111-111111111111 --world world
```

直接 `backup` 与 Job Backup Step 共用增量变化检测、SkipIfUnchanged、保留策略、KnotLink 在线协调、metadata、HistoryRepository 和 rclone 后处理。`--no-network` 禁用 KnotLink 和云后处理；世界锁定时仍会带 warning 使用 7-Zip `-ssw` 降级。默认网络模式下，本地成功但 rclone 后处理失败返回 10，且不删除本地成果。

### 校验与冷还原

```bash
# --latest 只选择本地历史中最新且归档实际存在的记录。
"$CLI" --data-dir "$PROFILE" --json verify \
  --config 11111111-1111-4111-8111-111111111111 --world world --latest

# 演练完整链规划、metadata 验证和每包 7z t，不改世界。
"$CLI" --data-dir "$PROFILE" --json restore \
  --config 11111111-1111-4111-8111-111111111111 --world world \
  --latest --mode clean --dry-run

# 实际写入必须显式确认；默认模式是 clean。
"$CLI" --data-dir "$PROFILE" --json restore \
  --config 11111111-1111-4111-8111-111111111111 --world world \
  --latest --mode clean --confirm
```

`clean` 先把现有世界切换为同文件系统快照，成功后恢复 preserve 规则并删除快照；解压、提交或取消失败时尝试回滚。Smart clean 缺少完整 metadata 或 Full 基线时会安全拒绝。`overwrite` 逐链覆盖目标，不删除归档中不存在的现有文件，也不承诺完整回滚。配置启用 `restore.backupBefore` 时，实际还原前先执行安全备份。普通 CLI restore 仍是冷还原：检测到世界占用会拒绝，必须先停止服务器并用 `doctor` 确认 `coldRestoreReady=true`。

### KnotLink 查询、备份与热还原

网络模式的 `serve` 长期持有 KnotLink endpoint，支持 `PING`、能力/状态、Config/World/本地历史查询、`BACKUP`、`BACKUP_ALL`、`RESTORE` 和 `MARK_IMPORTANT`。`AUTO_BACKUP`/`STOP_AUTO_BACKUP` 已从能力清单删除；时间触发始终由 systemd timer 或 Task Scheduler 持有。

`RESTORE current_save=true` 使用共享热还原协调器：验证本地历史归档链，默认 `clean`，与模组握手，要求游戏保存并退出，等待世界锁释放，调用与 CLI/GUI 相同的 `RestoreService`，最后通知重新进入世界。多个配置世界同时占用时会拒绝歧义请求；本地链缺失会明确失败，不下载云端数据。还原成功但 rejoin 失败或超时时，归档还原仍视为成功并发出 warning，玩家需手动重进。GUI 也使用同一协调状态机。

KnotLinkService 与联动模组须单独安装并与运行 `serve` 的同一账户/本机会话可达。若本地 KnotLink endpoint 启动失败，`serve status` 的 `knotLinkRunning=false`，普通在线备份继续按既有规则 warning 后使用 `-ssw` 降级，但热还原不可用。`--no-network serve` 明确禁用 KnotLink 与 rclone；转发命令自己的 `--no-network` 只禁用该次操作的网络后处理。

灾难恢复演练至少应定期完成 `verify --latest`、`restore --dry-run`，并在隔离目录/profile 中执行真实 clean restore 后对关键文件做哈希或字节比对。历史和 metadata 必须与归档一起保存；不要只复制 `.7z` 增量包。

## doctor

`doctor` 返回结构化的：配置和 Job schema、Job 跨引用、存档根和各世界存在性、世界占用/冷还原状态、备份根写探针、有效 restore preserve（始终包含 `session.lock`）、7-Zip 能力、rclone 可执行文件及本地 remote 配置，以及 KnotLink listener 状态。

`doctor --no-network` 不启动 KnotLink；rclone `listremotes` 是本地配置检查，云已启用时仍会执行。遗留 `[SpCfg]` 和 `special-tasks.json` 只产生 ignored warning，文件原样保留且永不执行。

## 系统调度

时间计划属于操作系统，不属于 Job。Linux 包含 `minebackup-serve@.service`、`minebackup-backup@.service`、`.timer` 和示例 env。先启动常驻 runtime，再启用 timer：

```bash
sudo install -d -m 0750 /etc/minebackup
sudo cp /usr/share/doc/minebackup-cli/examples/systemd.env /etc/minebackup/server.env
sudoedit /etc/minebackup/server.env
sudo systemctl daemon-reload
sudo systemctl enable --now minebackup-serve@server.service
sudo systemctl enable --now minebackup-backup@server.timer
systemctl status minebackup-serve@server.service
systemctl list-timers 'minebackup-backup@*'
journalctl -u minebackup-backup@server.service
```

模板默认以 `minecraft` 用户运行；按服务器账户调整 User/Group，并确保该账户可写 env 中的 profile、存档和备份路径。定时器启用 `Persistent=true`，错过的计划会在下次启动补跑。

Windows ZIP 的 `scheduling/windows/MineBackup-Serve.xml` 提供开机常驻模板，`MineBackup-Job.xml` 提供定时 Job 模板。替换 `@@MINEBACKUP_CLI@@`、`@@MINEBACKUP_DATA_DIR@@` 和 Job 模板中的 `@@MINEBACKUP_JOB_ID@@` 后，以同一个服务器服务账户导入；先启动 Serve 任务。计划任务动作保持普通 `job run` 命令，它会自动转发，不调用 GUI。

## JSON、日志与退出码

`--json` 保证 stdout 只有一个 schema v1 envelope；日志和进度进入配置档 `logs` 或 stderr。首个取消信号会清理并尽量返回最终 envelope，第二个信号允许立即退出。

| 退出码 | code | 含义 |
|---:|---|---|
| 0 | `success` / `no_changes` | 成功或没有变化 |
| 2 | `invalid_arguments` | 参数错误或缺少显式确认 |
| 3 | `profile_busy` | GUI/另一 CLI 占用 profile |
| 4 | `target_not_found` | Config、World、Job 或备份不存在 |
| 5 | `migration_required` / `invalid_profile` | 需迁移、schema/引用/路径配置无效 |
| 6 | `backup_failed` / `job_failed` / `verification_failed` | 备份、Job 或校验失败 |
| 7 | `restore_failed` | 还原失败 |
| 8 | `tool_unavailable` | 7-Zip/rclone 不可用 |
| 9 | `cancelled` | 已取消 |
| 10 | `partial_success` | Job 部分成功，或本地备份成功但云后处理失败 |

## 兼容性与边界

- CLI-only 不迁移 v1.15 普通 profile；用支持版本的 GUI 导出 manifest，或新建服务器 profile。
- `run-special`、SpecialConfig、内置 interval/scheduled 和 Script Step 已移除；遗留数据不迁移、不删除、不读取。
- GUI 与 `serve` 对同一 profile 严格互斥；GUI 不作为 `serve` 控制客户端。一次性 CLI 在 `serve` 存在时透明转发，在 GUI 或另一普通 CLI 占用时返回 `profile_busy`。
- 不支持 Restore 云补链、手动云分析/下载、历史删除/评论编辑、Agent、Pack Mode、容器镜像或原生 Windows Service 安装器。
- CLI 不弹窗、不读 stdin、不打开浏览器或目录，也不运行更新/公告服务。
