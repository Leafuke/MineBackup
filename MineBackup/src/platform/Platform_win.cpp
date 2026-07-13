#define GLFW_EXPOSE_NATIVE_WIN32
#include <dwmapi.h>
#include "Platform_win.h"
#include "text_to_text.h"
#include "AppState.h"
#include "AppPaths.h"
#include "ExternalToolManager.h"
#include "Globals.h"
#include "resource.h"
#include "i18n.h"
#include "Console.h"
#include "ConfigManager.h"
#include "RotatingFileLog.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <wchar.h>
#include <functional>
#include <vector>
#include <cctype>
#pragma comment(lib, "dwmapi.lib")
using namespace std;

NOTIFYICONDATA nid = { 0 };

LRESULT WINAPI HiddenWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void EnableDarkModeWin(bool enable) {
	HWND hwnd = glfwGetWin32Window(wc);
	BOOL useDark = enable ? TRUE : FALSE;
	DwmSetWindowAttribute(hwnd, 20 , &useDark, sizeof(useDark));
	return;
}



wstring GetDocumentsPath() {
#ifdef _WIN32
	PWSTR path = NULL;
	HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &path);
	if (SUCCEEDED(hr)) {
		wstring result(path);
		CoTaskMemFree(path);
		return result;
	}
#endif
	return L"";
}

void SetFileAttributesWin(const wstring& path, bool isHidden) {
	if(isHidden)
		SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN);
	else
		SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
}

void ExecuteCmd(const string &cmd) {
	//ShellExecuteW(NULL, L"open", L"explorer.exe", cmd.c_str(), NULL, SW_SHOWNORMAL);
}
void OpenFolder(const wstring& folderPath) {
	ShellExecuteW(NULL, L"open", folderPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
}
void OpenFolderWithFocus(const wstring folderPath, const wstring focus) {
	ShellExecuteW(NULL, L"open", L"explorer.exe", focus.c_str(), NULL, SW_SHOWNORMAL);
}
void OpenLinkInBrowser(const wstring& url) {
	ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
}
void ReStartApplication() {
	wchar_t selfPath[MAX_PATH];
	GetModuleFileNameW(NULL, selfPath, MAX_PATH);
	ShellExecuteW(NULL, L"open", selfPath, NULL, NULL, SW_SHOWNORMAL);
	PostQuitMessage(0);
	return;
}

void GetUserDefaultUILanguageWin() {
	LANGID langId = GetUserDefaultUILanguage();
	switch (PRIMARYLANGID(langId)) {
	case LANG_CHINESE:
		SetLanguage("zh_CN");
		break;
	case LANG_CHINESE_TRADITIONAL:
		SetLanguage("zh_CN");
		break;
	case LANG_ENGLISH:
		SetLanguage("en_US");
		break;
	default:
		SetLanguage("en_US"); // 默认英语
		break;
	}
	return;
}

// iconType: 2 = error, 0 = info, 1 = warning
void MessageBoxWin(const string& title, const string& message, int iconType) {
	switch (iconType)
	{
	case 0:
		MessageBoxW(nullptr, utf8_to_wstring(L(message.c_str())).c_str(), utf8_to_wstring(L(title.c_str())).c_str(), MB_OK | MB_ICONINFORMATION);
		break;
	case 1:
		MessageBoxW(nullptr, utf8_to_wstring(L(message.c_str())).c_str(), utf8_to_wstring(L(title.c_str())).c_str(), MB_OK | MB_ICONWARNING);
		break;
	case 2:
		MessageBoxW(nullptr, utf8_to_wstring(L(message.c_str())).c_str(), utf8_to_wstring(L(title.c_str())).c_str(), MB_OK | MB_ICONERROR);
		break;
	default:
		break;
	}
	return;
}

HWND CreateHiddenWindow(HINSTANCE hInstance) {
	const wchar_t HIDDEN_CLASS_NAME[] = L"MineBackupHiddenWindowClass";
	WNDCLASSW wc_hidden = {};
	wc_hidden.lpfnWndProc = HiddenWndProc;
	wc_hidden.hInstance = hInstance;
	wc_hidden.lpszClassName = HIDDEN_CLASS_NAME;
	RegisterClassW(&wc_hidden);

	HWND hwnd_hidden = CreateWindowExW(0, HIDDEN_CLASS_NAME, L"MineBackup Hidden Window", 0,
		0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
	if (hwnd_hidden == NULL)
		return NULL; // 创建失败
	return hwnd_hidden;
}

void RegisterHotkeys(HWND hwnd, int hotkeyId, int key) {
	RegisterHotKey(hwnd, hotkeyId, MOD_ALT | MOD_CONTROL, key);
}
void UnregisterHotkeys(HWND hwnd, int hotKeyId) {
	::UnregisterHotKey(hwnd, hotKeyId);
}
void CreateTrayIcon(HWND hwnd, HINSTANCE hInstance) {
	// 初始化托盘图标 (nid)
	nid.cbSize = sizeof(NOTIFYICONDATA);
	nid.hWnd = hwnd;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_USER + 1;
	nid.hIcon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_ICON3), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
#ifdef UNICODE
	wcscpy_s(nid.szTip, L"MineBackup");
#else
	strcpy_s(nid.szTip, "MineBackup");
#endif
	Shell_NotifyIcon(NIM_ADD, &nid);
}
void RemoveTrayIcon() {
	Shell_NotifyIcon(NIM_DELETE, &nid);
}

bool IsFileLocked(const wstring& path) {
	if (!filesystem::exists(path)) {
		return false;
	}
	HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();
		if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
			return true;
		}
		// 其他错误（如文件不存在等）不视为锁定
		return false;
	}
	CloseHandle(hFile);
	return false;
}


LRESULT WINAPI HiddenWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_USER + 1: // 托盘图标消息
		if (lParam == WM_LBUTTONUP) {
			g_appState.showMainApp = true;
			glfwShowWindow(wc);
		}
		else if (lParam == WM_RBUTTONUP) {
			HMENU hMenu = CreatePopupMenu();
			AppendMenuW(hMenu, MF_STRING, 1001, utf8_to_wstring((string)L("OPEN")).c_str());
			AppendMenuW(hMenu, MF_STRING, 1002, utf8_to_wstring((string)L("EXIT")).c_str());

			// 获取鼠标位置（菜单显示在鼠标右键点击的位置）
			POINT pt;
			GetCursorPos(&pt);

			// 显示菜单（TPM_BOTTOMALIGN：菜单底部对齐鼠标位置）
			TrackPopupMenu(
				hMenu,
				TPM_BOTTOMALIGN | TPM_LEFTBUTTON,  // 菜单样式
				pt.x, pt.y,
				0,
				hWnd,
				NULL
			);

			// 必须调用此函数，否则菜单可能无法正常关闭
			SetForegroundWindow(hWnd);
			// 销毁菜单（避免内存泄漏）
			DestroyMenu(hMenu);
			break;
		}
		return 0;
	case WM_HOTKEY:
		if (wParam == MINEBACKUP_HOTKEY_ID) {
			TriggerHotkeyBackup();
		}
		else if (wParam == MINERESTORE_HOTKEY_ID) {
			TriggerHotkeyRestore();
		}
		return 0;
	case WM_COMMAND: {
		switch (LOWORD(wParam)) {
		case 1001:  // 点击“打开界面”
			g_appState.showMainApp = true;
			glfwShowWindow(wc);
			SetForegroundWindow(hWnd);
			break;
		case 1002:  // 点击“关闭”
			// 先移除托盘图标，再退出程序
			SaveConfigs();
			g_appState.done = true;
			Shell_NotifyIcon(NIM_DELETE, &nid);
			PostQuitMessage(0);
			break;
		}
		break;
	}
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
			return 0;
		break;
	case WM_DESTROY:
		Shell_NotifyIcon(NIM_DELETE, &nid);  // 清理托盘图标
		g_appState.done = true;
		::PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

// 注册表查询
string GetRegistryValue(const string & keyPath, const string & valueName)
{
	HKEY hKey;
	string valueData;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		char buffer[1024];
		DWORD dataSize = sizeof(buffer);
		if (RegGetValueA(hKey, NULL, valueName.c_str(), RRF_RT_ANY, NULL, buffer, &dataSize) == ERROR_SUCCESS) {
			valueData = buffer;
		}
		RegCloseKey(hKey);
	}
	else
		return "";
	return valueData;
}

wstring GetLastOpenTime(const wstring& worldPath) {
	try {
		auto ftime = filesystem::last_write_time(worldPath);
		// 转换为 system_clock::time_point
		auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
			ftime - filesystem::file_time_type::clock::now()
			+ chrono::system_clock::now()
		);
		time_t cftime = chrono::system_clock::to_time_t(sctp);
		wchar_t buf[64];
		struct tm timeinfo;
		//wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtime要换成更安全的localtime
		if (localtime_s(&timeinfo, &cftime) == 0) {
			wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
			return buf;
		}
		else {
			return L"N/A";
		}
	}
	catch (...) {
		return L"N/A";
	}
}

wstring GetLastBackupTime(const wstring& backupDir) {
	time_t latest = 0;
	try {
		if (filesystem::exists(backupDir)) {
			for (const auto& entry : filesystem::directory_iterator(backupDir)) {
				if (entry.is_regular_file()) {
					auto ftime = entry.last_write_time();
					// 转换为 system_clock::time_point
					auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
						ftime - filesystem::file_time_type::clock::now()
						+ chrono::system_clock::now()
					);
					time_t cftime = chrono::system_clock::to_time_t(sctp);
					if (cftime > latest) latest = cftime;
				}
			}
		}
		if (latest == 0) return L"/";
		wchar_t buf[64];
		struct tm timeinfo;
		//wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtime要换成更安全的localtime
		if (localtime_s(&timeinfo, &latest) == 0) {
			wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
			return buf;
		}
		else {
			return L"N/A";
		}
	}
	catch (...) {
		return L"N/A";
	}
}

// configType: 1 特殊配置
void SetAutoStart(const string& appName, const wstring& appPath, bool configType, int& configId, bool& enable, bool silentStartupToTray) {
	HKEY hKey;
	const wstring keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

	// LSTATUS是Windows API中标准返回类型
	LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_WRITE, &hKey);

	if (status == ERROR_SUCCESS) {
		if (enable) {
			wstring command;
			if (configType) // 特殊配置
				command = L"\"" + appPath + L"\" -specialcfg " + to_wstring(configId);
			else // 普通配置
				command = L"\"" + appPath + L"\" -cfg " + to_wstring(configId);
			if (silentStartupToTray)
				command += L" --silent-startup";

			// RegSetValueExW 需要6个参数: HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD
			RegSetValueExW(
				hKey,
				utf8_to_wstring(appName).c_str(),
				0,
				REG_SZ,
				(const BYTE*)command.c_str(),
				(DWORD)((command.length() + 1) * sizeof(wchar_t))
			);
		}
		else {
			// RegDeleteValueW 需要2个参数: HKEY, LPCWSTR
			RegDeleteValueW(hKey, utf8_to_wstring(appName).c_str());
		}
		RegCloseKey(hKey);
	}
}


static std::filesystem::path g_logFilePath;

void SetLogFilePath(const std::string& path) {
    g_logFilePath = std::filesystem::u8path(path);
}

std::string GetCurrentTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm_now;
    localtime_s(&tm_now, &now);
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void WriteLogEntry(const std::string& message, LogLevel level) {
    std::string level_str;
    switch (level) {
        case LogLevel::Info: level_str = "[INFO]"; break;
        case LogLevel::Warning: level_str = "[WARN]"; break;
        case LogLevel::Error: level_str = "[ERROR]"; break;
        default: level_str = "[INFO]"; break;
    }
    const auto path = g_logFilePath.empty() ? GetAppPaths().logsRoot / "auto_log.txt" : g_logFilePath;
    RotatingFileLog::Append(path, GetCurrentTimestamp() + " " + level_str + " " + message + "\n");
}

bool ConfirmMessageBox(const string& title, const string& message) {
	return MessageBoxW(nullptr, utf8_to_wstring(message).c_str(), utf8_to_wstring(title).c_str(),
		MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

//选择文件
wstring SelectFileDialog() {
	HWND hwndOwner = NULL;
	IFileDialog* pfd;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
		IID_IFileDialog, reinterpret_cast<void**>(&pfd));

	if (SUCCEEDED(hr)) {
		hr = pfd->Show(hwndOwner);
		if (SUCCEEDED(hr)) {
			IShellItem* psi;
			hr = pfd->GetResult(&psi);
			if (SUCCEEDED(hr)) {
				PWSTR path = nullptr;
				psi->GetDisplayName(SIGDN_FILESYSPATH, &path);
				wstring wpath(path);
				CoTaskMemFree(path);
				psi->Release();
				return wpath;
			}
		}
		pfd->Release();
	}
	return L"";
}

//选择文件夹
wstring SelectFolderDialog() {
	HWND hwndOwner = NULL;
	IFileDialog* pfd;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
		IID_IFileDialog, reinterpret_cast<void**>(&pfd));

	if (SUCCEEDED(hr)) {
		DWORD options;
		pfd->GetOptions(&options);
		pfd->SetOptions(options | FOS_PICKFOLDERS); // 设置为选择文件夹
		hr = pfd->Show(hwndOwner);
		if (SUCCEEDED(hr)) {
			IShellItem* psi;
			hr = pfd->GetResult(&psi);
			if (SUCCEEDED(hr)) {
				PWSTR path = nullptr;
				psi->GetDisplayName(SIGDN_FILESYSPATH, &path);
				wstring wpath(path);
				CoTaskMemFree(path);
				psi->Release();
				return wpath;
			}
		}
		pfd->Release();
	}
	return L"";
}

// 保存文件对话框
wstring SelectSaveFileDialog(const wstring& defaultFileName, const wstring& filter) {
	HWND hwndOwner = NULL;
	IFileSaveDialog* pfd;
	HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_ALL,
		IID_IFileSaveDialog, reinterpret_cast<void**>(&pfd));

	if (SUCCEEDED(hr)) {
		// 设置默认文件名
		if (!defaultFileName.empty()) {
			pfd->SetFileName(defaultFileName.c_str());
		}
		
		hr = pfd->Show(hwndOwner);
		if (SUCCEEDED(hr)) {
			IShellItem* psi;
			hr = pfd->GetResult(&psi);
			if (SUCCEEDED(hr)) {
				PWSTR path = nullptr;
				psi->GetDisplayName(SIGDN_FILESYSPATH, &path);
				wstring wpath(path);
				CoTaskMemFree(path);
				psi->Release();
				pfd->Release();
				return wpath;
			}
		}
		pfd->Release();
	}
	return L"";
}

bool Extract7zToTempFile(wstring& extractedPath) {
	// 用主模块句柄
	HRSRC hRes = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDR_EXE1), L"EXE");
	if (!hRes) return false;

	HGLOBAL hData = LoadResource(GetModuleHandleW(NULL), hRes);
	if (!hData) return false;

	DWORD dataSize = SizeofResource(GetModuleHandleW(NULL), hRes);
	if (dataSize == 0) return false;

	LPVOID pData = LockResource(hData);
	if (!pData) return false;

	const auto install = ExternalToolManager::InstallBundledSevenZipForWindows(
		pData, static_cast<size_t>(dataSize), GetAppPaths());
	if (!install.success) return false;
	extractedPath = install.executable.wstring();
	return true;
}

bool GetBundledIconFontResource(const void*& data, size_t& size) {
	data = nullptr;
	size = 0;
	HRSRC hRes = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDR_FONTS1), L"FONTS");
	if (!hRes) return false;
	HGLOBAL hData = LoadResource(GetModuleHandleW(NULL), hRes);
	if (!hData) return false;
	DWORD dataSize = SizeofResource(GetModuleHandleW(NULL), hRes);
	if (dataSize == 0) return false;
	LPVOID pData = LockResource(hData);
	if (!pData) return false;
	data = pData;
	size = static_cast<size_t>(dataSize);
	return true;
}
