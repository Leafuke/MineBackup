#pragma once
#ifndef _PLATFORM_WIN_H
#define _PLATFORM_WIN_H
#include <iostream>
#include <windows.h>
#include <string>
class Console;
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
std::wstring GetLastOpenTime(const std::wstring& worldPath);
std::wstring GetLastBackupTime(const std::wstring& backupDir);
void CreateTrayIcon(HWND hwnd, HINSTANCE hInstance);
void RemoveTrayIcon();
void TriggerHotkeyBackup(std::string comment = "Hotkey");
void TriggerHotkeyRestore();
void GetUserDefaultUILanguageWin();
void MessageBoxWin(const std::string& title, const std::string& message, int iconType);
HWND CreateHiddenWindow(HINSTANCE hInstance);
void OpenFolder(const std::wstring& folderPath);
void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus);
void ReStartApplication();
void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable);
bool RunCommandInBackground(std::wstring command, Console& console, bool useLowPriority, const std::wstring& workingDirectory = L"");
void CheckForNoticesThread();
#endif // !_PLATFORM_WIN_H