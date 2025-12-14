#pragma once
#ifndef _i18n
#define _i18n
#include <iostream>
#include <unordered_map>

extern std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_LangTable;
extern std::string g_CurrentLang;
extern const char* lang_codes[2];
extern const char* langs[2];
const char* L(const char* key);

#endif // !_i18n