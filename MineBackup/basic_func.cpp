#include <string>
#include <map>
#include <filesystem>
#include <fstream>
#include <Windows.h>
using namespace std;

// ע����ѯ
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
        // ת��Ϊ system_clock::time_point
        auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
            ftime - filesystem::file_time_type::clock::now()
            + chrono::system_clock::now()
        );
        time_t cftime = chrono::system_clock::to_time_t(sctp);
        wchar_t buf[64];
        struct tm timeinfo;
        //wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtimeҪ���ɸ���ȫ��localtime
        if (localtime_s(&timeinfo, &cftime) == 0) {
            wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
            return buf;
        }
        else {
            return L"N/A";
        }
    }
    catch (...) {
        return L"N/A";
    }
}

wstring GetLastBackupTime(const wstring& backupDir) {
    time_t latest = 0;
    try {
        if (filesystem::exists(backupDir)) {
            for (const auto& entry : filesystem::directory_iterator(backupDir)) {
                if (entry.is_regular_file()) {
                    auto ftime = entry.last_write_time();
                    // ת��Ϊ system_clock::time_point
                    auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
                        ftime - filesystem::file_time_type::clock::now()
                        + chrono::system_clock::now()
                    );
                    time_t cftime = chrono::system_clock::to_time_t(sctp);
                    if (cftime > latest) latest = cftime;
                }
            }
        }
        if (latest == 0) return L"/";
        wchar_t buf[64];
        struct tm timeinfo;
        //wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtimeҪ���ɸ���ȫ��localtime
        if (localtime_s(&timeinfo, &latest) == 0) {
            wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
            return buf;
        }
        else {
            return L"N/A";
        }
    }
    catch (...) {
        return L"N/A";
    }
}

// �����ļ��Ĺ�ϣֵ������һ���򵥵�ʵ�֣��ܲ��ϸ��գ�
size_t CalculateFileHash(const std::filesystem::path& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return 0;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::hash<std::string> hasher;
    return hasher(content);
}

// ��ȡ�Ѹ��ĵ��ļ��б�������״̬�ļ�
vector<std::filesystem::path> GetChangedFiles(const std::filesystem::path& worldPath, const std::filesystem::path& metadataPath) {
    std::vector<std::filesystem::path> changedFiles;
    std::map<std::wstring, size_t> lastState;
    std::map<std::wstring, size_t> currentState;
    std::filesystem::path stateFilePath = metadataPath / L"backup_state.txt";

    // 1. ��ȡ��һ�ε�״̬
    std::wifstream stateFileIn(stateFilePath);
    if (stateFileIn.is_open()) {
        std::wstring path;
        size_t hash;
        while (stateFileIn >> path >> hash) {
            lastState[path] = hash;
        }
        stateFileIn.close();
    }

    // 2. ���㵱ǰ״̬�����ϴ�״̬�Ƚ�
    if (std::filesystem::exists(worldPath)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(worldPath)) {
            if (entry.is_regular_file()) {
                std::filesystem::path relativePath = std::filesystem::relative(entry.path(), worldPath);
                size_t currentHash = CalculateFileHash(entry.path());
                currentState[relativePath.wstring()] = currentHash;

                // ����ļ����µģ����߹�ϣֵ��ͬ�����ж�Ϊ�Ѹ���
                if (lastState.find(relativePath.wstring()) == lastState.end() || lastState[relativePath.wstring()] != currentHash) {
                    changedFiles.push_back(entry.path());
                }
            }
        }
    }

    // 3. ����ǰ״̬д���ļ������´�ʹ��
    std::wofstream stateFileOut(stateFilePath, std::ios::trunc);
    for (const auto& pair : currentState) {
        stateFileOut << pair.first << L" " << pair.second << std::endl;
    }
    stateFileOut.close();

    return changedFiles;
}