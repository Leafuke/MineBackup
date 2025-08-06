[中文](README-zn.md) | **English** <!-- lang -->

![MineBackup](MineBackup/MineBackup.png)

# MineBackup - Minecraft Archive Backup Program

MineBackup is a user-friendly application designed to help you easily back up, restore, and export your files. It works with almost any version of Minecraft!

Well, you can actually use it to backup any folders in your computer :P

## 📸Features

- **User Friendly GUI**: Use ImGUI, simple but efficient. (v1.5.0+)
- **Backup**: Securely backup your game saves with just a click.
- **Restore**: Quickly restore your game saves to a previous state from either a `.7z` file or directly from your computer.
- **High-Compression**: Use built-in 7-Zip to compress your archive and save storage space on your computer.
- **Intelligent**: You can choose Git-Like mode to backup and restore your archive.
- **Backup Path**: Set a custom path for your backups according to your storage preferences.
- **Multilanguage**: We have supported two languages by now, but they can be more with your help!

## ⚙️Installation

1. **Download**: Navigate to the [latest release](https://github.com/Leafuke/MineBackup/releases) and download the single file for Windows.
2. **Double click**: That's ALL. No need to install.

## 🗃️Support

For issues, feature requests, or contributions, please visit the [GitHub issues](https://github.com/Leafuke/MineBackup/issues). <br />
You can help internationalize this project by translating the [i18n.h](MineBackup/i18n.h).<br />
Help documentation is being written.✒️

## 🛠️Compilation Guide

The language standard for which this code is C++17. Link imgui library.

## 🔗KnotLink

 `APPID` : `0x00000020`
 `socket ID` : `0x00000010`
 `signal ID` : `0x00000020`
- `BACKUP`: BACKUP <config_idx> <world_idx> [comment]
- `RESTORE`: RESTORE <config_idx>
- `GET_CONFIG`: GET_CONFIG <config_idx>
- `LIST_WORLDS`: LIST_WORLDS <config_idx>
- `LIST_CONFIGS`: LIST_CONFIGS
- `GET_CONFIG`: GET_CONFIG <config_idx>
- `SET_CONFIG`: SET_CONFIG <config_idx> <key> <value> <br />
 key = backup_mode / hot_backup<br />
 value = 1/2/3     /  0/1

## 📄Project References

[7-Zip](https://github.com/ip7z/7zip) (7-zip.org)
> Who provides 7z.exe for MineBackup

[imgui](https://github.com/ocornut/imgui) 
> Who provides useful functions and GUI for MineBackup v1.5.0+

[stb](https://github.com/nothings/stb) 
> Who helps MineBackup to load image

KnotLink
> Who helps MineBackup to "communicate" with other programes
