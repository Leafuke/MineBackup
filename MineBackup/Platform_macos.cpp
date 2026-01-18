#include "Platform_macos.h"
#include "text_to_text.h"
#include "i18n.h"
#include "Console.h"
#include "AppState.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <cstdio>
#include <functional>
#include <limits.h>
#include <thread>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>
#include <mach-o/dyld.h>

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
    std::string icon = "note";
    if (iconType == 1) icon = "caution";
    else if (iconType == 2) icon = "stop";
    
    std::string escaped_message = message;
    size_t pos = 0;
    while ((pos = escaped_message.find('"', pos)) != std::string::npos) {
        escaped_message.replace(pos, 1, "\\\"");
        pos += 2;
    }
    
    std::string cmd = "osascript -e 'display dialog \"" + escaped_message + 
                      "\" with title \"" + title + "\" with icon " + icon + " buttons {\"OK\"}' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
}

void CheckForUpdatesThread() {
    g_NewVersionAvailable = false;
    g_LatestVersionStr.clear();
    g_ReleaseNotes.clear();
    
    std::string cmd = "curl -s -H 'User-Agent: MineBackup' https://api.github.com/repos/Leafuke/MineBackup/releases/latest 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buffer[4096];
        std::string result;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        pclose(pipe);
        
        size_t pos = result.find("\"tag_name\"");
        if (pos != std::string::npos) {
            pos = result.find(':', pos);
            if (pos != std::string::npos) {
                size_t start = result.find('"', pos + 1);
                size_t end = result.find('"', start + 1);
                if (start != std::string::npos && end != std::string::npos) {
                    std::string version = result.substr(start + 1, end - start - 1);
                    if (!version.empty() && version[0] == 'v') {
                        version = version.substr(1);
                    }
                    if (version > CURRENT_VERSION) {
                        g_LatestVersionStr = "v" + version;
                        g_NewVersionAvailable = true;
                        
                        pos = result.find("\"body\"");
                        if (pos != std::string::npos) {
                            pos = result.find(':', pos);
                            size_t noteStart = result.find('"', pos + 1);
                            size_t noteEnd = result.find("\"}", noteStart + 1);
                            if (noteStart != std::string::npos && noteEnd != std::string::npos) {
                                g_ReleaseNotes = result.substr(noteStart + 1, noteEnd - noteStart - 1);
                            }
                        }
                    }
                }
            }
        }
    }
    
    g_UpdateCheckDone = true;
}

void CheckForNoticesThread() {
    g_NewNoticeAvailable = false;
    g_NoticeContent.clear();
    g_NoticeUpdatedAt.clear();
    
    std::string cmd = "curl -s https://raw.githubusercontent.com/Leafuke/MineBackup/develop/notice 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buffer[4096];
        std::string result;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        pclose(pipe);
        
        if (!result.empty()) {
            // Use content hash as version identifier
            std::hash<std::string> hasher;
            g_NoticeUpdatedAt = std::to_string(hasher(result));
            
            if (g_NoticeUpdatedAt != g_NoticeLastSeenVersion) {
                g_NoticeContent = result;
                g_NewNoticeAvailable = true;
            }
        }
    }
    
    g_NoticeCheckDone = true;
}

static std::wstring RunOsaScript(const std::string& script) {
    std::string cmd = "osascript -e '" + script + "' 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return L"";
    
    char buffer[4096] = {0};
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    pclose(pipe);
    
    if (output.empty()) return L"";
    if (!output.empty() && output.back() == '\n') output.pop_back();
    return utf8_to_wstring(output);
}

std::wstring SelectFileDialog() {
    return RunOsaScript("choose file");
}

std::wstring SelectFolderDialog() {
    return RunOsaScript("POSIX path of (choose folder)");
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
        if (localtime_r(&cftime, &buf) != nullptr) {
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
        if (localtime_r(&latest, &buf) != nullptr) {
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
    
    FILE* pipe = popen("defaults read -g AppleLanguages 2>/dev/null | head -2 | tail -1 | tr -d ' \"'", "r");
    if (pipe) {
        char buffer[64] = {0};
        if (fgets(buffer, sizeof(buffer), pipe)) {
            std::string lang(buffer);
            if (lang.rfind("zh", 0) == 0) {
                g_CurrentLang = "zh_CN";
                pclose(pipe);
                return;
            }
        }
        pclose(pipe);
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
    std::string cmd = "open '" + u8 + "' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
}

void OpenFolder(const std::wstring& folderPath) {
    if (folderPath.empty()) return;
    std::string u8 = wstring_to_utf8(folderPath);
    std::string cmd = "open '" + u8 + "' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
}

void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus) {
    if (!focus.empty()) {
        std::string u8 = wstring_to_utf8(focus);
        std::string cmd = "open -R '" + u8 + "' >/dev/null 2>&1 &";
        std::system(cmd.c_str());
    } else {
        OpenFolder(folderPath);
    }
}

void ReStartApplication() {
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::string cmd = std::string(path) + " &";
        std::system(cmd.c_str());
        exit(0);
    }
}

void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable) {
}

void SetFileAttributesWin(const std::wstring& path, bool isHidden) {
}

void EnableDarkModeWin(bool enable) {
}

static fs::path GetExecutableDirectory() {
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        return fs::path(path).parent_path();
    }
    return fs::current_path();
}

bool Extract7zToTempFile(std::wstring& extractedPath) {
    const char* candidates[] = {
        "/usr/local/bin/7z",
        "/opt/homebrew/bin/7z",
        "/usr/bin/7z",
        "/opt/local/bin/7z",
        nullptr
    };
    
    for (const char** p = candidates; *p; ++p) {
        if (fs::exists(*p)) {
            extractedPath = fs::path(*p).wstring();
            return true;
        }
    }
    
    fs::path exeDir = GetExecutableDirectory();
    fs::path bundled7z = exeDir / "7z";
    if (fs::exists(bundled7z)) {
        extractedPath = bundled7z.wstring();
        return true;
    }
    
    const char* altCandidates[] = {
        "/usr/local/bin/7zz",
        "/opt/homebrew/bin/7zz",
        nullptr
    };
    
    for (const char** p = altCandidates; *p; ++p) {
        if (fs::exists(*p)) {
            extractedPath = fs::path(*p).wstring();
            return true;
        }
    }
    
    return false;
}

static bool CopyBundledFontToTemp(const fs::path& source, std::wstring& extractedPath) {
    std::error_code ec;
    if (!fs::exists(source, ec)) return false;

    fs::path tempDir = fs::temp_directory_path(ec);
    if (ec) {
        extractedPath = source.wstring();
        return true;
    }

    fs::path dest = tempDir / source.filename();
    fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec);
    if (!ec && fs::exists(dest, ec)) {
        extractedPath = dest.wstring();
        return true;
    }

    extractedPath = source.wstring();
    return true;
}

bool ExtractFontToTempFile(std::wstring& extractedPath) {
    fs::path exeDir = GetExecutableDirectory();
    
    const fs::path bundledCandidates[] = {
        exeDir / "fontawesome-sp.otf",
        exeDir / "fa-solid-900.ttf",
        exeDir / "fa-regular-400.ttf",
        exeDir / "../Resources/fontawesome-sp.otf",  // For .app bundles
        exeDir / "../Resources/fa-solid-900.ttf",
        exeDir / "../Resources/fa-regular-400.ttf"
    };
    
    for (const auto& p : bundledCandidates) {
        if (CopyBundledFontToTemp(p, extractedPath)) return true;
    }
    
    const char* sysCandidates[] = {
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNS.ttf",
        "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/AppleSDGothicNeo.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
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
    return false;
}

bool RunCommandInBackground(std::wstring command, Console& console, bool useLowPriority, const std::wstring& workingDirectory) {
    console.AddLog(L("LOG_EXEC_CMD"), wstring_to_utf8(command).c_str());
    
    std::error_code ec;
    fs::path oldCwd = fs::current_path(ec);
    if (!workingDirectory.empty() && fs::exists(workingDirectory)) {
        fs::current_path(workingDirectory, ec);
    }
    
    std::string cmd = wstring_to_utf8(command);
    
    if (useLowPriority) {
        cmd = "nice -n 10 " + cmd;
    }
    
    int ret = std::system(cmd.c_str());
    
    if (!workingDirectory.empty()) {
        fs::current_path(oldCwd, ec);
    }
    
    if (ret == 0) {
        console.AddLog(L("LOG_SUCCESS_CMD"));
        return true;
    }
    
    console.AddLog(L("LOG_ERROR_CMD_FAILED"), WEXITSTATUS(ret));
    return false;
}
