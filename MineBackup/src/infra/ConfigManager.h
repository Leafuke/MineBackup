#pragma once
#ifndef _CONFIG_MANAGER_H
#define _CONFIG_MANAGER_H
#include <iostream>
#include <string>
#include <vector>
void LoadConfigs(const std::string& filename = "config.ini");
void SaveConfigs(const std::wstring& filename = L"config.ini");
void AddHistoryEntry(int configIndex, const std::wstring& worldName, const std::wstring& backupFile, const std::wstring& backupType, const std::wstring& comment, const std::wstring& worldPath = L"");
void RemoveHistoryEntry(int configIndex, const std::wstring& backupFileToRemove);
int CreateNewSpecialConfig(const std::string& name_hint = "None");
int CreateNewNormalConfig(const std::string& name_hint = "None");
std::vector<std::wstring> DefaultBackupBlacklist();
std::vector<std::wstring> DefaultRestoreWhitelist();
std::vector<std::wstring> BuildEffectiveRestoreWhitelist(const std::vector<std::wstring>& userWhitelist);
void EnsureDefaultBackupBlacklist(std::vector<std::wstring>& blacklist);
void EnsureDefaultRestoreWhitelist();
#endif // CONFIG_MANAGER_H
