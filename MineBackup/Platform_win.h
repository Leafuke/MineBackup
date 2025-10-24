/*
* 
*
*/
#pragma once
#include <iostream>
#include <windows.h>
void RegisterHotkeys(HWND hwnd);
void UnregisterHotkeys(HWND hwnd);
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