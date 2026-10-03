#pragma once
#ifndef _HISTORY_MANAGER_H
#define _HISTORY_MANAGER_H

#include "HistoryRepository.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

void AddHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFile, const std::wstring& backupType, const std::wstring& comment, const std::wstring& worldPath);
void RemoveHistoryEntry(int configIndex, const std::wstring& backupFileToRemove);
void RemoveHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFileToRemove);
void LoadHistory();
bool ExportHistoryToFile(const std::wstring& destinationPath, int configIndex = -1);
bool ImportHistoryFromFile(const std::wstring& sourcePath, int configIndex, bool mergeExisting);
std::vector<HistoryEntry> GetHistoryEntriesForConfig(int configIndex);
std::vector<HistoryEntry> GetHistoryEntriesForWorld(int configIndex, const std::wstring& worldName);
HistoryRepository::EntriesView GetHistoryEntriesViewForConfig(int configIndex);
std::shared_ptr<const HistorySnapshot> GetHistorySnapshot();
bool TryGetHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFile, HistoryEntry& outEntry);
bool UpsertHistoryEntry(int configIndex, const HistoryEntry& entry, bool overwriteExisting = false);
bool UpdateHistoryEntry(
	int configIndex,
	const std::wstring& worldName,
	const std::wstring& backupFile,
	const std::function<void(HistoryEntry&)>& update);
bool ReplaceHistoryEntriesForConfig(int configIndex, std::vector<HistoryEntry> entries);
bool ClearHistoryEntriesForWorld(int configIndex, const std::wstring& worldName);
bool RemoveHistoryEntriesIf(
	int configIndex,
	const std::function<bool(const HistoryEntry&)>& predicate,
	std::size_t* removedCount = nullptr);
bool UpdateHistoryCloudState(
	int configIndex,
	const std::wstring& worldName,
	const std::wstring& backupFile,
	bool isCloudArchived,
	const std::wstring& archivedAtUtc,
	const std::wstring& archiveRemotePath,
	const std::wstring& metadataRecordRemotePath,
	const std::wstring& metadataStateRemotePath);

HistoryRepository& GetHistoryRepository();

#endif // _HISTORY_MANAGER_H
