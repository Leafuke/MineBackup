# Time Machine 联动与 Linux 验证报告

日期：2026-10-03。本文覆盖本次 MineBackup `develop` 改进；不将模拟协议测试、真实文件恢复或编译通过等同于 Minecraft 内完整旅行验收。

## 1. 对照版本

- MineBackup 基线：`e5940edf1c1f38f129dfe79737c22661f8361dcd`
- [FolderRewind 1.9.x](https://github.com/Leafuke/FolderRewind/commit/c6ce8a1df0ad41c17add7d2762f988fb07d65912)
- [MineRewind 1.9.x / 1.9.4](https://github.com/Leafuke/FolderRewind-Plugin-Minecraft/commit/45fbe348d926643cbbd3d670901331147eb0b958)
- [Time-Machine main](https://github.com/Leafuke/Time-Machine/commit/4b5247fdfb8dbca743a60791c21fe48633330e5a)
- 时间机器随附 `minebackup-neoforge-1.21-3.3.2.jar`：SHA-256 `996efbf4d7efb00f1ec115b58fe897876140b42176b7f3069ad3d26667e73b39`

该 JAR 已做静态接口和字节码核对。其说明所指的 MineBackup-Mod 源提交 `deb152e9bef565a89acb01d9074ef698ef2faecc` 在核对时尚不能从 GitHub 解析；公开主分支仍为 `e0c1b693e16bf62bf6846db259836e9645720f69`，不含同一能力查询接口。因此复现实验应核对 JAR 哈希，不能仅凭“3.3.2”版本号判断接口一致。

## 2. 本次实现

### 玩家状态保留

`RESTORE preserve_player_data=true` 现在执行实际、受限的 NBT 保留准备，而不是仅放宽参数校验。`false` 不进行玩家覆盖，未提供参数维持 MineBackup 原有行为。时间机器会自行显式提交 `true`。

对当前世界所有玩家（含离线玩家）保留以下字段：

`Pos`、`Rotation`、`Dimension`、`Inventory`、`EnderItems`、`XpLevel`、`XpP`、`XpTotal`、`Score`、`playerGameType`、`Health`、`foodLevel`、`foodSaturationLevel`。

- 备份中已有玩家：只覆盖所列字段，其他字段回到备份状态
- 备份中没有的当前玩家：保留完整当前玩家 compound
- 旧布局：处理 `playerdata/`，以 `level.dat → Data.Player` 为单人内嵌玩家权威数据，并同步同 UUID 文件
- 新布局：处理 `players/data/` 和 `Data.singleplayer_uuid`
- UUID、布局、NBT 数据损坏或身份冲突会明确拒绝，不猜测、不静默跳过
- 成就、统计、世界方块、第三方模组其他状态不因此全部保留；这也不是“仅保留舱内乘员”

支持 Java 大端命名 NBT 的原始或 gzip 文件。不进行版本升级/降级或跨布局迁移。安全上限：单文件 16 MiB，累计输入、解码和输出各 64 MiB，最多 4096 个输出文件，深度 64，标签数量受限。大规模服务器超限会拒绝，需后续设计可流式验证的限额配置，不能简单取消限额。

### 指定当前文件／目录保留

`restore_preserve_paths` 与原有 `restore_whitelist` 分开：

- 精确文件或以 `/` 结尾的完整目录；不是 glob 或任意系统路径
- 相对唯一 Minecraft 世界根解析，支持服务器目录中自定义命名的嵌套世界，不硬编码 `world/`
- 当前同名文件优先；当前新增文件保留；当前已删除的历史文件不会复活
- FTB 模板应一起选 `ftbquests/` 与 `ftbteams/`
- 最多 16 个选择项；准备文件加删除项最多 4096；冻结的当前内容最多 64 MiB
- 拒绝越界、链接／重解析点、模糊的世界根、大小写冲突和非法路径
- 玩家字段覆盖先执行，精确文件保留后执行，后者是显式的整文件覆盖

原有 MineBackup `restore_whitelist` 已有“当前匹配文件覆盖同名历史文件”的实现与测试，本次保留该兼容语义。它与 FolderRewind 同名参数当前语义不同；而 MineBackup 桌面端显式传入旧白名单时替换本次列表，无头端则追加到配置档列表，这一旧行为仍需后续统一。需要跨后端一致保留时应使用新的 `restore_preserve_paths`。

### 恢复提交安全性

启用新保留规则时，先在同文件系统的隔离目录解压并准备全部内容，再替换目标世界。准备失败或取消不应改变当前世界；提交阶段失败使用旧世界快照回滚。clean 和 overwrite 都走受保护的准备流程。

额外检查包括：

- 解压前拒绝归档符号链接、硬链接和不安全路径
- 解压后、清理内部标记及复制保留内容前重新验证整棵目录
- 服务器目录中的嵌套 `level.dat` 所在世界也检查占用，不只检查最外层目录
- 成功提交后清理快照失败记为警告，不能拿不完整快照覆盖已提交世界

这不是任意断电场景下的崩溃一致性承诺，也不替代 Minecraft 正确停服／保存退出。

### 单次备份规则

新增白名单和选定区域规则，与压缩算法、等级、Full/Smart 参数一起保持为单次操作，不写回全局配置。

- 白名单支持受限的相对路径／通配符规则；拒绝无超时保障的正则表达式
- 白名单模式覆盖普通配置黑名单的选择方式，但强制排除锁文件和内部保留项
- 区域按方块坐标转换到完整 region 文件，包含相应 entities、poi 和外部区块文件及必要世界／玩家元数据
- 维度／区域选择受世界布局和管理目录边界约束，不读取管理范围外的 Paper 兄弟目录
- 不支持的维度布局、范围或参数必须拒绝，不能退化成全世界备份后仍报告区域备份成功

局部结果使用独立 `Partial` 类型与记录，不伪装成 Full，不推进普通 Smart 的 `state.json`，不打断普通 Full/Smart 保留链。恢复 Partial 默认应选择 overwrite；clean 必须显式 `confirm_partial_clean=true`，否则拒绝。即使显式确认，clean 也会删除未包含在局部备份中的内容，应谨慎使用。

### 协议与桌面／无头一致性

- 能力清单声明实际参数与 `true` / `false` 选项，供时间机器公共 API 查询
- 桌面及 headless 都把操作规则传给共享服务
- 拒绝未知的备份／恢复／压缩／范围参数及放错命令的已知参数，避免悄悄忽略关键保留规则；无关扩展键沿用兼容行为，不宣称全部未知键都被拒绝
- 查询、异步事件、还原结果保留请求关联信息
- 模组回调按当前阶段和世界验证；集成服务器实际使用新的回调 UUID，不能错误要求它等于原始操作 UUID
- 桌面共享模组会话串行化，避免两个窗口／远程操作竞争同一回调状态
- “还原已提交”“重新进入成功”“时间机器乘员到达完成”仍是不同概念；重新进入成功不能单独证明还原成功

## 3. Linux 测试环境

- Debian 13.6，x86_64，glibc 2.41，GCC/G++ 14.2.0
- CMake 4.4.3，Ninja 1.13.2
- 官方 Debian 开发依赖置于独立测试目录，未改变系统安全设置
- GUI 编译启用 X11、Wayland、GTK 3 和 Ayatana AppIndicator；动态依赖检查无缺失
- 7-Zip ZS：仓库固定的 `v26.02-v1.5.7-R2`，Linux 下载资产 SHA-256：`be246e5a284d3b5e738bad5cbb24c2662996ddb9776e09575b5099ab53fa0ba3`
- 全部文件测试使用新建的隔离配置档和合成存档，未操作真实用户世界

### 当前环境限制：AF_UNIX 被拒绝

此环境能创建 AF_INET socket，但 Unix-domain socket 创建返回 `EPERM`；独立 Python 探针和正常／扩展执行检查结果一致。CLI 的配置档单实例锁与本地 IPC 依赖 AF_UNIX，因此需要持有配置档锁的 CLI 操作在启动阶段失败。GUI 也在窗口出现前退出：

```
[MineBackup] Could not create the profile instance socket.
```

这属于本测试环境限制，不能泛化为 Debian/Linux 不可用。本次没有关闭锁、绕过 IPC 或修改系统安全策略来制造“通过”。因此此机器上的实际 GUI 点击流程、serve 多客户端、真实模组全链路不能计为通过。

## 4. 验证结果的层级

### 基线

1. CLI-only 配置与编译通过
2. 原有 22 个 CTest 全部尝试：16 通过，6 因 AF_UNIX 受限失败；其中 data_core 的失败断言均涉及单实例 IPC
3. 使用未修改基线库、真实 7zz 的外部运行时测试：96/96 断言通过
4. 额外配置／Job／边界测试：59 通过，另有 2 个用例复现同一“已排除目录仍阻断备份”产品问题

真实运行时测试直接调用共享 ProfileRuntime／BackupService／RestoreService，不替换或绕过 CLI 锁。这验证备份引擎，不代表 CLI 端到端通过。

覆盖 ASCII、中文世界名、中文存档根、中文备份根、中文文件名和含空格路径；实际创建 Full→Smart 归档、验证归档、检测无变化、恢复新增／修改／删除内容，并逐字节核对 clean 和 overwrite 结果。损坏归档拒绝且不修改世界。还验证了配置 manifest apply/export/reload、备份 Job 和仅删除变更的 Smart 恢复。

### 改进后

本机验证结果：

- CLI 与完整 X11/Wayland GUI 编译通过；架构边界、CMake/MSBuild 源文件一致性、核心库独立链接检查通过
- 最终 CLI CTest：19/25 通过，6 个失败仍是上述基线 AF_UNIX 限制；真实归档集成测试包含在这轮完整测试中
- GUI 选择的 11 项测试中 10 项通过，唯一失败是 data_core 中同样的 10 个 IPC 断言；没有新增功能断言失败
- 玩家 NBT 的独立假编解码及真实 7zz gzip 测试通过；新增核心也通过 `-Wall -Wextra -Werror`、ASan/UBSan。LeakSanitizer 因容器 ptrace 限制未完成
- 最终真实引擎集成测试：**153 个断言全部通过**，包括“真实归档＋玩家 NBT＋FTB 当前目录／删除状态”的组合恢复，以及随后显式关闭保留，证明单次规则不污染后续操作
- 原外部比对测试在修复后再次通过：96/96 与 63/63；后者包含两种已排除目录问题的修复验证

新增 `tests/RuntimeArchiveIntegrationTest.cpp` 可通过 CTest 重复执行。当 PATH 中有 7zz/7z 时配置会注册 `minebackup.runtime.archive_integration`，使用隔离临时目录；失败时保留合成夹具以便排查。运行命令：

```bash
cmake --preset linux-x64-cli-only -DBUILD_TESTING=ON
cmake --build build/linux-x64-cli-only --parallel 4
ctest --test-dir build/linux-x64-cli-only --output-on-failure
ctest --test-dir build/linux-x64-cli-only -R 'player_preservation|runtime.archive_integration' --output-on-failure
```

PR 已配置 Linux 桌面与 headless 流水线在面向 develop 的 PR 上运行，具体跨平台 CI 状态以 PR 当前提交的检查为准。新增测试覆盖：

- 13 个玩家字段、未保留字段回退、新／离线玩家、单人嵌入数据、现代布局与 UUID 冲突
- 真实 7zz gzip 编解码、CRC 错误、解析长度／深度／数量／总字节上限与取消
- FTB 同名覆盖、新增、删除、嵌套世界根、目录大小写冲突、链接拒绝
- 恢复准备失败时原世界不变；Partial clean 确认；嵌套世界占用
- 两种列表编码、回调阶段／世界校验及请求关联
- 白名单、区域／维度、Partial 元数据独立性和普通 Smart 保留链
- 已排除的 Linux 特殊文件名、符号链接、不可读目录；取消排除后的 Smart 新增及后续删除

## 5. 已修复的 Linux 实际问题

### 已排除目录中的条目仍让整个备份失败

基线复现：正常世界设置 `exclude: ["cache"]`，在 `cache/` 内放 `namespace:data.txt` 或指向合成外部文件的符号链接。该路径不应进入备份，但备份扫描仍报 `backup.scan.failed`，没有产生归档。

原因：扫描／路径规范化先于排除过滤。修复将明确排除判断前移，排除目录在递归前剪枝；错误诊断带上具体路径与原因。回归同时检查取消排除后的文件重新作为 Smart 新增，以及后续删除，避免修复掩盖增量变化。

## 6. 后续跨平台改进建议

### 优先级 P1：协议与用户可理解的诊断

1. **统一列表编码**。当前时间机器随附 JAR 将 `ftbquests/,ftbteams/` 编码为 `ftbquests%2F%2Cftbteams%2F`；FolderRewind 按原始逗号先拆分，导致两个目录成为一个非法路径。MineBackup 同时兼容标准逐项编码与该调用格式。建议在 MineBackup-Mod 建立参数类型驱动的列表序列化，并给 FolderRewind 增加明确的兼容测试；不要对所有普通字符串粗暴按逗号拆分
2. **发布可追溯的配套 API 源码与版本**。能力查询 API 对这轮功能是必需的，应避免同一 3.3.2 标签下出现不同接口的 JAR。建议发布独立版本，并让时间机器记录最低 API 版本／构建哈希
3. **区分配置档被占用与 IPC 不可用**。当前 AF_UNIX 失败映射为 `invalid_profile` / `profile.lock.failed`，容易让用户误以为配置文件损坏。建议返回独立的 `ipc_unavailable`，包含 OS 错误码、运行目录和容器／sandbox 提示；不要建议删除锁或禁用权限保护
4. **FolderRewind 插件事件补齐关联字段**。当前插件部分热恢复事件以空 context 广播，没有原始 `from/request_id`。需要让 host/plugin/mod 对同一操作的标识含义一致，并覆盖世界名相同、连续重试和过期回调。旧版回调可能没有世界且使用新 UUID，串行化只能缩小误关联窗口；彻底排除延迟回调仍需回显独立的原始操作标识或 nonce。当前不承诺跨崩溃的 exactly-once 请求重放去重

### 优先级 P2：Linux 发行和服务器体验

- 在普通 Ubuntu 24.04 与 Debian 13 主机上做真实单人、专服、systemd 服务账户和 GUI 会话验收；不要仅测 root 或 CI 用户
- 给 `doctor` 增加只读 IPC 环境探测：AF_UNIX 支持、运行目录权限、Unix socket 路径长度、残留端点与身份冲突分类
- 发布包内自带并校验固定 7zz；CLI 包明确不依赖桌面库，GUI 包清晰列出 Wayland/X11／portal 可选能力
- 明确 Linux 合法但跨平台不支持的文件名策略：不自动重命名，应指出具体条目、解释限制并允许排除；对普通中文和空格路径保留回归
- 为 X11、Wayland、无 tray host、无 portal、无 DISPLAY、只读安装目录、非 ASCII 用户目录分别设测试，窗口不能因 tray 降级而不可达
- 玩家数据上限和 Partial 归档大小增长需要运营可见性；独立 Partial 不参与普通链保留，建议后续设计独立配额／清理策略

### 优先级 P3：进一步扩展

- Linux 原生安装／卸载、AppImage、deb、systemd 日志、升级保留 profile 与历史兼容性
- Paper 管理范围外的兄弟维度、多个世界根、模组自定义维度的显式映射；不通过放宽路径边界来支持
- 大型玩家数据的流式解析与受限暂存，及恢复提交阶段的持久事务恢复日志
- 使用实际 1.21.1／目标新版 Minecraft 加载保留后的 NBT；合成 NBT 测试不能证明每个游戏版本都接受所有边界值

## 7. 用户侧验收清单

在可用的真实 Linux 主机、隔离测试世界和匹配的模组构建上：

1. 先普通 Full 备份，再修改方块、背包、末影箱、经验和坐标
2. 时间机器无词条恢复：方块回退；所有玩家的约定字段保持当前值；机器和舱内乘员按时间机器到达逻辑移动
3. 显式 `preserve_player_data=false`：在没有其他整文件保留规则干预时，玩家字段随备份回退
4. FTB 模板：同名进度保持、新建队伍保持、已删除队伍不复活；验证奖励记录与背包的一致性，任务定义保持相同
5. 插入白名单／区域词条：检查真实归档内容与 Partial 类型；之后普通 Smart 仍基于原完整链
6. 测试拒绝／中止／重新进入超时、连续两次操作和重复回调；失败不得报告旅行成功或复制机器
7. 原样运行 CLI、serve 与 GUI，不修改或跳过单实例锁；验证同时请求、取消、进程退出和重启
8. 记录世界／归档哈希、协议请求和结果 UUID、后端与模组精确版本；保存完整错误输出供定位

本次不以“编译成功”代替以上游戏实机验收。未完成的项目应继续作为发布前门槛。
