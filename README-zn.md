# MineBackup — 存档时光机 🗂️💾

[![中文说明](https://img.shields.io/badge/README-中文-blue)](README-zn.md)
[![Latest Release](https://img.shields.io/github/v/release/Leafuke/MineBackup?style=flat-square)](https://github.com/Leafuke/MineBackup/releases)
[![Issues](https://img.shields.io/github/issues/Leafuke/MineBackup?style=flat-square)](https://github.com/Leafuke/MineBackup/issues)

![MineBackup Banner](MineBackup/MineBackup.png)

> **轻松备份 · 快速还原 · 智能压缩**  
> 为你的 Minecraft 世界存档、游戏数据，甚至电脑上的任何文件夹提供一键保护。  

---

## ✨ 为什么选择 MineBackup？
- 🎯 **即点即用** — 单文件可执行，下载后双击运行，无需安装。
- 🖥 **简洁直观的 GUI** — 基于 ImGui，功能布局清晰、响应迅速（v1.5.0+）。
- 💾 **安全备份** — 一键备份 Minecraft 存档，避免数据丢失。
- 🔄 **快速还原** — 支持从 `.7z` 文件或本地目录恢复任意版本。
- 📦 **高压缩率** — 内置 7-Zip 核心，节省存储空间。
- 🧠 **智能模式** — 类 Git 增量备份，节省时间与空间。
- 📁 **自定义路径** — 将备份保存到任意磁盘或外接设备。
- 🌏 **多语言支持** — 已支持中/英双语，欢迎贡献更多翻译。

💡 **不仅仅是 Minecraft**：你可以用它来备份任何文件夹，完全不局限于游戏存档。

---

## 🚀 快速开始

### 1️⃣ 下载 & 运行
1. 前往 [最新发布页](https://github.com/Leafuke/MineBackup/releases)。
2. 下载适用于 Windows 的单文件版本。
3. 双击运行 — **就是这么简单**。

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
- 通过 **KnotLink** 与其他程序或 Mod 联动，实现备份前自动保存世界。

---

## 🛠 功能亮点

### 📌 热键备份
- 按 **Alt + Ctrl + S**：自动检测当前运行的世界 → 广播保存请求 → 执行热备份。

### 📌 自动退出备份
- 开启 `DetectOnExit` 后，MineBackup 会每 10 秒检测一次存档文件占用情况。  
- 当检测到世界退出（`level.dat` 释放）时，自动进行热备份。

### 📌 KnotLink 消息交互
通过 KnotLink 协议，MineBackup 可以：
- **发送** 保存请求：`event=knotlink_save_request`
- **接收** Mod / 服务端返回的 `save_done` 事件后执行备份  
- **支持的指令**：
```

BACKUP \<config\_idx> \<world\_idx> \[comment]
RESTORE \<config\_idx>
LIST\_WORLDS \<config\_idx>
LIST\_CONFIGS
SET\_CONFIG \<config\_idx> <key> <value>

````
*key*: `backup_mode` / `hot_backup`  
*value*: `1/2/3` 或 `0/1`

> 🔍 **开发者请看**：[KnotLink 协议详情](#-开发者与进阶玩家)

---

## ⚙️ 安装与编译

**运行环境**：
- Windows 系统
- C++17 编译器（Visual Studio 推荐）
- 链接 ImGui 库
- 附带 7-Zip 

**编译步骤**：
```bash
# 克隆仓库
git clone https://github.com/Leafuke/MineBackup.git
cd MineBackup

# 使用 Visual Studio 打开并编译
# 确保包含 imgui 源码
````

---

## 🤝 贡献与支持

* **报告问题 / 提交建议**：[GitHub Issues](https://github.com/Leafuke/MineBackup/issues)
* **多语言支持**：翻译 [`i18n.h`](MineBackup/i18n.h)，让更多玩家用上自己的语言。
* **文档改进**：帮助完善使用文档和示例。

---

## 📚 开发者与进阶玩家

### KnotLink 接口快速参考

| 指令            | 说明          |
| ------------- | ----------- |
| BACKUP        | 立即备份指定配置与世界 |
| RESTORE       | 还原指定配置      |
| LIST\_WORLDS  | 列出某配置下的所有世界 |
| LIST\_CONFIGS | 列出所有配置      |
| SET\_CONFIG   | 修改配置参数      |

**APPID**: `0x00000020`
**socket ID**: `0x00000010`
**signal ID**: `0x00000020`

---

## 📄 项目依赖

* [**7-Zip**](https://github.com/ip7z/7zip) — 压缩核心（7z.exe）
* [**ImGui**](https://github.com/ocornut/imgui) — GUI 框架
* [**stb**](https://github.com/nothings/stb) — 图片加载
* **KnotLink** — 程序间消息通信框架

---

## 📸 截图示例



---

**MineBackup** — 给你的 Minecraft 世界一份安心的保险。
💬 如果你喜欢它，请点一个 ⭐ 支持！
