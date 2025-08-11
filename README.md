# MineBackup — The Ultimate Backup Companion for Your Minecraft Worlds 🗂️💾

[![中文说明](https://img.shields.io/badge/README-中文-blue)](README-zn.md)
[![Latest Release](https://img.shields.io/github/v/release/Leafuke/MineBackup?style=flat-square)](https://github.com/Leafuke/MineBackup/releases)
[![Issues](https://img.shields.io/github/issues/Leafuke/MineBackup?style=flat-square)](https://github.com/Leafuke/MineBackup/issues)

![MineBackup Banner](MineBackup/MineBackup.png)

> **Back up with confidence · Restore in seconds · Compress intelligently**  
> Keep your Minecraft worlds safe — and yes, you can back up any folder on your PC, too.

---

## ✨ Why MineBackup?
- 🎯 **Plug-and-Play** — A single executable. Download, double-click, done.
- 🖥 **Clean, Fast GUI** — Powered by ImGui. Simple layout, snappy response (v1.5.0+).
- 💾 **Secure Backups** — One click to safeguard your Minecraft saves.
- 🔄 **Quick Restores** — Roll back to any previous state from a `.7z` file or local backup.
- 📦 **High Compression** — Built-in 7-Zip core saves disk space.
- 🧠 **Smart Mode** — Git-like incremental backups to save time and storage.
- 📁 **Custom Paths** — Store backups wherever you want.
- 🌏 **Multi-language** — Currently supports English and Chinese — more are welcome!

💡 **Pro tip:** It works on any folder, not just Minecraft worlds.

---

## 🚀 Getting Started

### 1️⃣ Download & Run
1. Go to the [latest release](https://github.com/Leafuke/MineBackup/releases).
2. Download the single Windows executable.
3. Double-click to run — **no installation required**.

### 2️⃣ Basic Actions
| Feature      | How to Use |
|--------------|------------|
| Back up a world | Select a world → click **Backup** |
| Restore a world | Select a world → click **Restore** (from `.7z` or local directory) |
| Change backup location | Open **Settings** → choose your path |
| Switch language | Settings → Language |

### 3️⃣ Power Features
- **Hotkey Backup** — Press **Alt+Ctrl+S** in-game to trigger a live backup.
- **Exit Detection** — Enable *DetectOnExit* to back up automatically when Minecraft closes.
- **KnotLink Integration** — Let MineBackup talk to mods or other tools to trigger “save before backup.”

---

## 🛠 Feature Highlights

### 🔥 Hotkey Backups
- Hit **Alt + Ctrl + S** while playing:  
  Detects the currently active world → broadcasts a save request → runs a hot backup in the background.

### 🕒 Auto Exit Backups
- With *DetectOnExit* enabled, MineBackup checks every 10 seconds for changes to `level.dat`.
- When it sees the file go from “in use” to “released,” it automatically performs a hot backup.

### 📡 KnotLink Messaging
MineBackup can send and receive simple text events to coordinate with other apps or mods:
- **Sends:**  
  `event=knotlink_save_request;config=0;world=MyWorld`
- **Receives:**  
  `knotlink_save_done` → proceed with backup.
- **Supported Commands:**
```

BACKUP \<config\_idx> \<world\_idx> \[comment]
RESTORE \<config\_idx>
LIST\_WORLDS \<config\_idx>
LIST\_CONFIGS
SET\_CONFIG \<config\_idx> <key> <value>

````
*key*: `backup_mode` / `hot_backup`  
*value*: `1/2/3` or `0/1`

> 🔍 See [Developer & Advanced User Guide](#-developer--advanced-user-guide) for full protocol details.

---

## ⚙️ Installation & Build

**Requirements:**
- Windows
- C++17 compiler (Visual Studio recommended)
- ImGui library linked
- 7-Zip executable (already bundled)

**Build:**
```bash
# Clone the repo
git clone https://github.com/Leafuke/MineBackup.git
cd MineBackup

# Open in Visual Studio and build
# Make sure the ImGui source is included
````

---

## 🤝 Contributing & Support

* **Report bugs / request features:** [GitHub Issues](https://github.com/Leafuke/MineBackup/issues)
* **Help translate:** Edit [`i18n.h`](MineBackup/i18n.h) and submit a pull request.
* **Improve docs:** PRs welcome — examples, screenshots, and tips appreciated.

---

## 📚 Developer & Advanced User Guide

### KnotLink Quick Reference

| Command       | Description                              |
| ------------- | ---------------------------------------- |
| BACKUP        | Immediately back up a given config/world |
| RESTORE       | Restore a given config                   |
| LIST\_WORLDS  | List all worlds under a config           |
| LIST\_CONFIGS | List all configs                         |
| SET\_CONFIG   | Change a config parameter                |

**APPID:** `0x00000020`
**socket ID:** `0x00000010`
**signal ID:** `0x00000020`

---

## 📄 Project References

* [**7-Zip**](https://github.com/ip7z/7zip) — Compression core (7z.exe)
* [**ImGui**](https://github.com/ocornut/imgui) — GUI framework
* [**stb**](https://github.com/nothings/stb) — Image loading
* **KnotLink** — Lightweight inter-process messaging

---

## 📸 Screenshots


---

**MineBackup** — Peace of mind for your Minecraft worlds.
⭐ If you find it useful, please give it a star!
