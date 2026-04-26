[中文](README_zh.md) | **English**

# MineBackup — The Ultimate Backup Companion for Your Minecraft Worlds 🗂️

[![中文说明](https://img.shields.io/badge/README-中文-blue)](README_zh.md)
[![Latest Release](https://img.shields.io/github/v/release/Leafuke/MineBackup?style=flat-square)](https://github.com/Leafuke/MineBackup/releases)
[![Issues](https://img.shields.io/github/issues/Leafuke/MineBackup?style=flat-square)](https://github.com/Leafuke/MineBackup/issues)

![MineBackup Banner](MineBackup/MineBackup.png)

> **Back up with confidence · Restore in seconds · Compress intelligently**  
> Keep your Minecraft worlds safe — and yes, you can back up any folder on your PC, too.

---

**A Note Before You Begin**:

1.14.0 contains many **breaking changes**. After upgrading, please be sure to click "Auto-Verify Core Functions" in the "Tools" menu to validate that the current version works properly on your machine. Also, it's highly recommended to perform several backup and restore tests to ensure everything is working correctly before using it for real.

For Windows 10 and above users, it is recommended to prioritize the combination of [FolderRewind](https://github.com/Leafuke/FolderRewind) + [MineRewind Plugin](https://github.com/Leafuke/FolderRewind-Plugin-Minecraft) — the [Second Generation Time Machine](https://github.com/Leafuke/FolderRewind) features a more modern UI and more powerful functionalities, and it will be the main focus of future maintenance. However, considering that many users are already using it, I will continue to maintain MineBackup, fixing critical bugs and security issues, but updates will often lag behind the second-generation Time Machine.

---

## ✨ Why MineBackup?
- 🎯 **Plug-and-Play** — A single executable. Download, double-click, done.
- 🖥 **Clean, Fast GUI** — Powered by ImGui. Simple layout, snappy response.
- 💾 **Secure Backups** — One click to safeguard your Minecraft saves.
- 🔄 **Quick Restores** — Roll back to any previous state from a `.7z` file or local backup.
- 📦 **High Compression** — Built-in 7-Zip core saves disk space.
- 🧠 **Smart Mode** — Git-like incremental backups to save time and storage.
- 📁 **Custom Paths** — Store backups wherever you want.
- 🌏 **Multi-language** — Currently supports English and Chinese — more are welcome!
- 💻 **Multi-platform** — Currently supports Windows, Linux and MacOS.

💡 **Pro tip:** It works on any folder, not just Minecraft worlds.

---

## 🚀 Getting Started

### 1️⃣ Download & Run
1. Go to the [latest release](https://github.com/Leafuke/MineBackup/releases).
2. Download the single Windows executable.
3. Double-click to run — **no installation required**.

### 2️⃣ Basic Actions - Basic
| Feature      | How to Use |
|--------------|------------|
| Back up a world | Select a world → click **Backup** |
| Restore a world | Select a world → click **Restore** (from `.7z` or local directory) |
| Change backup location | Open **Settings** → choose your path |
| Switch language | Settings → Language |

### 3️⃣ Power Features
- **Hotkey Backup** — Press **Alt+Ctrl+S** in-game to trigger a live backup.
- **Exit Detection** — Enable *DetectOnExit* to back up automatically when Minecraft closes.
- **KnotLink Integration** — Let MineBackup talk to mods or other tools to trigger “save before backup.” An example mod is [here](https://modrinth.com/mod/minebackup).

---

## 🛠 Feature Highlights

### 🔥 Hotkey Backups
- Hit **Alt + Ctrl + S** while playing:  
  Detects the currently active world → broadcasts a save request → runs a hot backup in the background.

### 📌 Hotkey Restore
- **Alt + Ctrl + Z**: Instantly restore your currently active world to the last backup! Requires integration with the MineBackup-Mod.

### 📡 KnotLink Messaging
MineBackup can send and receive simple text events to coordinate with other apps or mods. View [MineBackup](https://modrinth.com/mod/minebackup) as an example mod for detail.

- **Supported Commands:**


See [Developer & Advanced User Guide](https://folderrewind.top/docs/plugins/knotlink) for full protocol details.

---

## ⚙️ Installation & Build

**Requirements:**
- Windows/Linux/MacOS
- C++20 compiler
- ImGui library linked
- 7-Zip executable

**Build:**
```bash
# Clone the repo
git clone https://github.com/Leafuke/MineBackup.git
cd MineBackup

````


---

## 🤝 Contributing & Support

* **Report bugs / request features:** [GitHub Issues](https://github.com/Leafuke/MineBackup/issues)
* **Help translate:** Edit [`i18n.h`](MineBackup/i18n.cpp) and submit a pull request.
* **Improve docs:** Visit the [official documentation](https://folderrewind.top) to submit suggestions for improvement. This is the official website for the second-generation FolderRewind, and it will also add documentation support for the first-generation MineBackup in the future.

---

## 📄 Project References

* [**7-Zip**](https://github.com/ip7z/7zip) — Compression core
* [**7-Zip-zstd**](https://github.com/mcmilk/7-Zip-zstd) - Zstd support
* [**ImGui**](https://github.com/ocornut/imgui) — GUI framework
* [**stb**](https://github.com/nothings/stb) — Image loading
* [**KnotLink**](https://github.com/hxh230802/KnotLink) — Lightweight inter-process messaging
* [**json**](https://github.com/nlohmann/json) — Metadata read & write
* [**Font-Awesome**](https://github.com/FortAwesome/Font-Awesome) - Icons

---

## 📜 Note

The next generation of MineBackup has been realesed as [FolderRewind](https://github.com/Leafuke/FolderRewind). It features better versatility and a modern UI. For Windows users, FR is now a competitive alternative to MineBackup.

---

**MineBackup** — Peace of mind for your Minecraft worlds.
⭐ If you find it useful, please give it a star!
