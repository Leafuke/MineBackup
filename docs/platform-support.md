# MineBackup 1.16 platform support

| Platform | Supported releases | Release asset | Desktop integration |
|---|---|---|---|
| Windows x64 | Windows 10 22H2, Windows 11 | `MineBackup-windows-x64.exe` | Native dialogs, tray, notifications, global hotkeys and current-user autostart |
| Ubuntu x86_64 | 22.04, 24.04, 26.04 | `minebackup_1.16.0_amd64.deb` or AppImage | X11/Wayland automatic selection; portals and tray degrade by session capability |
| Debian x86_64 | 12, 13 | `MineBackup-1.16.0-x86_64.AppImage` | Same capability-based Linux behavior |
| macOS arm64 | macOS 15 and later | `MineBackup-1.16.0-macos-arm64.dmg` | Native dialogs, menu bar, notifications, hotkeys and login item |

Backup, restore, history, FolderRewind metadata, rclone cloud workflows and
automatic tasks share the same data contracts on all three platforms. Linux
desktop features are environment-dependent: the UI reports `Available`,
`Unavailable`, `PermissionRequired` or `Failed` and explains the active reason.
Missing portals, a missing StatusNotifier host or a rejected shortcut must not
disable backup and restore or make the main window unreachable.

KnotLink and WorldEdit integration receive build and basic smoke coverage but
are experimental and are not hard release gates for 1.16.

## macOS distribution status

The 1.16 DMG is arm64-only and ad-hoc signed, but not Apple-notarized. After
copying MineBackup to Applications, macOS may require **System Settings →
Privacy & Security → Open Anyway** on first launch. Do not disable Gatekeeper
and do not remove quarantine metadata with `xattr`.
