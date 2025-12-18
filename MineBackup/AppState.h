#pragma once
#ifndef APP_STATE_H
#define APP_STATE_H
#include <vector>
#include <string>
#include <map>
#include <atomic>
#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>
#include <ctime>
#include <cwchar>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <limits.h>
#include <sys/stat.h>
#include <cerrno>
#include <filesystem>
#ifndef CONSTANT1
#define CONSTANT1 256
#define CONSTANT2 512
#define MINEBACKUP_HOTKEY_ID 1
#define MINERESTORE_HOTKEY_ID 2
#endif

#ifndef _WIN32
// 模仿 Windows 平台的部分函数行为
inline int localtime_s(struct tm* _Tm, const time_t* _Time) {
	return localtime_r(_Time, _Tm) ? 0 : -1;
}
inline void Sleep(unsigned long milliseconds) {
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// 为 Linux 平台重新定义一些别名...
using DWORD = unsigned long;
using BOOL = int;
using HINSTANCE = void*;
using HWND = void*;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using errno_t = int;
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef SW_SHOWNORMAL
#define SW_SHOWNORMAL 1
#endif

inline errno_t strcpy_s(char* dest, size_t destsz, const char* src) {
	if (!dest || !src || destsz == 0) return EINVAL;
	std::strncpy(dest, src, destsz - 1);
	dest[destsz - 1] = '\0';
	return 0;
}

inline errno_t strncpy_s(char* dest, size_t destsz, const char* src) {
	if (!dest || !src || destsz == 0) return EINVAL;
	std::strncpy(dest, src, destsz - 1);
	dest[destsz - 1] = '\0';
	return 0;
}

inline errno_t strncpy_s(char* dest, const char* src, size_t destsz) {
	return strncpy_s(dest, destsz, src);
}

template <size_t N>
inline errno_t strncpy_s(char (&dest)[N], const char* src) {
	return strncpy_s(dest, N, src);
}

template <size_t N>
inline errno_t strcpy_s(char (&dest)[N], const char* src) {
	return strncpy_s(dest, N, src);
}

inline errno_t strcpy_s(char* dest, const char* src) {
	if (!dest || !src) return EINVAL;
	std::strncpy(dest, src, std::strlen(src));
	return 0;
}

inline errno_t sprintf_s(char* dest, size_t destsz, const char* fmt, ...) {
	if (!dest || !fmt || destsz == 0) return EINVAL;
	va_list args;
	va_start(args, fmt);
	int ret = vsnprintf(dest, destsz, fmt, args);
	va_end(args);
	if (ret < 0 || static_cast<size_t>(ret) >= destsz) {
		dest[destsz - 1] = '\0';
		return ERANGE;
	}
	return 0;
}

template <size_t N>
inline errno_t sprintf_s(char (&dest)[N], const char* fmt, ...) {
	if (!fmt) return EINVAL;
	va_list args;
	va_start(args, fmt);
	int ret = vsnprintf(dest, N, fmt, args);
	va_end(args);
	if (ret < 0 || static_cast<size_t>(ret) >= N) {
		dest[N - 1] = '\0';
		return ERANGE;
	}
	return 0;
}

inline int swprintf_s(wchar_t* buffer, size_t bufsz, const wchar_t* fmt, ...) {
	if (!buffer || !fmt || bufsz == 0) return EINVAL;
	va_list args;
	va_start(args, fmt);
	int ret = vswprintf(buffer, bufsz, fmt, args);
	va_end(args);
	if (ret < 0 || static_cast<size_t>(ret) >= bufsz) {
		buffer[bufsz - 1] = L'\0';
		return ERANGE;
	}
	return ret;
}

inline errno_t ctime_s(char* buffer, size_t bufsz, const time_t* t) {
	if (!buffer || !t || bufsz == 0) return EINVAL;
	return ctime_r(t, buffer) ? 0 : errno;
}

inline errno_t _dupenv_s(char** buffer, size_t*, const char* name) {
	const char* v = std::getenv(name);
	if (!v) { if (buffer) *buffer = nullptr; return 0; }
	size_t len = std::strlen(v);
	char* tmp = static_cast<char*>(std::malloc(len + 1));
	if (!tmp) return ENOMEM;
	std::memcpy(tmp, v, len + 1);
	if (buffer) *buffer = tmp;
	return 0;
}

inline int _wcsicmp(const wchar_t* a, const wchar_t* b) {
	return ::wcscasecmp(a, b);
}

inline int _kbhit() { return 0; }

inline DWORD GetModuleFileNameW(HINSTANCE, wchar_t* buffer, DWORD size) {
	if (!buffer || size == 0) return 0;
	char path[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (len == -1) return 0;
	path[len] = '\0';
	std::mbstate_t state{};
	const char* src = path;
	std::mbsrtowcs(buffer, &src, size - 1, &state);
	buffer[size - 1] = L'\0';
	return static_cast<DWORD>(std::wcslen(buffer));
}

inline DWORD GetCurrentDirectoryW(DWORD size, wchar_t* buffer) {
	if (!buffer || size == 0) return 0;
	char path[PATH_MAX];
	if (!getcwd(path, sizeof(path))) return 0;
	std::mbstate_t state{};
	const char* src = path;
	std::mbsrtowcs(buffer, &src, size - 1, &state);
	buffer[size - 1] = L'\0';
	return static_cast<DWORD>(std::wcslen(buffer));
}

inline BOOL CopyFileW(const wchar_t* existing, const wchar_t* newfile, BOOL) {
	try {
		std::filesystem::copy_file(existing, newfile, std::filesystem::copy_options::overwrite_existing);
		return TRUE;
	}
	catch (...) {
		return FALSE;
	}
}

inline void ShellExecuteW(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, int) {
	// No-op on Linux.
}

#endif

// 结构体们
struct Config {
	std::wstring saveRoot;
	std::vector<std::pair<std::wstring, std::wstring>> worlds; // {name, desc}
	std::wstring backupPath;
	std::wstring zipPath;
	std::wstring zipFormat = L"7z";
	std::wstring fontPath;
	std::wstring zipMethod = L"LZMA2";
	int backupMode = 1;
	int zipLevel = 5;
	int keepCount = 0;
	bool hotBackup = true; // 1.11.3 之后默认开启热备份
	bool backupBefore = false;
	int theme = 1;
	int folderNameType = 0;
	std::string name;
	int cpuThreads = 0;
	bool useLowPriority = false;
	bool skipIfUnchanged = true;
	int maxSmartBackupsPerFull = 5;
	bool backupOnGameStart = false;
	std::vector<std::wstring> blacklist;
	bool cloudSyncEnabled = false;
	std::wstring rclonePath;
	std::wstring rcloneRemotePath;
	std::wstring snapshotPath;
	std::wstring othersPath;
	bool enableWEIntegration = false;
	std::wstring weSnapshotPath = L"";
};
struct AutomatedTask {
	int configIndex = -1;
	int worldIndex = -1;
	int backupType = 0; // 0: 单次, 1: 间隔, 2: 计划
	int intervalMinutes = 15;
	int schedMonth = 0, schedDay = 0, schedHour = 0, schedMinute = 0; // 0 意味着“每一”
};
struct SpecialConfig {
	bool autoExecute = false;
	std::vector<std::wstring> commands;
	std::vector<AutomatedTask> tasks;
	bool exitAfterExecution = false;
	std::string name;
	int zipLevel = 5;
	int keepCount = 0;
	int cpuThreads = 0;
	int theme = 1;
	bool useLowPriority = true;
	bool hotBackup = false;
	std::vector<std::wstring> blacklist;
	bool runOnStartup = false;
	bool hideWindow = false;
	bool backupOnGameStart = false;
};
struct HistoryEntry {
	std::wstring timestamp_str;
	std::wstring worldName;
	std::wstring backupFile;
	std::wstring backupType;
	std::wstring comment;
	bool isImportant = false;
};
struct AutoBackupTask {
	std::thread worker;
	std::atomic<bool> stop_flag{ false }; // 原子布尔值，用于安全地通知线程停止
};
struct DisplayWorld { // 一个新的结构体，让 UI 不再直接读取 configs[currentConfigIndex].worlds，而使用 DisplayWorld
	std::wstring name;      // 世界名（文件夹名）
	std::wstring desc;      // 描述
	int baseConfigIndex = -1; // 来源配置 id
	int baseWorldIndex = -1;  // 来源配置中世界索引
	Config effectiveConfig;   // 合并后的配置（拷贝）
};
struct MyFolder {
	std::wstring path;		// 世界文件夹路径
	std::wstring name;		// 世界名（文件夹名）
	std::wstring desc;		// 描述
	Config config;			// 所属配置
	int configIndex = -1;	// 所属配置索引
	int worldIndex = -1;	// 世界索引
};

enum class HotRestoreState {
	IDLE,              // 空闲状态
	WAITING_FOR_MOD,   // 已发送请求，正在等待模组响应
	RESTORING,         // 模组已响应，正在执行还原
};

//struct WorldStateCache {
//	std::wstring lastOpenTime;
//	std::wstring lastBackupTime;
//	bool needsBackup;
//	std::filesystem::file_time_type lastChecked;
//};

struct AppState {

	bool done = false;

    // UI State
    bool showMainApp = false;
	bool specialConfigMode = false; // 用来开启简单UI


    // Data
	int currentConfigIndex = 1, realConfigIndex = -1; // 如果realConfigIndex不为-1，说明是特殊配置
	std::map<int, Config> configs;
	std::map<int, std::vector<HistoryEntry>> g_history;
	std::map<int, SpecialConfig> specialConfigs;

	std::map<std::pair<int, int>, AutoBackupTask> g_active_auto_backups; // Key: {configIdx, worldIdx}
	//std::map<std::pair<int, int>, WorldStateCache> worldCache; // Key: {configIdx, worldIdx}



	std::mutex configsMutex;			// 用于保护全局配置的互斥锁
	std::mutex task_mutex;		// 专门用于保护 g_active_auto_backups
	bool isRespond = false;
	std::atomic<HotRestoreState> hotkeyRestoreState = HotRestoreState::IDLE;
    // Settings
    bool g_CheckForUpdates = true;
};

// Declare a single global instance
extern AppState g_appState;
#endif