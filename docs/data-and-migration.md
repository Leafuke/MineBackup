# Data locations, portable mode and 1.15 migration

MineBackup does not depend on the current working directory and does not require
`config.ini` beside the executable. A profile owns separate `config`, `data`,
`state`, `cache`, `runtime`, `tools` and `logs` roots. `--data-dir <absolute
path>` selects the complete profile root and takes precedence over every other
mode; an invalid explicit path is an error rather than a silent fallback.

Default locations:

- Windows: `%LOCALAPPDATA%\MineBackup\{config,data,state,cache,runtime,tools,logs}`.
- Linux: the matching XDG config/data/state/cache/runtime roots. If
  `XDG_RUNTIME_DIR` is absent or unsafe, MineBackup uses a private mode-0700
  runtime directory below the state root.
- macOS: config/data/state/tools under `~/Library/Application Support/MineBackup`,
  cache/runtime under `~/Library/Caches/MineBackup`, and logs under the standard
  Library Logs location.

On Windows and AppImage only, an adjacent `portable.flag` selects
`MineBackupData/{config,data,state,cache,runtime,tools,logs}` beside the
executable/AppImage. macOS applications never write inside `.app`. AppImage
uses normal XDG locations unless the marker exists.

## Migrating from 1.15

Startup order is parameters → AppPaths → per-profile single-instance lock → old
location discovery and confirmation → transactional 1.15 conversion → 1.16
data load → desktop, task and network services. Source files are not deleted,
moved, renamed or recompressed. Recovery snapshots are retained below
`state/migration-snapshots/1.15/<transaction-id>` and the UI reports their
location and size.

The permanent Migration Coordinator owns transactions, reports and write gates.
The 1.16-only V15 Migration Adapter and read-only legacy reader interpret old
formats. Failed dependent units remain pending and block only their dangerous
writes. Run 1.16 to migrate old data before upgrading to a future 1.17 release,
where the v1.15 reader and adapter are removed.

## Portable cloud configuration and rclone

Cloud configuration exchange uses `portable-config.json`, keyed by stable
ConfigId and restricted to an explicit portable-field whitelist. Paths, tool
locations, credentials, commands, scripts, automation, special configurations
and legacy Service Mode fields are excluded. New remote configurations remain
pending until local world and backup paths are bound.

rclone is not included in MineBackup packages. MineBackup installs only the
version pinned by its release manifest, only after user confirmation, from the
official rclone source and after SHA-256 plus `rclone version` validation.
MineBackup does not copy, parse or upload the user's rclone credential file.

## Windows Service Mode

Service Mode is deprecated in 1.16. MineBackup cannot install or start a
service. The local legacy fields are preserved read-only so the settings page
can identify an older service. Removal requires user confirmation, UAC, an
absolute `MineBackup.exe ... --service` ImagePath and MineBackup resource
validation; otherwise the program leaves the service untouched and gives manual
inspection guidance. The compatibility fields are removed in 1.17.
