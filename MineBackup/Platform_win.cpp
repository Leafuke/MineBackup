#include "Platform_win.h"
#include "text_to_text.h"
#include "AppState.h"
#include "resource.h"
#include "i18n.h"
#include "Console.h"
#include "ConfigManager.h"
#include "json.hpp"
#include <GLFW/glfw3.h>
#include <filesystem>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
using namespace std;

extern GLFWwindow* wc;

extern atomic<bool> g_UpdateCheckDone;
extern atomic<bool> g_NewVersionAvailable;
extern string g_LatestVersionStr;
extern string g_ReleaseNotes;
extern string CURRENT_VERSION;

NOTIFYICONDATA nid = { 0 };

LRESULT WINAPI HiddenWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


void SetFileAttributesWin(const wstring& path, bool isHidden) {
	if(isHidden)
		SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN);
	else
		SetFileAttributes(path.c_str(), FILE_ATTRIBUTE_NORMAL);
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
		g_CurrentLang = "en_US"; // Ĭ��Ӣ��
		break;
	}
	return;
}

void MessageBoxWin(const string& title, const string& message) {
	MessageBoxW(nullptr, utf8_to_wstring(L(message.c_str())).c_str(), utf8_to_wstring(L(title.c_str())).c_str(), MB_OK | MB_ICONERROR);
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
		return NULL; // ����ʧ��
	return hwnd_hidden;
}

void RegisterHotkeys(HWND hwnd) {
	// ע���ȼ�������ͼ��
	RegisterHotKey(hwnd, MINEBACKUP_HOTKEY_ID, MOD_ALT | MOD_CONTROL, 'S');
	RegisterHotKey(hwnd, MINERESTORE_HOTKEY_ID, MOD_ALT | MOD_CONTROL, 'Z');
}
void UnregisterHotkeys(HWND hwnd) {
	::UnregisterHotKey(hwnd, MINEBACKUP_HOTKEY_ID);
	::UnregisterHotKey(hwnd, MINERESTORE_HOTKEY_ID);
}
void CreateTrayIcon(HWND hwnd, HINSTANCE hInstance) {
	// ��ʼ������ͼ�� (nid)
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
	case WM_USER + 1: // ����ͼ����Ϣ
		if (lParam == WM_LBUTTONUP) {
			g_appState.showMainApp = true;
			glfwShowWindow(wc);
		}
		else if (lParam == WM_RBUTTONUP) {
			HMENU hMenu = CreatePopupMenu();
			AppendMenu(hMenu, MF_STRING, 1001, utf8_to_wstring((string)L("OPEN")).c_str());
			AppendMenu(hMenu, MF_STRING, 1002, utf8_to_wstring((string)L("EXIT")).c_str());

			// ��ȡ���λ�ã��˵���ʾ������Ҽ������λ�ã�
			POINT pt;
			GetCursorPos(&pt);

			// ��ʾ�˵���TPM_BOTTOMALIGN���˵��ײ��������λ�ã�
			TrackPopupMenu(
				hMenu,
				TPM_BOTTOMALIGN | TPM_LEFTBUTTON,  // �˵���ʽ
				pt.x, pt.y,
				0,
				hWnd,
				NULL
			);

			// ������ô˺���������˵������޷������ر�
			SetForegroundWindow(hWnd);
			// ���ٲ˵��������ڴ�й©��
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
		case 1001:  // ������򿪽��桱
			g_appState.showMainApp = true;
			glfwShowWindow(wc);
			SetForegroundWindow(hWnd);
			break;
		case 1002:  // ������رա�
			// ���Ƴ�����ͼ�꣬���˳�����
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
		Shell_NotifyIcon(NIM_DELETE, &nid);  // ��������ͼ��
		g_appState.done = true;
		::PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}




void CheckForUpdatesThread() {
	DWORD dwSize = 0;
	DWORD dwDownloaded = 0;
	LPSTR pszOutBuffer;
	string responseBody;
	BOOL bResults = FALSE;
	HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

	hSession = WinHttpOpen(L"MineBackup Update Checker/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) goto cleanup;

	hConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) goto cleanup;

	hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/repos/Leafuke/MineBackup/releases/latest", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) goto cleanup;

	WinHttpSendRequest(hRequest, L"User-Agent: MineBackup-Update-Checker\r\n", -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

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
		// ʹ�ø��ɿ��� JSON ������
		string latestVersion = nlohmann::json::parse(responseBody)["tag_name"].get<std::string>();
		// �Ƴ��汾��ǰ�� 'v'
		if (!latestVersion.empty() && (latestVersion[0] == 'v' || latestVersion[0] == 'V')) {
			latestVersion = latestVersion.substr(1);
		}
		short pos1 = latestVersion.find('.');
		short pos2 = latestVersion.find('.', pos1 + 1);
		short pos3 = latestVersion.find('-');
		short pos11 = CURRENT_VERSION.find('.');
		short pos22 = CURRENT_VERSION.find('.', pos1 + 1);
		short pos33 = CURRENT_VERSION.find('-');
		bool isNew = false;
		// ��������ֵ����ֵ
		short curMajor = stoi(CURRENT_VERSION.substr(0, pos11)), curMinor1 = stoi(CURRENT_VERSION.substr(pos11 + 1, pos22)), newMajor = stoi(latestVersion.substr(0, pos1)), newMinor1 = stoi(latestVersion.substr(pos1 + 1, pos2));
		short curMinor2 = pos33 == string::npos ? stoi(CURRENT_VERSION.substr(pos22 + 1)) : stoi(CURRENT_VERSION.substr(pos22 + 1, pos33)), newMinor2 = pos3 == string::npos ? stoi(latestVersion.substr(pos2 + 1)) : stoi(latestVersion.substr(pos2 + 1, pos3));
		short curSp = pos33 == string::npos ? 0 : stoi(CURRENT_VERSION.substr(pos33 + 3)), newSp = pos3 == string::npos ? 0 : stoi(latestVersion.substr(pos3 + 3));
		// ���⼸�ְ汾�� v1.7.9 v1.7.10 v1.7.9-sp1
		// ��һ����д�÷ǳ��ǳ������⣬���ǡ��������Ű�


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

		// �򵥰汾�Ƚ� (���� "1.7.0" > "1.6.7")
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
			// ���� .exe ��������  -- ֱ���ֶ�ƴ�Ӿ���
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
			//			break; // �ҵ����˳�
			//		}
			//		search_pos = asset_obj_end;
			//	}
			//}
		}
	}
	catch (...) {
		// ����ʧ�ܣ���Ĭ����
	}

cleanup:
	if (hRequest) WinHttpCloseHandle(hRequest);
	if (hConnect) WinHttpCloseHandle(hConnect);
	if (hSession) WinHttpCloseHandle(hSession);
	g_UpdateCheckDone = true;
}