#pragma once
#ifndef _PLATFORM_LINUX_H
#define _PLATFORM_LINUX_H
#include <iostream>
#include <string>
#include <filesystem>

// Linux stub implementations used when building on non-Windows platforms.
std::wstring SelectFileDialog();
std::wstring SelectFolderDialog();
std::wstring SelectSaveFileDialog(const std::wstring& defaultFileName = L"", const std::wstring& filter = L"");
std::wstring GetLastOpenTime(const std::wstring& worldPath);
std::wstring GetLastBackupTime(const std::wstring& backupDir);
std::wstring GetDocumentsPath();
bool CreateTrayIcon();
void RemoveTrayIcon();
void PumpLinuxDesktopEvents();
bool RegisterHotkeys(int hotkeyId, int key);
void UnregisterHotkeys(int hotkeyId);
void TriggerHotkeyBackup(std::string comment = "Hotkey");
void TriggerHotkeyRestore(const std::string& backupFile = "");
void GetUserDefaultUILanguageWin();
std::string GetRegistryValue(const std::string& key, const std::string& valueName);
// iconType: 0 info, 1 warning, 2 error (matching Windows signature)
void MessageBoxWin(const std::string& title, const std::string& message, int iconType = 0);
bool ConfirmMessageBox(const std::string& title, const std::string& message);
void OpenLinkInBrowser(const std::wstring& url);
void OpenFolder(const std::wstring& folderPath);
void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus);
void ReStartApplication();
void SetFileAttributesWin(const std::wstring& path, bool isHidden);
void EnableDarkModeWin(bool enable);
bool IsSystemDarkMode();
bool Extract7zToTempFile(std::wstring& extractedPath);
bool ExtractFontToTempFile(std::wstring& extractedPath);
bool IsFileLocked(const std::wstring& path);
#endif // !_PLATFORM_LINUX_H
