#pragma once
#include <iostream>
void CheckForUpdatesThread();
std::wstring SelectFileDialog();
std::wstring SelectFolderDialog();
std::wstring GetLastOpenTime(const std::wstring& worldPath);
std::wstring GetLastBackupTime(const std::wstring& backupDir);
void RemoveTrayIcon();
void TriggerHotkeyBackup();
void TriggerHotkeyRestore();
void GetUserDefaultUILanguageWin();
void MessageBoxWin(const std::string& title, const std::string& message);
void OpenFolder(const std::wstring& folderPath);
void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus);
void ReStartApplication();
void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable);