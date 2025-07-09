#include <string>
#include <filesystem>
#include <Windows.h>
using namespace std;

// 注册表查询
string GetRegistryValue(const string& keyPath, const string& valueName)
{
    HKEY hKey;
    string valueData;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buffer[1024];
        DWORD dataSize = sizeof(buffer);
        if (RegGetValueA(hKey, NULL, valueName.c_str(), RRF_RT_ANY, NULL, buffer, &dataSize) == ERROR_SUCCESS) {
            valueData = buffer;
        }
        RegCloseKey(hKey);
    }
    else
        return "";
    return valueData;
}

wstring GetLastOpenTime(const wstring& worldPath) {
    try {
        auto ftime = filesystem::last_write_time(worldPath);
        // 转换为 system_clock::time_point
        auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
            ftime - filesystem::file_time_type::clock::now()
            + chrono::system_clock::now()
        );
        time_t cftime = chrono::system_clock::to_time_t(sctp);
        wchar_t buf[64];
        struct tm timeinfo;
        //wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtime要换成更安全的localtime
        if (localtime_s(&timeinfo, &cftime) == 0) {
            wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
            return buf;
        }
        else {
            return L"未知";
        }
    }
    catch (...) {
        return L"未知";
    }
}

wstring GetLastBackupTime(const wstring& backupDir) {
    time_t latest = 0;
    try {
        if (filesystem::exists(backupDir)) {
            for (const auto& entry : filesystem::directory_iterator(backupDir)) {
                if (entry.is_regular_file()) {
                    auto ftime = entry.last_write_time();
                    // 转换为 system_clock::time_point
                    auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
                        ftime - filesystem::file_time_type::clock::now()
                        + chrono::system_clock::now()
                    );
                    time_t cftime = chrono::system_clock::to_time_t(sctp);
                    if (cftime > latest) latest = cftime;
                }
            }
        }
        if (latest == 0) return L"无备份";
        wchar_t buf[64];
        struct tm timeinfo;
        //wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtime要换成更安全的localtime
        if (localtime_s(&timeinfo, &latest) == 0) {
            wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
            return buf;
        }
        else {
            return L"未知";
        }
    }
    catch (...) {
        return L"未知";
    }
}