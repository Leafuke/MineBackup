#define GLFW_EXPOSE_NATIVE_WIN32
#include <dwmapi.h>
#include "Platform_win.h"
#include "text_to_text.h"
#include "AppState.h"
#include "Globals.h"
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
#include <tuple>
#include <cctype>
#include <cstdint>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dwmapi.lib")
using namespace std;

NOTIFYICONDATA nid = { 0 };

LRESULT WINAPI HiddenWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static string ExtractLocalizedContent(const string& content) {
	size_t sepPos = content.find("---");
	if (sepPos == string::npos) {
		return content;
	}
	
	size_t beforeSep = sepPos;
	while (beforeSep > 0 && (content[beforeSep - 1] == '\n' || content[beforeSep - 1] == '\r' || content[beforeSep - 1] == ' ')) {
		beforeSep--;
	}
	
	size_t afterSep = sepPos + 3; // "---" 长度为3
	while (afterSep < content.size() && (content[afterSep] == '\n' || content[afterSep] == '\r' || content[afterSep] == ' ' || content[afterSep] == '-')) {
		afterSep++;
	}
	
	string chineseContent = content.substr(0, beforeSep);
	string englishContent = afterSep < content.size() ? content.substr(afterSep) : "";
	
	// 去除末尾空白
	while (!chineseContent.empty() && (chineseContent.back() == '\n' || chineseContent.back() == '\r' || chineseContent.back() == ' ')) {
		chineseContent.pop_back();
	}
	while (!englishContent.empty() && (englishContent.back() == '\n' || englishContent.back() == '\r' || englishContent.back() == ' ')) {
		englishContent.pop_back();
	}
	
	// 根据当前语言返回对应内容
	if (g_CurrentLang == "zh_CN") {
		return chineseContent.empty() ? content : chineseContent;
	} else {
		return englishContent.empty() ? content : englishContent;
	}
}

struct HttpResponseData {
	bool requestOk = false;
	DWORD statusCode = 0;
	string body;
};

static string ToLowerAscii(const string& value) {
	string lowered = value;
	for (char& ch : lowered) {
		ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
	}
	return lowered;
}

static string TrimAsciiWhitespace(const string& value) {
	size_t begin = 0;
	while (begin < value.size() && isspace(static_cast<unsigned char>(value[begin]))) {
		++begin;
	}
	size_t end = value.size();
	while (end > begin && isspace(static_cast<unsigned char>(value[end - 1]))) {
		--end;
	}
	return value.substr(begin, end - begin);
}

static string NormalizeNoticeText(const string& value) {
	string normalized;
	normalized.reserve(value.size());
	for (size_t i = 0; i < value.size(); ++i) {
		if (value[i] == '\r') {
			if (i + 1 < value.size() && value[i + 1] == '\n') {
				++i;
			}
			normalized.push_back('\n');
		}
		else {
			normalized.push_back(value[i]);
		}
	}
	return TrimAsciiWhitespace(normalized);
}

static bool IsLikely404Body(const string& body) {
	string lowered = ToLowerAscii(TrimAsciiWhitespace(body));
	if (lowered.empty()) return true;
	if (lowered == "404" || lowered == "404: not found" || lowered == "not found") return true;
	if (lowered.find("<title>404") != string::npos) return true;
	if (lowered.find("404 not found") != string::npos) return true;
	if (lowered.find("error 404") != string::npos) return true;
	return false;
}

static string BuildMirrorUrl(const string& directUrl) {
	const string mirrorPrefix = "https://gh-proxy.org/";
	if (directUrl.rfind(mirrorPrefix, 0) == 0) {
		return directUrl;
	}
	return mirrorPrefix + directUrl;
}

static string BuildStableContentId(const string& text) {
	// 使用稳定的 FNV-1a，避免不同运行环境下 std::hash 抖动。
	uint64_t hash = 1469598103934665603ull;
	for (unsigned char ch : text) {
		hash ^= ch;
		hash *= 1099511628211ull;
	}
	ostringstream oss;
	oss << "notice-v1-" << hex << hash;
	return oss.str();
}

static bool WinHttpFetchUrl(const wstring& url, const wchar_t* userAgent, HttpResponseData& result) {
	result = HttpResponseData{};

	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.dwSchemeLength = static_cast<DWORD>(-1);
	components.dwHostNameLength = static_cast<DWORD>(-1);
	components.dwUrlPathLength = static_cast<DWORD>(-1);
	components.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
		return false;
	}

	wstring host(components.lpszHostName, components.dwHostNameLength);
	wstring path(components.lpszUrlPath, components.dwUrlPathLength);
	if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo) {
		path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
	}
	if (path.empty()) {
		path = L"/";
	}

	HINTERNET hSession = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) return false;

	HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), components.nPort, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		return false;
	}

	DWORD requestFlags = 0;
	if (components.nScheme == INTERNET_SCHEME_HTTPS) {
		requestFlags |= WINHTTP_FLAG_SECURE;
	}

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags);
	if (!hRequest) {
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	bool ok = WinHttpSendRequest(hRequest, L"User-Agent: MineBackup-Network\r\n", -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
		&& WinHttpReceiveResponse(hRequest, NULL);
	if (!ok) {
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	result.requestOk = true;
	DWORD statusCode = 0;
	DWORD statusCodeSize = sizeof(statusCode);
	if (WinHttpQueryHeaders(hRequest,
		WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX,
		&statusCode,
		&statusCodeSize,
		WINHTTP_NO_HEADER_INDEX)) {
		result.statusCode = statusCode;
	}

	DWORD dwSize = 0;
	DWORD dwDownloaded = 0;
	do {
		dwSize = 0;
		if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
		if (dwSize == 0) break;

		vector<char> buffer(dwSize + 1, 0);
		if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
			result.body.append(buffer.data(), dwDownloaded);
		}
	} while (dwSize > 0);

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	return true;
}

static bool FetchWithMirrorFallback(const string& directUrl, const wchar_t* userAgent, bool reject404Body, string& outBody) {
	vector<string> candidates = { directUrl, BuildMirrorUrl(directUrl) };
	for (const string& candidate : candidates) {
		HttpResponseData response;
		if (!WinHttpFetchUrl(utf8_to_wstring(candidate), userAgent, response)) {
			continue;
		}
		if (!response.requestOk || response.statusCode != 200) {
			continue;
		}
		string trimmed = TrimAsciiWhitespace(response.body);
		if (trimmed.empty()) {
			continue;
		}
		if (reject404Body && IsLikely404Body(trimmed)) {
			continue;
		}
		outBody = trimmed;
		return true;
	}
	return false;
}

static tuple<int, int, int, int> ParseVersionTuple(const string& ver) {
	try {
		size_t p1 = ver.find('.');
		size_t p2 = (p1 != string::npos) ? ver.find('.', p1 + 1) : string::npos;
		size_t p3 = ver.find('-');
		if (p1 == string::npos || p2 == string::npos) return { 0, 0, 0, 0 };

		int major = stoi(ver.substr(0, p1));
		int minor = stoi(ver.substr(p1 + 1, p2 - p1 - 1));
		int patch = (p3 == string::npos) ? stoi(ver.substr(p2 + 1)) : stoi(ver.substr(p2 + 1, p3 - p2 - 1));
		int sp = 0;
		if (p3 != string::npos) {
			size_t spPos = ver.find("sp", p3);
			if (spPos != string::npos) {
				sp = stoi(ver.substr(spPos + 2));
			}
		}
		return { major, minor, patch, sp };
	}
	catch (...) {
		return { 0, 0, 0, 0 };
	}
}

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

void CheckForNoticesThread() {
	g_NoticeCheckDone = false;
	g_NewNoticeAvailable = false;
	g_NoticeContent.clear();
	g_NoticeUpdatedAt.clear();

	const string langUrl = g_CurrentLang == "zh_CN"
		? "https://raw.githubusercontent.com/Leafuke/MineBackup/develop/notice_zh"
		: "https://raw.githubusercontent.com/Leafuke/MineBackup/develop/notice_en";

	string content;
	FetchWithMirrorFallback(langUrl, L"MineBackup Notice Checker/1.1", true, content);

	if (!content.empty()) {
		string shownNotice = content;
		shownNotice = NormalizeNoticeText(shownNotice);
		if (!shownNotice.empty() && !IsLikely404Body(shownNotice)) {
			g_NoticeUpdatedAt = BuildStableContentId(shownNotice);
			if (g_NoticeUpdatedAt != g_NoticeLastSeenVersion) {
				g_NoticeContent = shownNotice;
				g_NewNoticeAvailable = true;
			}
		}
	}

	g_NoticeCheckDone = true;
}

void CheckForUpdatesThread() {
	g_UpdateCheckDone = false;
	g_NewVersionAvailable = false;
	g_LatestVersionStr.clear();
	g_ReleaseNotes.clear();

	const string apiUrl = "https://api.github.com/repos/Leafuke/MineBackup/releases/latest";
	vector<string> candidates = { apiUrl, BuildMirrorUrl(apiUrl) };

	for (const string& candidate : candidates) {
		HttpResponseData response;
		if (!WinHttpFetchUrl(utf8_to_wstring(candidate), L"MineBackup Update Checker/2.2", response)) {
			continue;
		}
		if (!response.requestOk || response.statusCode != 200) {
			continue;
		}

		string body = TrimAsciiWhitespace(response.body);
		if (body.empty() || IsLikely404Body(body)) {
			continue;
		}

		try {
			nlohmann::json parsed = nlohmann::json::parse(body);
			if (!parsed.contains("tag_name") || !parsed["tag_name"].is_string()) {
				continue;
			}

			string latestVersion = parsed["tag_name"].get<string>();
			if (!latestVersion.empty() && (latestVersion[0] == 'v' || latestVersion[0] == 'V')) {
				latestVersion = latestVersion.substr(1);
			}

			if (latestVersion.empty()) {
				continue;
			}

			bool isNew = ParseVersionTuple(latestVersion) > ParseVersionTuple(CURRENT_VERSION);
			if (isNew) {
				g_LatestVersionStr = "v" + latestVersion;
				g_NewVersionAvailable = true;

				string rawNotes;
				if (parsed.contains("body") && parsed["body"].is_string()) {
					rawNotes = parsed["body"].get<string>();
				}
				for (size_t i = 0; i + 1 < rawNotes.size(); ++i) {
					if (rawNotes[i] == '#') {
						rawNotes[i] = ' ';
					}
					else if (rawNotes[i] == '\\' && rawNotes[i + 1] == 'n') {
						rawNotes[i] = '\n';
						rawNotes[i + 1] = ' ';
					}
					else if (rawNotes[i] == '\\') {
						rawNotes[i] = ' ';
						rawNotes[i + 1] = ' ';
					}
				}
				g_ReleaseNotes = ExtractLocalizedContent(rawNotes);
			}

			break;
		}
		catch (...) {
			continue;
		}
	}

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
bool RunCommandInBackground(const wstring& command, Console& console, bool useLowPriority, const wstring& workingDirectory) {
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
	bool success = false;
	if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
		if (exit_code == 0) {
			console.AddLog(L("LOG_SUCCESS_CMD"));
			success = true;
		}
		else {
			console.AddLog(L("LOG_ERROR_CMD_FAILED"), exit_code);
			if (exit_code == 1) {
				console.AddLog(L("LOG_ERROR_CMD_FAILED_HOTBACKUP_SUGGESTION"));
				MessageBoxWin((string)L("ERROR"), (string)L("LOG_ERROR_CMD_FAILED_HOTBACKUP_SUGGESTION"), 2);
			}
			if (exit_code == 2) {
				console.AddLog(L("LOG_7Z_ERROR_SUGGESTION"));
			}
		}
	}
	else {
		console.AddLog(L("LOG_ERROR_GET_EXIT_CODE"));
	}

	// 清理句柄
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return success;
}

bool RunCommandWithResult(const wstring& command, Console& console, bool useLowPriority, int timeoutSeconds, int& exitCode, bool& timedOut, string& errorMessage, const wstring& workingDirectory) {
	exitCode = -1;
	timedOut = false;
	errorMessage.clear();

	vector<wchar_t> cmd_line(command.begin(), command.end());
	cmd_line.push_back(L'\0');

	STARTUPINFOW si = {};
	PROCESS_INFORMATION pi = {};
	si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;

	DWORD creationFlags = CREATE_NO_WINDOW;
	if (useLowPriority) {
		creationFlags |= BELOW_NORMAL_PRIORITY_CLASS;
	}

	const wchar_t* pWorkingDir = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
	console.AddLog(L("LOG_EXEC_CMD"), wstring_to_utf8(command).c_str());

	if (!CreateProcessW(NULL, cmd_line.data(), NULL, NULL, FALSE, creationFlags, NULL, pWorkingDir, &si, &pi)) {
		DWORD err = GetLastError();
		errorMessage = "CreateProcess failed: " + to_string(err);
		console.AddLog(L("LOG_ERROR_CREATE_PROCESS"), err);
		return false;
	}

	DWORD waitMs = INFINITE;
	if (timeoutSeconds > 0) {
		waitMs = static_cast<DWORD>(timeoutSeconds) * 1000U;
	}

	DWORD waitResult = WaitForSingleObject(pi.hProcess, waitMs);
	if (waitResult == WAIT_TIMEOUT) {
		timedOut = true;
		errorMessage = "Command timed out.";
		TerminateProcess(pi.hProcess, 124);
		exitCode = 124;
		console.AddLog("[Error] %s", errorMessage.c_str());
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return false;
	}

	DWORD processExitCode = 0;
	if (!GetExitCodeProcess(pi.hProcess, &processExitCode)) {
		errorMessage = "Failed to get process exit code.";
		console.AddLog(L("LOG_ERROR_GET_EXIT_CODE"));
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return false;
	}

	exitCode = static_cast<int>(processExitCode);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	if (exitCode == 0) {
		console.AddLog(L("LOG_SUCCESS_CMD"));
		return true;
	}

	console.AddLog(L("LOG_ERROR_CMD_FAILED"), exitCode);
	errorMessage = "Command failed with exit code " + to_string(exitCode) + ".";
	return false;
}
