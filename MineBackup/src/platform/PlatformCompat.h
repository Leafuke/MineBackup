#pragma once
#ifndef PLATFORM_COMPAT_H
#define PLATFORM_COMPAT_H

// 跨平台兼容层：在非 Windows 平台上提供 Windows API 的兼容替代实现
// 包括：localtime_s, strcpy_s, sprintf_s, _kbhit, GetModuleFileNameW, CopyFileW 等

#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cwchar>
#include <cerrno>
#include <thread>
#include <chrono>
#include <filesystem>
#include <limits.h>

#ifndef _WIN32
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#endif

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

#endif // !_WIN32

#endif // PLATFORM_COMPAT_H
