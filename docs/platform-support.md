# MineBackup 1.16 platform support

| Platform | Supported releases | Release asset | Desktop integration |
|---|---|---|---|
| Windows x64 | Windows 10 22H2, Windows 11 | `MineBackup-windows-x64.exe` | Native dialogs, tray, notifications and global hotkeys |
| Ubuntu x86_64 | 24.04 and later | `minebackup_1.16.0_amd64.deb` or AppImage | X11/Wayland automatic selection; portals and tray degrade by session capability |
| Debian x86_64 | 13 and later | `MineBackup-1.16.0-x86_64.AppImage` | Same capability-based Linux behavior |
| macOS arm64 | macOS 15 and later | `MineBackup-1.16.0-macos-arm64.dmg` | Native dialogs, menu bar, notifications and hotkeys |

Backup, restore, history, FolderRewind metadata, rclone cloud workflows and
automatic tasks share the same data contracts on all three platforms. Linux
desktop features are environment-dependent: the UI reports `Available`,
`Unavailable`, `PermissionRequired` or `Failed` and explains the active reason.
Missing portals, a missing StatusNotifier host or a rejected shortcut must not
disable backup and restore or make the main window unreachable.

Desktop special-task execution and its Windows/macOS login startup integration
are disabled. The source tree provides a desktop-free CLI replacement, but it
is not included in the release assets yet; see [Headless CLI](headless-cli.md).

## Logging verification

All platforms use the same structured record and rotation contract. Validate
the platform log root, runtime Off/Info/Debug switching, five-file maximum,
unwritable-directory degradation, forced-exit warning and clean marker removal
as described in [Logging and diagnostics](logging-and-diagnostics.md). Release
dependency inspection must not report a spdlog or fmt runtime library: both are
statically linked from the pinned source.

Linux release binaries use the Ubuntu 24.04 toolchain baseline and require
glibc 2.39 or later. Ubuntu 22.04 and Debian 12 are not supported.

## KnotLink v2 platform behavior

KnotLink interop is v2-only. Windows x64 expects a locally installed
KnotLinkService 3.2.0.0 or newer. MineBackup discovers it through App Paths and
the 32/64-bit uninstall registry views, falls back to the executable file
version, and requires loopback ports 6370 and 6378 before connecting. An
installed unknown or older version is treated as incompatible and produces a
dismissible reminder on every startup. A compatible stopped
service can be started automatically (enabled by default) or from Settings;
startup times out after 10 seconds without blocking the main UI.

Linux reads the installed `knotlinkservice` version through dpkg and macOS reads
the `com.knotlink.service` Installer receipt. Their packages are managed by
systemd and launchd respectively. On all supported platforms the first-run
wizard and Settings may download KnotLinkService 3.2.0.0 from the official
release URL, retry through `gh-proxy.org`, and open the native system installer.
The user completes installation there.

The Minecraft companion mod must be version 3.0.0 or newer. Older or malformed
versions do not participate in hot workflows: hot backup falls back to an
ordinary live snapshot, while hot restore is rejected.

## macOS distribution status

The 1.16 DMG is arm64-only and ad-hoc signed, but not Apple-notarized. After
copying MineBackup to Applications, macOS may require **System Settings →
Privacy & Security → Open Anyway** on first launch. Do not disable Gatekeeper
and do not remove quarantine metadata with `xattr`.
