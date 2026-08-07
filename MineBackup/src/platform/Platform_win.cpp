#define GLFW_EXPOSE_NATIVE_WIN32
#include <dwmapi.h>
#include "Platform_win.h"
#include "PlatformCompat.h"
#include "text_to_text.h"
#include "AppState.h"
#include "AppPaths.h"
#include "ExternalToolManager.h"
#include "Globals.h"
#include "resource.h"
#include "i18n.h"
#include "ConfigManager.h"
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
#include <memory>
#include <vector>
#include <cctype>
#pragma comment(lib, "dwmapi.lib")
using namespace std;

NOTIFYICONDATAW nid = { 0 };
HICON g_trayIcon = nullptr;
bool g_trayIconAdded = false;
constexpr UINT WM_MINEBACKUP_TRAY_NOTIFY = WM_APP + 0x120;

struct TrayNotificationRequest {
	wstring title;
	wstring message;
};

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

	HWND hwnd_hidden = CreateWindowExW(
		WS_EX_TOOLWINDOW,
		HIDDEN_CLASS_NAME,
		L"MineBackup Hidden Window",
		WS_POPUP,
		0, 0, 0, 0,
		nullptr,
		nullptr,
		hInstance,
		nullptr);
	if (hwnd_hidden == NULL)
		return NULL; // 创建失败
	return hwnd_hidden;
}

bool CreateTrayIcon(HWND hwnd, HINSTANCE hInstance) {
	if (g_trayIconAdded) return true;
	// 初始化托盘图标 (nid)
	nid = {};
	nid.cbSize = sizeof(NOTIFYICONDATAW);
	nid.hWnd = hwnd;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_USER + 1;
	g_trayIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_ICON3),
		IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
	if (!g_trayIcon) return false;
	nid.hIcon = g_trayIcon;
	wcscpy_s(nid.szTip, L"MineBackup");
	if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
		DestroyIcon(g_trayIcon);
		g_trayIcon = nullptr;
		return false;
	}
	g_trayIconAdded = true;
	NOTIFYICONDATAW version = nid;
	version.uVersion = NOTIFYICON_VERSION_4;
	(void)Shell_NotifyIconW(NIM_SETVERSION, &version);
	return true;
}
void RemoveTrayIcon() {
	if (g_trayIconAdded) {
		(void)Shell_NotifyIconW(NIM_DELETE, &nid);
		g_trayIconAdded = false;
	}
	if (g_trayIcon) {
		DestroyIcon(g_trayIcon);
		g_trayIcon = nullptr;
	}
	nid = {};
}

bool ShowTrayNotification(const wstring& title, const wstring& message) {
	if (title.empty() || message.empty() || !g_trayIconAdded || !nid.hWnd) return false;
	DWORD windowThread = GetWindowThreadProcessId(nid.hWnd, nullptr);
	if (windowThread != GetCurrentThreadId()) {
		auto request = make_unique<TrayNotificationRequest>(
			TrayNotificationRequest{title, message});
		if (!PostMessageW(nid.hWnd, WM_MINEBACKUP_TRAY_NOTIFY, 0,
				reinterpret_cast<LPARAM>(request.get()))) {
			return false;
		}
		(void)request.release();
		return true;
	}
	NOTIFYICONDATAW notification = nid;
	notification.uFlags = NIF_INFO;
	wcsncpy_s(notification.szInfoTitle, title.c_str(), _TRUNCATE);
	wcsncpy_s(notification.szInfo, message.c_str(), _TRUNCATE);
	notification.dwInfoFlags = NIIF_INFO;
	return Shell_NotifyIconW(NIM_MODIFY, &notification) == TRUE;
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
	case WM_MINEBACKUP_TRAY_NOTIFY: {
		unique_ptr<TrayNotificationRequest> request(
			reinterpret_cast<TrayNotificationRequest*>(lParam));
		if (request) {
			(void)ShowTrayNotification(request->title, request->message);
		}
		return 0;
	}
	case WM_USER + 1: // 托盘图标消息
		if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == NIN_SELECT
			|| LOWORD(lParam) == NIN_KEYSELECT
			|| LOWORD(lParam) == NIN_BALLOONUSERCLICK) {
			g_appState.showMainApp = true;
			if (wc) {
				glfwShowWindow(wc);
				glfwRestoreWindow(wc);
				glfwFocusWindow(wc);
				SetForegroundWindow(glfwGetWin32Window(wc));
			}
		}
		else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
			HMENU hMenu = CreatePopupMenu();
			AppendMenuW(hMenu, MF_STRING, 1001, utf8_to_wstring((string)L("OPEN")).c_str());
			AppendMenuW(hMenu, MF_STRING, 1002, utf8_to_wstring((string)L("EXIT")).c_str());

			// 获取鼠标位置（菜单显示在鼠标右键点击的位置）
			POINT pt;
			GetCursorPos(&pt);

			// The tray owner must be a foreground-capable top-level window before
			// TrackPopupMenu starts its modal loop, otherwise Windows dismisses
			// the menu as soon as it appears.
			SetForegroundWindow(hWnd);
			TrackPopupMenu(
				hMenu,
				TPM_BOTTOMALIGN | TPM_LEFTBUTTON,  // 菜单样式
				pt.x, pt.y,
				0,
				hWnd,
				NULL
			);

			// Complete the tray-menu modal loop so a subsequent right-click can
			// open the menu normally.
			PostMessageW(hWnd, WM_NULL, 0, 0);
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
			if (wc) {
				glfwShowWindow(wc);
				glfwRestoreWindow(wc);
				glfwFocusWindow(wc);
				SetForegroundWindow(glfwGetWin32Window(wc));
			}
			break;
		case 1002:  // 点击“关闭”
			// 先移除托盘图标，再退出程序
			SaveConfigs();
			g_appState.done = true;
			RemoveTrayIcon();
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
		RemoveTrayIcon();
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
