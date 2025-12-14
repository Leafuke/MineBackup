#include <string>
#ifdef _WIN32
#include "Platform_win.h"
#else
#include "Platform_linux.h"
#endif
using namespace std;

// 辅助函数：wstring <-> utf8 string（使用WinAPI，兼容C++17+）本地多字节编码（GBK）转UTF-8
string wstring_to_utf8(const wstring& wstr) {
    if (wstr.empty()) return string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
    string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}
wstring utf8_to_wstring(const string& str) {
    if (str.empty()) return wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), NULL, 0);
    wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}
string gbk_to_utf8(const string& gbk)
{
    int lenW = MultiByteToWideChar(CP_ACP, 0, gbk.c_str(), -1, nullptr, 0);
    wstring wstr(lenW, 0);
    MultiByteToWideChar(CP_ACP, 0, gbk.c_str(), -1, &wstr[0], lenW);

    int lenU8 = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    string u8str(lenU8, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &u8str[0], lenU8, nullptr, nullptr);

    // 去掉末尾的\0
    if (!u8str.empty() && u8str.back() == '\0') u8str.pop_back();
    return u8str;
}
string utf8_to_gbk(const string& utf8)
{
    if (utf8.empty())
        return string();

    // 1. 先把 UTF-8 转成宽字符 (UTF-16)
    int wide_len = ::MultiByteToWideChar(
        CP_UTF8,            // 源是 UTF-8
        0,                  // 默认转换方式
        utf8.c_str(),
        (int)utf8.length(),
        nullptr,
        0
    );
    if (wide_len == 0)
        return string();

    std::wstring wide;
    wide.resize(wide_len);
    ::MultiByteToWideChar(
        CP_UTF8,
        0,
        utf8.c_str(),
        (int)utf8.length(),
        &wide[0],
        wide_len
    );

    // 2. 再把宽字符转换成 ANSI (即 GBK)
    int gbk_len = ::WideCharToMultiByte(
        CP_ACP,             // ANSI code page (GBK)
        0,
        wide.c_str(),
        wide_len,
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (gbk_len == 0)
        return string();

    string gbk;
    gbk.resize(gbk_len);
    ::WideCharToMultiByte(
        CP_ACP,
        0,
        wide.c_str(),
        wide_len,
        &gbk[0],
        gbk_len,
        nullptr,
        nullptr
    );

    return gbk;
}