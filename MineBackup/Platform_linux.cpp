#include "Platform_linux.h"
#include "text_to_text.h"
#include "i18n.h"
#include "Console.h"
#include "AppState.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <cstdio>
#include <thread>
#include <system_error>
#include <unistd.h>

using namespace std;
namespace fs = std::filesystem;

extern atomic<bool> g_UpdateCheckDone;
extern atomic<bool> g_NewVersionAvailable;
extern atomic<bool> g_NoticeCheckDone;
extern atomic<bool> g_NewNoticeAvailable;
extern string g_LatestVersionStr;
extern string g_ReleaseNotes;
extern string g_NoticeContent;
extern string g_NoticeUpdatedAt;
extern string g_NoticeLastSeenVersion;
extern string CURRENT_VERSION;
extern AppState g_appState;

void MessageBoxWin(const std::string& title, const std::string& message, int iconType) {
    (void)iconType;
    std::cout << "[" << title << "] " << message << std::endl;
}

void CheckForUpdatesThread() {
    g_NewVersionAvailable = false;
    g_LatestVersionStr.clear();
    g_ReleaseNotes.clear();
    g_UpdateCheckDone = true;
}

void CheckForNoticesThread() {
    g_NewNoticeAvailable = false;
    g_NoticeContent.clear();
    g_NoticeUpdatedAt.clear();
    g_NoticeCheckDone = true;
}

static std::wstring RunZenity(const std::string& args) {
    std::string cmd = "zenity --file-selection " + args + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return L"";
    char buffer[4096] = {0};
    std::string output;
    if (fgets(buffer, sizeof(buffer), pipe)) {
        output = buffer;
    }
    pclose(pipe);
    if (output.empty()) return L"";
    if (!output.empty() && output.back() == '\n') output.pop_back();
    return utf8_to_wstring(output);
}

std::wstring SelectFileDialog() {
    return RunZenity("--title=\"Select File\"");
}

std::wstring SelectFolderDialog() {
    return RunZenity("--directory --title=\"Select Folder\"");
}

std::wstring GetDocumentsPath() {
    const char* home = std::getenv("HOME");
    if (home) {
        fs::path doc = fs::path(home) / "Documents";
        if (fs::exists(doc)) return doc.wstring();
        return fs::path(home).wstring();
    }
    return L"";
}

static std::wstring TimePointToString(const fs::file_time_type& tp) {
    try {
        auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
            tp - fs::file_time_type::clock::now() + chrono::system_clock::now());
        time_t cftime = chrono::system_clock::to_time_t(sctp);
        struct tm buf;
        if (localtime_s(&buf, &cftime) == 0) {
            wchar_t out[64];
            wcsftime(out, sizeof(out) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &buf);
            return out;
        }
    } catch (...) {
    }
    return L"N/A";
}

std::wstring GetLastOpenTime(const std::wstring& worldPath) {
    try {
        if (!fs::exists(worldPath)) return L"/";
        return TimePointToString(fs::last_write_time(worldPath));
    } catch (...) {
        return L"N/A";
    }
}

std::wstring GetLastBackupTime(const std::wstring& backupDir) {
    try {
        time_t latest = 0;
        if (fs::exists(backupDir)) {
            for (const auto& entry : fs::directory_iterator(backupDir)) {
                if (!entry.is_regular_file()) continue;
                auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
                    entry.last_write_time() - fs::file_time_type::clock::now() + chrono::system_clock::now());
                latest = std::max(latest, chrono::system_clock::to_time_t(sctp));
            }
        }
        if (latest == 0) return L"/";
        struct tm buf;
        if (localtime_s(&buf, &latest) == 0) {
            wchar_t out[64];
            wcsftime(out, sizeof(out) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &buf);
            return out;
        }
    } catch (...) {
    }
    return L"N/A";
}

void RemoveTrayIcon() {
    
}

void GetUserDefaultUILanguageWin() {
    const char* langEnv = std::getenv("LANG");
    if (!langEnv || std::strlen(langEnv) < 2) langEnv = std::getenv("LANGUAGE");
    if (langEnv && std::strlen(langEnv) >= 2) {
        std::string lang(langEnv);
        if (lang.rfind("zh", 0) == 0) {
            g_CurrentLang = "zh_CN";
            return;
        }
    }
    g_CurrentLang = "en_US";
}

std::string GetRegistryValue(const std::string& key, const std::string& valueName) {
    (void)key;
    (void)valueName;
    return std::string();
}

void OpenLinkInBrowser(const std::wstring& url) {
    if (url.empty()) return;
    std::string u8 = wstring_to_utf8(url);
    std::string cmd = "xdg-open '" + u8 + "' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
}

void OpenFolder(const std::wstring& folderPath) {
    if (folderPath.empty()) return;
    std::string u8 = wstring_to_utf8(folderPath);
    std::string cmd = "xdg-open '" + u8 + "' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
}

void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus) {
    (void)focus;
    OpenFolder(folderPath);
}

void ReStartApplication() {
}

void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable) {
    (void)appName;
    (void)appPath;
    (void)configType;
    (void)configId;
    (void)enable;
}

void SetFileAttributesWin(const std::wstring& path, bool isHidden) {
    (void)path;
    (void)isHidden;
}

void EnableDarkModeWin(bool enable) {
    (void)enable;
}

bool Extract7zToTempFile(std::wstring& extractedPath) {
    const char* candidates[] = {"/usr/bin/7z", "/usr/local/bin/7z", nullptr};
    for (const char** p = candidates; *p; ++p) {
        if (fs::exists(*p)) {
            extractedPath = fs::path(*p).wstring();
            return true;
        }
    }
    return false;
}

bool ExtractFontToTempFile(std::wstring& extractedPath) {
    auto exeDir = []() -> fs::path {
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len <= 0) return fs::current_path();
        buf[len] = '\0';
        return fs::path(buf).parent_path();
    }();

    const fs::path bundledCandidates[] = {
        exeDir / "fontawesome-sp.otf",
        exeDir / "fa-solid-900.ttf",
        exeDir / "fa-regular-400.ttf"
    };
    for (const auto& p : bundledCandidates) {
        if (fs::exists(p)) {
            extractedPath = p.wstring();
            return true;
        }
    }

    const char* sysCandidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        nullptr
    };
    for (const char** p = sysCandidates; *p; ++p) {
        if (fs::exists(*p)) {
            extractedPath = fs::path(*p).wstring();
            return true;
        }
    }
    return false;
}

bool IsFileLocked(const std::wstring& path) {
	// ���� Linux ���ļ�û��������
    (void)path;
    return false;
}

bool RunCommandInBackground(std::wstring command, Console& console, bool useLowPriority, const std::wstring& workingDirectory) {
    (void)useLowPriority;
    console.AddLog(L("LOG_EXEC_CMD"), wstring_to_utf8(command).c_str());

    std::error_code ec;
    fs::path oldCwd = fs::current_path(ec);
    if (!workingDirectory.empty() && fs::exists(workingDirectory)) {
        fs::current_path(workingDirectory, ec);
    }

    std::string cmd = wstring_to_utf8(command);
    int ret = std::system(cmd.c_str());

    if (!workingDirectory.empty()) {
        fs::current_path(oldCwd, ec);
    }

    if (ret == 0) {
        console.AddLog(L("LOG_SUCCESS_CMD"));
        return true;
    }
    console.AddLog(L("LOG_ERROR_CMD_FAILED"), ret);
    return false;
}