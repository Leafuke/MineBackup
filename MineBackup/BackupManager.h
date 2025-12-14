#pragma once
#ifndef _BACKUP_MANAGER_H
#define _BACKUP_MANAGER_H
#include <iostream>
#include <filesystem>
#include <atomic>
#include "AppState.h"
#include "Console.h"
void DoBackup(const MyFolder& folder, Console& console, const std::wstring& comment = L"");
void DoRestore2(const Config config, const std::wstring& worldName, const std::filesystem::path& fullBackupPath, Console& console, int restoreMethod);
void DoRestore(const Config config, const std::wstring& worldName, const std::wstring& backupFile, Console& console, int restoreMethod, const std::string& customRestoreList = "");
void DoHotRestore(const MyFolder& world, Console& console, bool deleteBackup);
void DoOthersBackup(const Config config, std::filesystem::path backupWhat, const std::wstring& comment);
void AutoBackupThreadFunction(int configIdx, int worldIdx, int intervalMinutes, Console* console, std::atomic<bool>& stop_flag);
void DoSafeDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, int configIndex, Console& console);
void DoDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, int& configIndex, Console& console);
void AddBackupToWESnapshots(const Config config, const std::wstring& worldName, const std::wstring& backupFile, Console& console);
#endif // BACKUP_MANAGER_H