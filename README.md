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
- 🎯 **Native distribution** — A signed Windows executable, Linux deb/AppImage and an arm64 macOS DMG.
- 🖥 **Clean, Fast GUI** — Powered by ImGui. Simple layout, snappy response.
- 💾 **Secure Backups** — One click to safeguard your Minecraft saves.
- 🔄 **Quick Restores** — Roll back to any previous state from a `.7z` file or local backup.
- 📦 **High Compression** — Built-in 7-Zip core saves disk space.
- 🧠 **Smart Mode** — Git-like incremental backups to save time and storage.
- 📁 **Custom Paths** — Store backups wherever you want.
- 🌏 **Multi-language** — Currently supports English and Chinese — more are welcome!
- 💻 **Multi-platform** — Windows x64, Linux x86_64 and macOS 15+ arm64 share the same backup data contracts.

💡 **Pro tip:** It works on any folder, not just Minecraft worlds.

---

## 🚀 Getting Started

### 1️⃣ Download & Run
1. Go to the [latest release](https://github.com/Leafuke/MineBackup/releases).
2. Download `MineBackup-windows-x64.exe`, the Ubuntu `.deb`, the Linux
   AppImage, or `MineBackup-1.16.0-macos-arm64.dmg`.
3. Verify the asset against `SHA256SUMS`, then install or run it normally.

See the [platform support matrix](docs/platform-support.md) for supported OS
versions and honest Linux desktop degradation. The macOS build is not notarized;
use **Privacy & Security → Open Anyway** if prompted, without disabling
Gatekeeper or running `xattr` commands.

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
MineBackup uses the same strict KnotLink v2 parameterized contract as
FolderRewind. Payloads are `key=value;key2=value2`, values use RFC 3986
percent-encoding, and old positional/free-text commands are rejected. The
companion mod must be version **3.1.0 or newer**.

Windows requires KnotLinkService **3.0.0 or newer**. MineBackup detects the
installed version, can start a compatible stopped service, and blocks unknown
or older versions. Linux and macOS retain client support but do not bundle or
manage a KnotLink server until upstream publishes one.

See the [MineBackup KnotLink v2 reference](docs/knotlink-v2.md) for commands,
correlation metadata, lifecycle events, current-world requests, and examples.

---

## ⚙️ Installation & Build

**Requirements:**
- Windows with MSVC, Ubuntu 24.04+ x86_64 (or an equivalent glibc 2.39+
  distribution), or Apple Silicon macOS 15+
- CMake 3.22+ and a C++20 compiler
- Platform packages listed in the verification guide for that platform

**Build:**
```bash
git clone https://github.com/Leafuke/MineBackup.git
cd MineBackup
cmake --preset <windows-msvc-x64|linux-x64|macos-arm64>
cmake --build --preset windows-msvc-x64-release  # Windows
# On Linux/macOS: cmake --build build/<preset> --parallel
```

MineBackup stores data in a platform profile, not beside the executable. Read
[data locations, portable mode and 1.15 migration](docs/data-and-migration.md)
before moving an existing installation. Windows Service Mode is deprecated and
cannot be newly installed or started. rclone is optional, not bundled, and is
downloaded only after explicit confirmation with version/hash validation.
Logging defaults to an Info-level rotating `logs/minebackup.log`. Settings can
switch Off/Info/Debug at runtime, and the Log panel can export a diagnostic copy
with known paths redacted. See [logging and diagnostics](docs/logging-and-diagnostics.md)
for locations, retention, privacy boundaries and troubleshooting.

---

## Sponsor

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

- Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).
- Committers and reviewers: [Members team](https://github.com/Leafuke/MineBackup/graphs/contributors)
- Approvers: [Leafuke](https://github.com/Leafuke)

---

## 🤝 Contributing & Support

* **Report bugs / request features:** [GitHub Issues](https://github.com/Leafuke/MineBackup/issues)
* **Help translate:** Edit [`i18n.h`](MineBackup/src/infra/i18n.h) and submit a pull request.
* **Improve docs:** Visit the [official documentation](https://folderrewind.top) to submit suggestions for improvement. This is the official website for the second-generation FolderRewind, and it will also add documentation support for the first-generation MineBackup in the future.

---

## 📄 Project References

* [**7-Zip**](https://github.com/ip7z/7zip) — Compression core
* [**7-Zip-zstd**](https://github.com/mcmilk/7-Zip-zstd) - Zstd support
* [**ImGui**](https://github.com/ocornut/imgui) — GUI framework
* [**stb**](https://github.com/nothings/stb) — Image loading
* [**KnotLink**](https://github.com/KnotLink-Protocol/KnotLink) — Lightweight inter-process messaging
* [**json**](https://github.com/nlohmann/json) — Metadata read & write
* [**Font-Awesome**](https://github.com/FortAwesome/Font-Awesome) - Icons

---

## 📜 Note

The next generation of MineBackup has been realesed as [FolderRewind](https://github.com/Leafuke/FolderRewind). It features better versatility and a modern UI. For Windows users, FR is now a competitive alternative to MineBackup.

---

**MineBackup** — Peace of mind for your Minecraft worlds.
⭐ If you find it useful, please give it a star!
