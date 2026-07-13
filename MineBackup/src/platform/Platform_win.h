#pragma once
#ifndef _PLATFORM_WIN_H
#define _PLATFORM_WIN_H
#include <iostream>
#include <windows.h>
#include <cstddef>
#include <string>
struct Console;
enum class LogLevel {
    Info,
    Warning,
    Error
};
void RegisterHotkeys(HWND hwnd, int hotkeyId, int key);
void UnregisterHotkeys(HWND hwnd, int hotkeyId);
std::wstring SelectFileDialog();
std::wstring SelectFolderDialog();
std::wstring SelectSaveFileDialog(const std::wstring& defaultFileName = L"", const std::wstring& filter = L"");
std::wstring GetDocumentsPath();
std::wstring GetLastOpenTime(const std::wstring& worldPath);
std::wstring GetLastBackupTime(const std::wstring& backupDir);
void CreateTrayIcon(HWND hwnd, HINSTANCE hInstance);
void RemoveTrayIcon();
void TriggerHotkeyBackup(std::string comment = "Hotkey");
void TriggerHotkeyRestore(const std::string& backupFile = "");
void SetFileAttributesWin(const std::wstring& path, bool isHidden);
void EnableDarkModeWin(bool enable);
void GetUserDefaultUILanguageWin();
void MessageBoxWin(const std::string& title, const std::string& message, int iconType);
bool ConfirmMessageBox(const std::string& title, const std::string& message);
HWND CreateHiddenWindow(HINSTANCE hInstance);
void OpenLinkInBrowser(const std::wstring& url);
void OpenFolder(const std::wstring& folderPath);
void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus);
void ReStartApplication();
bool Extract7zToTempFile(std::wstring& extractedPath);
bool GetBundledIconFontResource(const void*& data, std::size_t& size);
bool IsFileLocked(const std::wstring& path);
std::string GetRegistryValue(const std::string& keyPath, const std::string& valueName);
void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable, bool silentStartupToTray = false);
#endif // !_PLATFORM_WIN_H
