#pragma once
#ifndef _i18n
#define _i18n
#include <iostream>
#include <string>
#include <unordered_map>

// Helper to accept both char and char8_t literals while storing UTF-8 bytes
struct Utf8Value {
	std::string value;
	Utf8Value() = default;
	Utf8Value(const char* s) : value(s ? s : "") {}
	Utf8Value(const char8_t* s) : value(s ? reinterpret_cast<const char*>(s) : "") {}
};

extern std::unordered_map<std::string, std::unordered_map<std::string, Utf8Value>> g_LangTable;
extern std::string g_CurrentLang;
extern const char* lang_codes[2];
extern const char* langs[2];
const char* L(const char* key);

#endif // !_i18n