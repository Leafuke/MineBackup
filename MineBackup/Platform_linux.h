#pragma once
#ifndef _PLATFORM_LINUX_H
#define _PLATFORM_LINUX_H
#include <iostream>
#include <string>
#include <filesystem>

class Console;
// Linux stub implementations used when building on non-Windows platforms.
void CheckForUpdatesThread();
std::wstring SelectFileDialog();
std::wstring SelectFolderDialog();
std::wstring GetLastOpenTime(const std::wstring& worldPath);
std::wstring GetLastBackupTime(const std::wstring& backupDir);
std::wstring GetDocumentsPath();
void RemoveTrayIcon();
void TriggerHotkeyBackup(std::string comment = "Hotkey");
void TriggerHotkeyRestore();
void GetUserDefaultUILanguageWin();
std::string GetRegistryValue(const std::string& key, const std::string& valueName);
// iconType: 0 info, 1 warning, 2 error (matching Windows signature)
void MessageBoxWin(const std::string& title, const std::string& message, int iconType = 0);
void OpenLinkInBrowser(const std::wstring& url);
void OpenFolder(const std::wstring& folderPath);
void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus);
void ReStartApplication();
void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable);
void SetFileAttributesWin(const std::wstring& path, bool isHidden);
void EnableDarkModeWin(bool enable);
bool Extract7zToTempFile(std::wstring& extractedPath);
bool ExtractFontToTempFile(std::wstring& extractedPath);
bool IsFileLocked(const std::wstring& path);
bool RunCommandInBackground(std::wstring command, Console& console, bool useLowPriority, const std::wstring& workingDirectory = L"");
#endif // !_PLATFORM_LINUX_H