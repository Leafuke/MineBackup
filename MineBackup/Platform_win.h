/*
* 
*
*/
#pragma once
#include <iostream>
#include <windows.h>
void CheckForUpdatesThread();
void RegisterHotkeys(HWND hwnd);
void UnregisterHotkeys(HWND hwnd);
std::wstring SelectFileDialog(HWND hwndOwner = NULL);
std::wstring SelectFolderDialog(HWND hwndOwner = NULL);
std::wstring GetLastOpenTime(const std::wstring& worldPath);
std::wstring GetLastBackupTime(const std::wstring& backupDir);
void CreateTrayIcon(HWND hwnd, HINSTANCE hInstance);
void RemoveTrayIcon();
void TriggerHotkeyBackup();
void TriggerHotkeyRestore();
void GetUserDefaultUILanguageWin();
void MessageBoxWin(const std::string& title, const std::string& message);
HWND CreateHiddenWindow(HINSTANCE hInstance);
void ExecuteCmd();
void OpenFolder(const std::wstring& folderPath);
void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus);
void ReStartApplication();
void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable);