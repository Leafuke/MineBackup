# MineBackup 1.16 platform support

| Platform | Supported releases | Release asset | Desktop integration |
|---|---|---|---|
| Windows x64 | Windows 10 22H2, Windows 11 | `MineBackup-windows-x64.exe` | Native dialogs, tray, notifications, global hotkeys and current-user autostart |
| Ubuntu x86_64 | 24.04 and later | `minebackup_1.16.0_amd64.deb` or AppImage | X11/Wayland automatic selection; portals and tray degrade by session capability |
| Debian x86_64 | 13 and later | `MineBackup-1.16.0-x86_64.AppImage` | Same capability-based Linux behavior |
| macOS arm64 | macOS 15 and later | `MineBackup-1.16.0-macos-arm64.dmg` | Native dialogs, menu bar, notifications, hotkeys and login item |

Backup, restore, history, FolderRewind metadata, rclone cloud workflows and
automatic tasks share the same data contracts on all three platforms. Linux
desktop features are environment-dependent: the UI reports `Available`,
`Unavailable`, `PermissionRequired` or `Failed` and explains the active reason.
Missing portals, a missing StatusNotifier host or a rejected shortcut must not
disable backup and restore or make the main window unreachable.

Linux release binaries use the Ubuntu 24.04 toolchain baseline and require
glibc 2.39 or later. Ubuntu 22.04 and Debian 12 are not supported.

## KnotLink v2 platform behavior

KnotLink interop is v2-only. Windows x64 requires a locally installed
KnotLinkService 3.0.0 or newer. MineBackup discovers it through App Paths and
the 32/64-bit uninstall registry views, falls back to the executable file
version, and requires loopback ports 6370 and 6378 before connecting. An
unknown or older version is treated as incompatible. A compatible stopped
service can be started automatically (enabled by default) or from Settings;
startup times out after 10 seconds without blocking the main UI.

Linux and macOS no longer contain an embedded KnotLink server. They keep the
same C++ SDK client and may connect when a future upstream server is listening
on the fixed loopback ports, but MineBackup does not discover, install, or
start a server there. No platform downloads or runs a KnotLink installer;
Settings opens the [official releases page](https://github.com/KnotLink-Protocol/KnotLink/releases).

The Minecraft companion mod must be version 3.1.0 or newer. Older or malformed
versions do not participate in hot workflows: hot backup falls back to an
ordinary live snapshot, while hot restore is rejected.

## macOS distribution status

The 1.16 DMG is arm64-only and ad-hoc signed, but not Apple-notarized. After
copying MineBackup to Applications, macOS may require **System Settings →
Privacy & Security → Open Anyway** on first launch. Do not disable Gatekeeper
and do not remove quarantine metadata with `xattr`.
