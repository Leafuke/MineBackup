# MineBackup — 存档时光机 🗂️

[![Latest Release](https://img.shields.io/github/v/release/Leafuke/MineBackup?style=flat-square)](https://github.com/Leafuke/MineBackup/releases)
[![Issues](https://img.shields.io/github/issues/Leafuke/MineBackup?style=flat-square)](https://github.com/Leafuke/MineBackup/issues)

![MineBackup Banner](MineBackup/MineBackup.png)

> **轻松备份 · 快速还原 · 智能压缩**  
> 为你的 Minecraft 世界存档、游戏数据，甚至电脑上的任何文件夹提供一键保护。  


---

**写在前面**：

针对 Windows10 以上的用户，建议优先考虑 [FolderRewind](https://github.com/Leafuke/FolderRewind) + [MineRewind插件](https://github.com/Leafuke/FolderRewind-Plugin-Minecraft) 的组合——[二代时光机](https://github.com/Leafuke/FolderRewind)具有更现代的UI和更强大的功能，并且是将来主要的维护对象。不过考虑到已经有不少用户在使用它，所以我会继续维护 MineBackup，修复一些关键的 bug 和安全问题，但往往会滞后于二代时光机。

---

## ✨ 为什么选择 MineBackup？
- 🎯 **原生分发** — 提供签名 Windows 单 EXE、Linux deb/AppImage 和 arm64 macOS DMG。
- 🖥 **简洁直观的 GUI** — 基于 ImGui，功能布局清晰、响应迅速。
- 💾 **安全备份** — 一键备份 Minecraft 存档，避免数据丢失。
- 🔄 **快速还原** — 支持从 `.7z` 文件或本地目录恢复任意版本。
- 📦 **高压缩率** — 内置 7-Zip 核心，节省存储空间。
- 🧠 **智能模式** — 类 Git 增量备份，节省时间与空间。
- 📁 **自定义路径** — 将备份保存到任意磁盘或外接设备。
- 🌏 **多语言支持** — 已支持中/英双语，欢迎贡献更多翻译。
- 💻 **多平台支持** — Windows x64、Linux x86_64 与 macOS 15+ arm64 共用同一备份数据契约。

💡 **不仅仅是 Minecraft**：你可以用它来备份任何文件夹，完全不局限于游戏存档。

---

## 🚀 快速开始

### 1️⃣ 下载 & 运行
1. 前往 [最新发布页](https://github.com/Leafuke/MineBackup/releases)。
2. 下载 `MineBackup-windows-x64.exe`、Ubuntu `.deb`、Linux AppImage 或
   `MineBackup-1.16.0-macos-arm64.dmg`。
3. 使用 `SHA256SUMS` 核对文件后再正常安装或运行。

各系统版本与 Linux 桌面能力降级规则见[平台支持矩阵](docs/platform-support.md)。
macOS 包尚未公证；若系统拦截，请使用“隐私与安全性 → 仍要打开”，不要关闭
Gatekeeper，也不要执行 `xattr` 绕过安全检查。

### 2️⃣ 基础操作
| 功能       | 操作方式 |
|------------|----------|
| 备份世界   | 在列表中选择世界 → 点击 **备份** |
| 还原世界   | 选择世界 → 点击 **还原**（可从 `.7z` 或本地目录恢复） |
| 修改备份路径 | 打开 **设置** → 选择备份存放位置 |
| 切换语言   | 设置 → 语言 |

### 3️⃣ 高级技巧
- 使用 **热键 Alt+Ctrl+S** 即可在游戏运行时触发“热备份”。
- 启用 **退出检测（DetectOnExit）**：自动在退出 Minecraft 后进行备份。
- 通过 **KnotLink** 与其他程序或 Mod 联动，实现备份前自动保存世界。示例 Mod 可见 [这里](https://modrinth.com/mod/minebackup)。

---

## 🛠 功能亮点

### 📌 热键备份
- 按 **Alt + Ctrl + S**：自动检测当前运行的世界 → 广播保存请求 → 执行热备份。

### 📌 热键还原
- **Alt + Ctrl + Z**：将你当前运行的世界还原到上一个备份版本！需要配合MineBackup-Mod联动模组。

### 📌 KnotLink 消息交互
MineBackup 现已与 FolderRewind 使用完全相同的严格 KnotLink v2 参数化
契约：载荷固定为 `key=value;key2=value2`，值使用 RFC 3986
percent-encoding；旧式位置参数和自由文本命令会被直接拒绝。联动模组最低
版本为 **3.1.0**。

命令、关联元数据、生命周期事件、当前世界参数和完整示例见
[MineBackup KnotLink v2 协议说明](docs/knotlink-v2.md)。

---

## ⚙️ 安装与编译

**运行环境**：
- Windows + MSVC、Ubuntu 24.04+ x86_64（或同等的 glibc 2.39+ 发行版），
  或 Apple Silicon macOS 15+
- CMake 3.22+ 与 C++20 编译器
- 对应平台验证文档中列出的开发依赖

**编译步骤**：
```bash
# 克隆仓库
git clone https://github.com/Leafuke/MineBackup.git
cd MineBackup

cmake --preset <windows-msvc-x64|linux-x64|macos-arm64>
cmake --build --preset windows-msvc-x64-release  # Windows
# Linux/macOS: cmake --build build/<preset> --parallel
```

配置不再要求与 EXE 同目录；默认数据目录、便携模式与 1.15 迁移说明见
[数据与迁移文档](docs/data-and-migration.md)。Windows Service Mode 已弃用，
不能再安装或启动。rclone 不随包分发，只会在用户确认后下载固定版本并验证哈希。
日志默认以 Info 级别实时轮转写入配置档的 `logs/minebackup.log`；可在设置中
切换 Off/Info/Debug，并从日志面板导出已知路径脱敏的诊断副本。位置、保留、
隐私边界和排障步骤见[日志与诊断说明](docs/logging-and-diagnostics.md)。

---

## 代码签名策略

<table>
  <tr>
    <td>
      <img alt="SignPath" src="https://signpath.org/assets/favicon-50x50.png" />
    </td>
    <td>
    Free code signing on Windows provided by <a href="https://signpath.io">SignPath.io</a>, certficate by <a href="https://signpath.org/">SignPath Foundation</a>
    </td>
  </tr> 
</table>

- 由 [SignPath.io](https://about.signpath.io/) 提供免费代码签名，由 [SignPath Foundation](https://signpath.org/) 提供证书。
- 提交者和审阅者：[团队成员](https://github.com/Leafuke/MineBackup/graphs/contributors)
- 审批人：[Leafuke](https://github.com/Leafuke)

---

## 🤝 贡献与支持

* **报告问题 / 提交建议**：[GitHub Issues](https://github.com/Leafuke/MineBackup/issues)
* **多语言支持**：翻译 [`i18n.h`](MineBackup/src/infra/i18n.h)，让更多玩家用上自己的语言。
* **文档改进**：访问 [官方文档](https://folderrewind.top) 提交改进建议。

---

## 📄 项目依赖

* [**7-Zip**](https://github.com/ip7z/7zip) — 压缩核心
* [**7-Zip-zstd**](https://github.com/mcmilk/7-Zip-zstd) - Zstd 压缩支持
* [**ImGui**](https://github.com/ocornut/imgui) — GUI 框架
* [**stb**](https://github.com/nothings/stb) — 图片加载
* [**KnotLink**](https://github.com/KnotLink-Protocol/KnotLink) — 程序间消息通信框架
* [**Font-Awesome**](https://github.com/FortAwesome/Font-Awesome) - Icons

---

**MineBackup** — 给你的 Minecraft 世界一份安心的保险。
💬 如果你喜欢它，请点一个 ⭐ 支持！
