# ADR 0007：服务器清单、一次性作业与配置档运行时

- 状态：Accepted
- 日期：2026-08-09

## 背景

ADR 0006 建立的 headless CLI 仍要求 GUI 创建普通配置，并以 SpecialConfig 同时表达工作流和内部调度。这使服务器首次部署、自动化审计、故障恢复和常驻 KnotLink 协调都缺少稳定边界。

## 决策

1. CLI 是独立服务器产品：空 Profile 可通过版本化 Server Manifest 完成初始化、校验、差异预览、事务 apply 和 export；`config.ini` 与 `jobs.json` 仍是共享运行时权威存储。
2. Config 表达备份策略；Job 只表达一次性工作流；Schedule 由 systemd timer 或 Windows Task Scheduler 持有。Job 由顺序 Stage 组成，同一 Stage 的 Backup/Process Step 并行执行。
3. Process Step 只保存 executable 与 arguments，不提供隐式 shell 或 Script Step。需要 shell 的用户必须显式调用 `/bin/sh -c` 或 `cmd.exe /C`。
4. `run-special`、SpecialConfig 和内置 interval/scheduled 执行路径退出产品；旧 `[SpCfg]` 与 `special-tasks.json` 保留在磁盘但不迁移、不执行。
5. Profile Runtime 是备份、校验、还原、Job 和 KnotLink 的唯一协调边界。第一阶段由一次性 CLI 临时持有；第二阶段可由本地 `serve` 代理长期持有，并通过同用户双向 IPC 接受原命令的透明转发。
6. GUI 与 `serve` 对同一 Profile 严格互斥；Schedule 不因 `serve` 存在而改变命令行，普通命令自行选择直接执行或转发。
7. 首阶段只支持本地冷还原；第二阶段在共享 RestoreService 上增加 KnotLink 热还原。云端补链、内置调度和容器交付不属于这两个里程碑。

## 结果

ADR 0006 的 runtime 分层、非交互契约、ConfigId、JSON envelope 和 CLI 不承担 v1.15 迁移的决定继续有效；其 SpecialConfig、`run-special`、特殊任务 JSON 和“Restore CLI/正式 CLI 打包不属于里程碑”的决定由本 ADR 取代。外部调度成为唯一时间触发来源，运行时存储和声明式交换格式保持分离。
