#define GLFW_EXPOSE_NATIVE_WIN32
#include <dwmapi.h>
#include "Platform_win.h"
#include "text_to_text.h"
#include "AppState.h"
#include "resource.h"
#include "i18n.h"
#include "Console.h"
#include "ConfigManager.h"
#include "json.hpp"
#include <shobjidl.h>
#include <shlobj.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <filesystem>
#include <chrono>
#include <winhttp.h>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <wchar.h>
#include <functional>
#include <vector>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dwmapi.lib")
using namespace std;

extern GLFWwindow* wc;

extern atomic<bool> g_UpdateCheckDone;
extern atomic<bool> g_NewVersionAvailable;
extern atomic<bool> g_NoticeCheckDone;
extern atomic<bool> g_NewNoticeAvailable;
extern string g_LatestVersionStr;
extern string g_ReleaseNotes;
extern string g_NoticeContent;
extern string g_NoticeUpdatedAt;
extern string g_NoticeLastSeenVersion;
extern string CURRENT_VERSION;
extern int g_hotKeyBackupId, g_hotKeyRestoreId;

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
		g_CurrentLang = "zh_CN";
		break;
	case LANG_CHINESE_TRADITIONAL:
		g_CurrentLang = "zh_CN";
		break;
	case LANG_ENGLISH:
		g_CurrentLang = "en_US";
		break;
	default:
		g_CurrentLang = "en_US"; // 默认英语
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
	wcscpy_s(nid.szTip, L"MineBackup");
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
	//HANDLE hFile = CreateFile(filePath.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_SHARE_READ, NULL)
	if (hFile == INVALID_HANDLE_VALUE) {
		return true;
		if (GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_LOCK_VIOLATION) {
			return true;
		}
	}
	if (hFile != INVALID_HANDLE_VALUE) {
		CloseHandle(hFile);
	}
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





void CheckForNoticesThread() {
	g_NoticeCheckDone = false;
	g_NewNoticeAvailable = false;
	g_NoticeContent.clear();
	g_NoticeUpdatedAt.clear();

	HINTERNET hSession = nullptr, hConnect = nullptr, hRequest = nullptr;
	DWORD dwSize = 0;
	DWORD dwDownloaded = 0;
	LPSTR pszOutBuffer = nullptr;
	string responseBody;
	bool success = false;

	hSession = WinHttpOpen(L"MineBackup Notice Checker/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) goto cleanup;

	hConnect = WinHttpConnect(hSession, L"raw.githubusercontent.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) goto cleanup;

	hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/Leafuke/MineBackup/develop/notice", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) goto cleanup;

	WinHttpSendRequest(hRequest, L"User-Agent: MineBackup-Notice-Checker\r\n", -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

	if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;

	do {
		dwSize = 0;
		if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
		if (dwSize == 0) break;
		pszOutBuffer = new char[dwSize + 1];
		ZeroMemory(pszOutBuffer, dwSize + 1);
		if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
			responseBody.append(pszOutBuffer, dwDownloaded);
		}
		delete[] pszOutBuffer;
	} while (dwSize > 0);

	if (!responseBody.empty()) {
		wstring lastModified;
		dwSize = 0;
		if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LAST_MODIFIED, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &dwSize, WINHTTP_NO_HEADER_INDEX)) {
			if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && dwSize > 0) {
				vector<wchar_t> headerBuf(dwSize / sizeof(wchar_t));
				if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LAST_MODIFIED, WINHTTP_HEADER_NAME_BY_INDEX, headerBuf.data(), &dwSize, WINHTTP_NO_HEADER_INDEX)) {
					lastModified.assign(headerBuf.data());
				}
			}
		}

		g_NoticeUpdatedAt = lastModified.empty() ? to_string(hash<string>{}(responseBody)) : wstring_to_utf8(lastModified);

		if (g_NoticeUpdatedAt != g_NoticeLastSeenVersion) {
			g_NoticeContent = responseBody;
			g_NewNoticeAvailable = true;
		}
		success = true;
	}

cleanup:
	if (hRequest) WinHttpCloseHandle(hRequest);
	if (hConnect) WinHttpCloseHandle(hConnect);
	if (hSession) WinHttpCloseHandle(hSession);

	if (!success) {
		g_NewNoticeAvailable = false;
	}
	g_NoticeCheckDone = true;
}

void CheckForUpdatesThread() {
	DWORD dwSize = 0;
	DWORD dwDownloaded = 0;
	LPSTR pszOutBuffer;
	string responseBody;
	BOOL bResults = FALSE;
	HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

	hSession = WinHttpOpen(L"MineBackup Update Checker/2.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) goto cleanup;

	hConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) goto cleanup;

	hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/repos/Leafuke/MineBackup/releases/latest", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) goto cleanup;

	WinHttpSendRequest(hRequest, L"User-Agent: MineBackup-Update-Checker\r\n", -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

	//WinHttpSendRequest(hRequest, L"User-Agent: MineBackup-Update-Checker\r\n", (DWORD)(wcslen(L"User-Agent: MineBackup-Update-Checker\r\n") * sizeof(wchar_t)), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

	bResults = WinHttpReceiveResponse(hRequest, NULL);
	if (!bResults) goto cleanup;

	do {
		dwSize = 0;
		if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
		if (dwSize == 0) break;
		pszOutBuffer = new char[dwSize + 1];
		ZeroMemory(pszOutBuffer, dwSize + 1);
		if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded))
			responseBody.append(pszOutBuffer, dwDownloaded);
		delete[] pszOutBuffer;
	} while (dwSize > 0);

	try {
		//string latestVersion = find_json_value(responseBody, "tag_name");
		// 使用更可靠的 JSON 解析库
		string latestVersion = nlohmann::json::parse(responseBody)["tag_name"].get<std::string>();
		// 移除版本号前的 'v'
		if (!latestVersion.empty() && (latestVersion[0] == 'v' || latestVersion[0] == 'V')) {
			latestVersion = latestVersion.substr(1);
		}
		size_t pos1 = latestVersion.find('.');
		size_t pos2 = latestVersion.find('.', pos1 + 1);
		size_t pos3 = latestVersion.find('-');
		size_t pos11 = CURRENT_VERSION.find('.');
		size_t pos22 = CURRENT_VERSION.find('.', pos1 + 1);
		size_t pos33 = CURRENT_VERSION.find('-');
		bool isNew = false;
		// 把所有数值都赋值
		size_t curMajor = stoi(CURRENT_VERSION.substr(0, pos11)), curMinor1 = stoi(CURRENT_VERSION.substr(pos11 + 1, pos22)), newMajor = stoi(latestVersion.substr(0, pos1)), newMinor1 = stoi(latestVersion.substr(pos1 + 1, pos2));
		size_t curMinor2 = pos33 == string::npos ? stoi(CURRENT_VERSION.substr(pos22 + 1)) : stoi(CURRENT_VERSION.substr(pos22 + 1, pos33)), newMinor2 = pos3 == string::npos ? stoi(latestVersion.substr(pos2 + 1)) : stoi(latestVersion.substr(pos2 + 1, pos3));
		size_t curSp = pos33 == string::npos ? 0 : stoi(CURRENT_VERSION.substr(pos33 + 3)), newSp = pos3 == string::npos ? 0 : stoi(latestVersion.substr(pos3 + 3));
		// 有这几种版本号 v1.7.9 v1.7.10 v1.7.9-sp1
		// 这一段我写得非常非常不满意，但是……将就着吧


		if (newMajor > curMajor) {
			isNew = true;
		}
		else if (newMajor == curMajor) {
			if (newMinor1 > curMinor1) {
				isNew = true;
			}
			else if (newMinor1 == curMinor1) {
				if (newMinor2 > curMinor2) {
					isNew = true;
				}
				else if (newMinor2 == curMinor2) {
					if (newSp > curSp) {
						isNew = true;
					}
				}
			}
		}

		// 简单版本比较 (例如 "1.7.0" > "1.6.7")
		if (!latestVersion.empty() && isNew) {
			g_LatestVersionStr = "v" + latestVersion;
			g_NewVersionAvailable = true;
			g_ReleaseNotes = nlohmann::json::parse(responseBody)["body"].get<std::string>();;
			for (int i = 0; i < g_ReleaseNotes.size() - 1; ++i)
			{
				if (g_ReleaseNotes[i] == '#')
					g_ReleaseNotes[i] = ' ';
				else if (g_ReleaseNotes[i] == '\\' && g_ReleaseNotes[i + 1] == 'n')
					g_ReleaseNotes[i] = '\n', g_ReleaseNotes[i + 1] = ' ';
				else if (g_ReleaseNotes[i] == '\\')
					g_ReleaseNotes[i] = ' ', g_ReleaseNotes[i + 1] = ' ';
			}
			// 查找 .exe 下载链接  -- 直接手动拼接就行
			//string assets_key = "\"assets\": [";
			//size_t assets_start = responseBody.find(assets_key);
			//if (assets_start != string::npos) {
			//	size_t search_pos = assets_start + assets_key.length();
			//	while (search_pos < responseBody.length()) {
			//		size_t asset_obj_start = responseBody.find("{", search_pos);
			//		if (asset_obj_start == string::npos) break;
			//		size_t asset_obj_end = responseBody.find("}", asset_obj_start);
			//		if (asset_obj_end == string::npos) break;

			//		string asset_json = responseBody.substr(asset_obj_start, asset_obj_end - asset_obj_start);
			//		string asset_name = find_json_value(asset_json, "name");

			//		if (asset_name.size() > 4 && asset_name.substr(asset_name.size() - 4) == ".exe") {
			//			g_AssetDownloadURL = find_json_value(asset_json, "browser_download_url");
			//			break; // 找到即退出
			//		}
			//		search_pos = asset_obj_end;
			//	}
			//}
		}
	}
	catch (...) {
		// 解析失败，静默处理
	}

cleanup:
	if (hRequest) WinHttpCloseHandle(hRequest);
	if (hConnect) WinHttpCloseHandle(hConnect);
	if (hSession) WinHttpCloseHandle(hSession);
	g_UpdateCheckDone = true;
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
void SetAutoStart(const string& appName, const wstring& appPath, bool configType, int& configId, bool& enable) {
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


static std::string g_logFilePath = "auto_log.txt";

void SetLogFilePath(const std::string& path) {
    g_logFilePath = path;
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
    std::ofstream log_file(g_logFilePath, std::ios::app);
    if (!log_file.is_open()) return;
    std::string level_str;
    switch (level) {
        case LogLevel::Info: level_str = "[INFO]"; break;
        case LogLevel::Warning: level_str = "[WARN]"; break;
        case LogLevel::Error: level_str = "[ERROR]"; break;
        default: level_str = "[INFO]"; break;
    }
    log_file << GetCurrentTimestamp() << " " << level_str << " " << message << std::endl;
    log_file.close();
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

bool Extract7zToTempFile(wstring& extractedPath) {


	// 构造目标路径：文档\7z.exe
	wstring finalPath = GetDocumentsPath();

	if (finalPath.back() != L'\\') finalPath += L'\\';
	finalPath += L"7z.exe";

	if (filesystem::exists(finalPath)) {
		extractedPath = finalPath;
		return true;
	}

	// 用主模块句柄
	HRSRC hRes = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDR_EXE1), L"EXE");
	if (!hRes) return false;

	HGLOBAL hData = LoadResource(GetModuleHandleW(NULL), hRes);
	if (!hData) return false;

	DWORD dataSize = SizeofResource(GetModuleHandleW(NULL), hRes);
	if (dataSize == 0) return false;

	LPVOID pData = LockResource(hData);
	if (!pData) return false;

	HANDLE hFile = CreateFileW(finalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) return false;

	DWORD bytesWritten;
	BOOL ok = WriteFile(hFile, pData, dataSize, &bytesWritten, nullptr);
	CloseHandle(hFile);
	if (!ok || bytesWritten != dataSize) {
		DeleteFileW(finalPath.c_str());
		return false;
	}

	extractedPath = finalPath;
	return true;

	/*wchar_t tempFile[MAX_PATH];
	if (!GetTempFileNameW(tempPath, L"7z", 0, tempFile)) return false;

	// 随机名称，其实没必要
	std::wstring finalPath = tempFile;
	finalPath += L".exe";
	MoveFileW(tempFile, finalPath.c_str());*/
}

bool ExtractFontToTempFile(wstring& extractedPath) {

	wstring finalPath = GetDocumentsPath();

	if (finalPath.back() != L'\\') finalPath += L'\\';
	finalPath += L"fontawesome - sp.otf";

	if (filesystem::exists(finalPath)) {
		extractedPath = finalPath;
		return true;
	}

	HRSRC hRes = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDR_FONTS1), L"FONTS");
	if (!hRes) return false;

	HGLOBAL hData = LoadResource(GetModuleHandleW(NULL), hRes);
	if (!hData) return false;

	DWORD dataSize = SizeofResource(GetModuleHandleW(NULL), hRes);
	if (dataSize == 0) return false;

	LPVOID pData = LockResource(hData);
	if (!pData) return false;

	HANDLE hFile = CreateFileW(finalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) return false;

	DWORD bytesWritten;
	BOOL ok = WriteFile(hFile, pData, dataSize, &bytesWritten, nullptr);
	CloseHandle(hFile);
	if (!ok || bytesWritten != dataSize) {
		DeleteFileW(finalPath.c_str());
		return false;
	}

	extractedPath = finalPath;
	return true;
}

//在后台静默执行一个命令行程序（如7z.exe），并等待其完成。
//这是实现备份和还原功能的核心，避免了GUI卡顿和黑窗口弹出。
// 参数:
//   - command: 要执行的完整命令行（宽字符）。
//   - console: 监控台对象的引用，用于输出日志信息。
bool RunCommandInBackground(wstring command, Console& console, bool useLowPriority, const wstring& workingDirectory) {
	// CreateProcessW需要一个可写的C-style字符串，所以我们将wstring复制到vector<wchar_t>
	vector<wchar_t> cmd_line(command.begin(), command.end());
	cmd_line.push_back(L'\0'); // 添加字符串结束符

	STARTUPINFOW si = {};
	PROCESS_INFORMATION pi = {};
	si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE; // 隐藏子进程的窗口

	DWORD creationFlags = CREATE_NO_WINDOW;
	if (useLowPriority) {
		creationFlags |= BELOW_NORMAL_PRIORITY_CLASS;
	}

	// 开始创建进程
	const wchar_t* pWorkingDir = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
	console.AddLog(L("LOG_EXEC_CMD"), wstring_to_utf8(command).c_str());

	if (!CreateProcessW(NULL, cmd_line.data(), NULL, NULL, FALSE, creationFlags, NULL, pWorkingDir, &si, &pi)) {
		console.AddLog(L("LOG_ERROR_CREATE_PROCESS"), GetLastError());
		return false;
	}

	// 等待子进程执行完毕
	WaitForSingleObject(pi.hProcess, INFINITE);

	// 检查子进程的退出代码
	DWORD exit_code;
	if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
		if (exit_code == 0) {
			console.AddLog(L("LOG_SUCCESS_CMD"));
		}
		else {
			console.AddLog(L("LOG_ERROR_CMD_FAILED"), exit_code);
			if (exit_code == 1) {
				console.AddLog(L("LOG_ERROR_CMD_FAILED_HOTBACKUP_SUGGESTION"));
				MessageBoxWin((string)L("ERROR"), (string)L("LOG_ERROR_CMD_FAILED_HOTBACKUP_SUGGESTION"), 2);
			}
			if (exit_code == 2) // 致命错误
				console.AddLog(L("LOG_7Z_ERROR_SUGGESTION"));
		}
	}
	else {
		console.AddLog(L("LOG_ERROR_GET_EXIT_CODE"));
	}

	// 清理句柄
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return true;
}