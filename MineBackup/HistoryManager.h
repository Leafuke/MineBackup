#pragma once
#include <iostream>
void AddHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFile, const std::wstring& backupType, const std::wstring& comment);
void RemoveHistoryEntry(int configIndex, const std::wstring& backupFileToRemove);
void SaveHistory();
void LoadHistory();