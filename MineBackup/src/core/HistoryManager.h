#pragma once
#ifndef _HISTORY_MANAGER_H
#define _HISTORY_MANAGER_H

#include <iostream>
#include <vector>
#include "AppState.h"

void AddHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFile, const std::wstring& backupType, const std::wstring& comment, const std::wstring& worldPath);
void RemoveHistoryEntry(int configIndex, const std::wstring& backupFileToRemove);
void RemoveHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFileToRemove);
void SaveHistory();
void LoadHistory();
bool ExportHistoryToFile(const std::wstring& destinationPath, int configIndex = -1);
bool ImportHistoryFromFile(const std::wstring& sourcePath, int configIndex, bool mergeExisting);
std::vector<HistoryEntry> GetHistoryEntriesForConfig(int configIndex);
std::vector<HistoryEntry> GetHistoryEntriesForWorld(int configIndex, const std::wstring& worldName);
bool TryGetHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFile, HistoryEntry& outEntry);
HistoryEntry* FindHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFile);
bool UpsertHistoryEntry(int configIndex, const HistoryEntry& entry, bool overwriteExisting = false);
bool UpdateHistoryCloudState(
	int configIndex,
	const std::wstring& worldName,
	const std::wstring& backupFile,
	bool isCloudArchived,
	const std::wstring& archivedAtUtc,
	const std::wstring& archiveRemotePath,
	const std::wstring& metadataRecordRemotePath,
	const std::wstring& metadataStateRemotePath);

#endif // _HISTORY_MANAGER_H
