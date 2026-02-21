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
#include <termios.h>
#include <sys/select.h>
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
#if defined(__APPLE__) && defined(__OBJC__)
#include <objc/objc.h>
#else
using BOOL = int;
#endif
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
	std::size_t srclen = std::strlen(src);
	if (srclen >= destsz) {
		dest[0] = '\0';
		return ERANGE;
	}
	std::memcpy(dest, src, srclen + 1);
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
	return strcpy_s(dest, N, src);
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

inline termios& _mb_saved_termios() {
	static termios saved{};
	return saved;
}
inline bool& _mb_termios_saved() {
	static bool saved = false;
	return saved;
}
inline void _mb_restore_termios() {
	if (_mb_termios_saved()) {
		tcsetattr(STDIN_FILENO, TCSANOW, &_mb_saved_termios());
	}
}
inline int _kbhit() {
	static bool initialized = false;
	static termios newt;
	if (!initialized) {
		termios oldt;
		tcgetattr(STDIN_FILENO, &oldt);
		_mb_saved_termios() = oldt;
		_mb_termios_saved() = true;
		newt = oldt;
		newt.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
		tcsetattr(STDIN_FILENO, TCSANOW, &newt);
		setbuf(stdin, NULL);
		std::atexit(_mb_restore_termios);
		initialized = true;
	}
	fd_set set;
	FD_ZERO(&set);
	FD_SET(STDIN_FILENO, &set);
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	int res = select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv);
	return res > 0;
}

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
#ifdef _WIN32
	bool hotBackup = true; // 1.11.3 之后默认开启热备份 (Windows)
#else
	bool hotBackup = false; // Linux/MacOS 默认关闭热备份，因为游戏不锁定文件
#endif
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
// 新的统一任务系统（v2）
enum class TaskTypeV2 {
	Backup,     // 备份任务
	Command,    // CMD命令任务
	Script      // 脚本任务
};

enum class TaskExecMode {
	Sequential,    // 顺序执行（等待上一个任务完成）
	Parallel       // 并行执行（和上一个任务同时进行）
};

enum class TaskTrigger {
	Once,          // 单次执行
	Interval,      // 间隔执行
	Scheduled      // 计划执行
};

struct UnifiedTaskV2 {
	int id = 0;
	std::string name;
	TaskTypeV2 type = TaskTypeV2::Backup;
	TaskExecMode executionMode = TaskExecMode::Sequential;
	TaskTrigger triggerMode = TaskTrigger::Once;
	bool enabled = true;

	// 备份任务相关
	int configIndex = -1;
	int worldIndex = -1;

	// CMD命令相关
	std::wstring command;
	std::wstring workingDirectory;

	// 计划相关
	int intervalMinutes = 15;
	int schedMonth = 0, schedDay = 0, schedHour = 0, schedMinute = 0;

	// 高级选项
	int retryCount = 0;
	int timeoutMinutes = 0;
	bool notifyOnComplete = false;
	bool notifyOnError = true;
};

// 服务模式配置
struct ServiceConfig {
	bool installAsService = false;
	std::wstring serviceName = L"MineBackupService";
	std::wstring serviceDisplayName = L"MineBackup Auto Backup Service";
	std::wstring serviceDescription = L"Automated backup service for Minecraft worlds";
	bool startWithSystem = true;
	bool delayedStart = false;
};

struct SpecialConfig {
	bool autoExecute = false;
	std::vector<std::wstring> commands;              // 旧版兼容：命令列表
	std::vector<AutomatedTask> tasks;                // 旧版兼容：任务列表
	std::vector<UnifiedTaskV2> unifiedTasks;         // 新版统一任务系统
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
	
	// Windows服务模式
	ServiceConfig serviceConfig;
	bool useServiceMode = false;                     // 是否使用服务模式
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

// KnotLink 联动模组状态信息
// 用于跟踪联动模组的检测结果和通信状态
struct KnotLinkModInfo {
	std::atomic<bool> modDetected{false};       // 是否检测到联动模组
	std::string modVersion;                      // 模组版本号
	std::atomic<bool> versionCompatible{false}; // 模组版本是否兼容

	// 最低要求的模组版本号
	static constexpr const char* MIN_MOD_VERSION = "1.0.0";

	// 异步响应同步机制
	std::mutex mtx;
	std::condition_variable cv;

	// 响应标志 (受 mtx 保护)
	bool handshakeReceived = false;            // 收到握手响应
	bool worldSaveComplete = false;            // 模组已完成世界保存 (用于热备份)
	bool worldSaveAndExitComplete = false;     // 模组已完成世界保存并退出 (用于热还原)
	bool rejoinResponseReceived = false;       // 收到重进世界结果
	bool rejoinSuccess = false;                // 重进世界是否成功

	// 重置单次操作的标志 (在每次操作前调用)
	void resetForOperation() {
		std::lock_guard<std::mutex> lock(mtx);
		handshakeReceived = false;
		worldSaveComplete = false;
		worldSaveAndExitComplete = false;
		rejoinResponseReceived = false;
		rejoinSuccess = false;
	}

	// 完全重置
	void resetDetection() {
		modDetected = false;
		modVersion.clear();
		versionCompatible = false;
		resetForOperation();
	}

	// 版本比较
	static bool IsVersionCompatible(const std::string& current, const std::string& required) {
		auto parseVersion = [](const std::string& v) -> std::tuple<int, int, int> {
			int major = 0, minor = 0, patch = 0;
			std::sscanf(v.c_str(), "%d.%d.%d", &major, &minor, &patch);
			return { major, minor, patch };
		};
		return parseVersion(current) >= parseVersion(required);
	}

	// 通知指定标志并唤醒等待线程
	void notifyFlag(bool KnotLinkModInfo::* flag, bool value = true) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			this->*flag = value;
		}
		cv.notify_all();
	}

	// 等待指定标志变为 true，带超时
	bool waitForFlag(bool KnotLinkModInfo::* flag, std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> lock(mtx);
		return cv.wait_for(lock, timeout, [this, flag]() { return this->*flag; });
	}
};

struct AppState {

	bool done = false;

    // UI State
    bool showMainApp = false;
	bool specialConfigMode = false; // 用来开启简单UI


    // Data
	int currentConfigIndex = 1;
	std::atomic<int> realConfigIndex{-1}; // 如果realConfigIndex不为-1，说明是特殊配置
	std::map<int, Config> configs;
	std::map<int, std::vector<HistoryEntry>> g_history;
	std::map<int, SpecialConfig> specialConfigs;

	std::map<std::pair<int, int>, AutoBackupTask> g_active_auto_backups; // Key: {configIdx, worldIdx}

	std::mutex configsMutex;			// 用于保护全局配置的互斥锁
	std::mutex task_mutex;		// 专门用于保护 g_active_auto_backups
	bool isRespond = false;
	std::atomic<HotRestoreState> hotkeyRestoreState = HotRestoreState::IDLE;

	KnotLinkModInfo knotLinkMod;

    // Settings
    bool g_CheckForUpdates = true;
};

// Declare a single global instance
extern AppState g_appState;
#endif