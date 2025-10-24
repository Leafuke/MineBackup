#pragma once
#include <iostream>
void LoadConfigs(const std::string& filename = "config.ini");
void SaveConfigs(const std::wstring& filename = L"config.ini");
void AddHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFile, const std::wstring& backupType, const std::wstring& comment);
void RemoveHistoryEntry(int configIndex, const std::wstring& backupFileToRemove);
int CreateNewSpecialConfig(const std::string& name_hint);
int CreateNewNormalConfig(const std::string& name_hint);