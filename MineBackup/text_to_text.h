#pragma once
#ifndef TEXT_TO_TEXT_H
#define TEXT_TO_TEXT_H
#include <string>
#include <filesystem>

// UTF-8 helpers built around char8_t for C++20
std::u8string wstring_to_u8string(const std::wstring& wstr);
std::wstring u8string_to_wstring(const std::u8string& str);

// 跨平台路径工具函数
// 统一路径分隔符：在非Windows平台将反斜杠转为正斜杠
inline std::wstring NormalizeSeparators(const std::wstring& s) {
#ifndef _WIN32
	std::wstring r = s;
	std::replace(r.begin(), r.end(), L'\\', L'/');
	return r;
#else
	return s;
#endif
}

inline std::filesystem::path JoinPath(const std::wstring& a, const std::wstring& b) {
	return std::filesystem::path(NormalizeSeparators(a)) / std::filesystem::path(NormalizeSeparators(b));
}

// 将 filesystem::path 转换为适合当前平台的 wstring
inline std::wstring PathToWstring(const std::filesystem::path& p) {
	return p.wstring();
}

// 获取适合在命令行中使用的路径字符串（带引号处理）
inline std::wstring PathToCommandString(const std::filesystem::path& p) {
	return L"\"" + p.wstring() + L"\"";
}

// 旧的接口，继续保留以兼容现有代码
std::string wstring_to_utf8(const std::wstring& wstr);
std::wstring utf8_to_wstring(const std::string& str);
std::string gbk_to_utf8(const std::string& gbk);
std::string utf8_to_gbk(const std::string& utf8);

// Lightweight bridges when interoperating with legacy char-based APIs 为了保证imgui兼容
const char* as_utf8(const char8_t* s) noexcept;
std::string u8string_to_string(const std::u8string& s);

#endif