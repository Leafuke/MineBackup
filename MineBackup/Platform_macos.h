#pragma once
#ifndef _PLATFORM_MACOS_H
#define _PLATFORM_MACOS_H
#include <iostream>
#include <string>
#include <filesystem>

class Console;

void CheckForUpdatesThread();
std::wstring SelectFileDialog();
std::wstring SelectFolderDialog();
std::wstring SelectSaveFileDialog(const std::wstring& defaultFileName = L"", const std::wstring& filter = L"");
std::wstring GetLastOpenTime(const std::wstring& worldPath);
std::wstring GetLastBackupTime(const std::wstring& backupDir);
std::wstring GetDocumentsPath();
void CreateTrayIcon();
void RemoveTrayIcon();
void RegisterHotkeys(int hotkeyId, int key);
void UnregisterHotkeys(int hotkeyId);
void TriggerHotkeyBackup(std::string comment = "Hotkey");
void TriggerHotkeyRestore(const std::string& backupFile = "");
void GetUserDefaultUILanguageWin();
std::string GetRegistryValue(const std::string& key, const std::string& valueName);
void MessageBoxWin(const std::string& title, const std::string& message, int iconType = 0);
void OpenLinkInBrowser(const std::wstring& url);
void OpenFolder(const std::wstring& folderPath);
void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus);
void ReStartApplication();
void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable, bool silentStartupToTray = false);
void SetFileAttributesWin(const std::wstring& path, bool isHidden);
void EnableDarkModeWin(bool enable);
bool Extract7zToTempFile(std::wstring& extractedPath);
bool ExtractFontToTempFile(std::wstring& extractedPath);
bool IsFileLocked(const std::wstring& path);
bool RunCommandInBackground(const std::wstring& command, Console& console, bool useLowPriority, const std::wstring& workingDirectory = L"");
void CheckForNoticesThread();

#endif // !_PLATFORM_MACOS_H
