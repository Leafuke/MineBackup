#pragma once
#ifndef TEXT_TO_TEXT_H
#define TEXT_TO_TEXT_H
#include <string>

// UTF-8 helpers built around char8_t for C++20
std::u8string wstring_to_u8string(const std::wstring& wstr);
std::wstring u8string_to_wstring(const std::u8string& str);

// 旧的接口，继续保留以兼容现有代码
std::string wstring_to_utf8(const std::wstring& wstr);
std::wstring utf8_to_wstring(const std::string& str);
std::string gbk_to_utf8(const std::string& gbk);
std::string utf8_to_gbk(const std::string& utf8);

// Lightweight bridges when interoperating with legacy char-based APIs 为了保证imgui兼容
const char* as_utf8(const char8_t* s) noexcept;
std::string u8string_to_string(const std::u8string& s);

#endif