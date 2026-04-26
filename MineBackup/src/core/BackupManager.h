#pragma once
#ifndef _BACKUP_MANAGER_H
#define _BACKUP_MANAGER_H
#include <iostream>
#include <filesystem>
#include <atomic>
#include "AppState.h"
#include "Console.h"

enum class BackupCheckResult {
	NO_CHANGE,
	CHANGES_DETECTED,
	FORCE_FULL_BACKUP_METADATA_INVALID,
	FORCE_FULL_BACKUP_BASE_MISSING
};

struct BackupFileState {
	uintmax_t size = 0;
	long long lastWriteTimeTicks = 0;
};

struct BackupChangeSet {
	std::vector<std::wstring> addedFiles;
	std::vector<std::wstring> modifiedFiles;
	std::vector<std::wstring> deletedFiles;

	bool HasChanges() const {
		return !addedFiles.empty() || !modifiedFiles.empty() || !deletedFiles.empty();
	}

	bool HasContentChanges() const {
		return !addedFiles.empty() || !modifiedFiles.empty();
	}
};

void DoBackup(const MyFolder& folder, Console& console, const std::wstring& comment = L"");
bool DoRestore2(const Config& config, const std::wstring& worldName, const std::filesystem::path& fullBackupPath, Console& console, int restoreMethod);
bool DoRestore(const Config& config, const std::wstring& worldName, const std::wstring& backupFile, Console& console, int restoreMethod, const std::string& customRestoreList = "");
void DoHotRestore(const MyFolder& world, Console& console, bool deleteBackup, const std::wstring& backupFile = L"");
void DoOthersBackup(const Config& config, std::filesystem::path backupWhat, const std::wstring& comment, Console& console);
void AutoBackupThreadFunction(int configIdx, int worldIdx, int intervalMinutes, Console* console, std::atomic<bool>& stop_flag);

enum class BackupDeleteMode {
	HistoryOnly = 0,
	LocalArchiveOnly = 1,
	LocalArchiveAndHistory = 2
};

void DeleteBackupWithMode(const Config& config, const HistoryEntry& entryToDelete, int configIndex, BackupDeleteMode mode, bool useSafeDelete, Console& console);
void DoSafeDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, int configIndex, Console& console);
void DoDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, int& configIndex, Console& console);
void AddBackupToWESnapshots(const Config& config, const std::wstring& worldName, const std::wstring& backupFile, Console& console);
#endif // BACKUP_MANAGER_H
