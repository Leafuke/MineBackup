#pragma once
#ifndef _PLATFORM_WIN_H
#define _PLATFORM_WIN_H
#include <iostream>
#include <windows.h>
#include <string>
struct Console;
enum class LogLevel {
    Info,
    Warning,
    Error
};
void CheckForUpdatesThread();
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
HWND CreateHiddenWindow(HINSTANCE hInstance);
void OpenLinkInBrowser(const std::wstring& url);
void OpenFolder(const std::wstring& folderPath);
void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus);
void ReStartApplication();
bool Extract7zToTempFile(std::wstring& extractedPath);
bool ExtractFontToTempFile(std::wstring& extractedPath);
bool IsFileLocked(const std::wstring& path);
std::string GetRegistryValue(const std::string& keyPath, const std::string& valueName);
void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable, bool silentStartupToTray = false);
bool RunCommandInBackground(const std::wstring& command, Console& console, bool useLowPriority, const std::wstring& workingDirectory = L"");
bool RunCommandWithResult(const std::wstring& command, Console& console, bool useLowPriority, int timeoutSeconds, int& exitCode, bool& timedOut, std::string& errorMessage, const std::wstring& workingDirectory = L"");
void CheckForNoticesThread();
#endif // !_PLATFORM_WIN_H
