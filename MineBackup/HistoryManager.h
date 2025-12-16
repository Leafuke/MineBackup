#pragma once
#ifndef _HISTORY_MANAGER_H
#define _HISTORY_MANAGER_H
#include <iostream>
void AddHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFile, const std::wstring& backupType, const std::wstring& comment);
void RemoveHistoryEntry(int configIndex, const std::wstring& backupFileToRemove);
void SaveHistory();
void LoadHistory();
#endif // _HISTORY_MANAGER_H