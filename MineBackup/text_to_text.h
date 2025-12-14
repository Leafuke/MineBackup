#pragma once
#ifndef TEXT_TO_TEXT_H
#define TEXT_TO_TEXT_H
#include <iostream>
std::string wstring_to_utf8(const std::wstring& wstr);
std::wstring utf8_to_wstring(const std::string& str);
std::string gbk_to_utf8(const std::string& gbk);
std::string utf8_to_gbk(const std::string& utf8);
#endif