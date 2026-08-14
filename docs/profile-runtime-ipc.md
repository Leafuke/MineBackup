# Profile Runtime IPC v2

MineBackup 对每个规范 profile 身份持有一个单实例锁。Windows 使用当前用户 ACL 的 named pipe 与 `Local` mutex；Linux/macOS 将 lock file 保留在 profile runtime 目录，并在权限 `0700` 的 `/tmp/minebackup-<uid>` 目录中使用权限 `0600` 的短 Unix socket。socket 文件名只包含规范 profile 身份的固定长度 hash，因此不受 profile 路径长度影响。协议不监听 TCP/UDP，也不允许跨用户控制。

桌面激活保留只写的 v1 `Activate`/`SelectConfig` 消息。服务器控制面使用长度前缀 JSON v2，并在同一连接返回最终响应：

```json
{
  "version": 2,
  "message": "request",
  "requestId": "client-generated-id",
  "type": "probe|execute|status|cancel|stop",
  "arguments": ["--json", "backup", "--config", "..."],
  "operationId": ""
}
```

```json
{
  "version": 2,
  "message": "response",
  "requestId": "client-generated-id",
  "accepted": true,
  "role": "serve",
  "capabilities": ["execute", "status", "cancel", "stop"],
  "operationId": "server-operation-id",
  "exitCode": 0,
  "payload": "{...final CLI envelope...}",
  "error": ""
}
```

`requestId` 关联一次 IPC 往返，`operationId` 标识服务端可取消操作。参数保持 Unicode 数组语义，不经 shell。客户端必须验证响应 `requestId`、runtime role 与 capability；连接到 GUI、收到未知版本、超长/畸形消息或超时都不得降级为不受认证的旁路执行。

v2 payload 上限为 8 MiB；旧桌面消息继续限制为 64 KiB。服务端接收连接后保留回复通道，因此可以在执行工作线程结束时返回与直接 CLI 相同的最终结果和退出码；取消与停机请求使用独立连接，不依赖正在运行的命令连接。服务端编码或写入响应失败时也会关闭保留的连接；可编码的超限响应会降级为简短的协议错误，客户端不会无限等待。

`status` 的 envelope 数据包含正在执行的 IPC operationId/命令/取消状态、KnotLink 活动操作数量、网络与 listener 状态和运行时间。客户端 Ctrl+C 发送对应 operationId 的 `cancel`；连接意外断开不会扩大权限或切换到旁路执行。`stop`/SIGTERM 先停止接单，再取消并收尾 IPC 与 KnotLink 工作，最后释放 profile 锁。
