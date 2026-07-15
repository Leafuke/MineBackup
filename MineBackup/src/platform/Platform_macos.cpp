#include "Platform_macos.h"
#include "text_to_text.h"
#include "i18n.h"
#include "AppPaths.h"
#include "ExternalToolManager.h"
#include "MacDesktopBridge.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <system_error>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <CoreFoundation/CoreFoundation.h>

using namespace std;
namespace fs = std::filesystem;

void MessageBoxWin(const std::string& title, const std::string& message, int iconType) {
    MacShowAlert(title, message, iconType);
}

std::wstring SelectFileDialog() {
    return MacSelectFile().path.wstring();
}

std::wstring SelectFolderDialog() {
    return MacSelectFolder().path.wstring();
}

std::wstring SelectSaveFileDialog(const std::wstring& defaultFileName, const std::wstring& filter) {
    return MacSelectSaveFile(defaultFileName, filter).path.wstring();
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

void GetUserDefaultUILanguageWin() {
    const char* langEnv = std::getenv("LANG");
    if (!langEnv || std::strlen(langEnv) < 2) langEnv = std::getenv("LANGUAGE");
    if (langEnv && std::strlen(langEnv) >= 2) {
        std::string lang(langEnv);
        if (lang.rfind("zh", 0) == 0) {
            SetLanguage("zh_CN");
            return;
        }
    }
    
    CFArrayRef languages = CFLocaleCopyPreferredLanguages();
    if (languages && CFArrayGetCount(languages) > 0) {
        CFStringRef language = static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, 0));
        char buffer[64] = {};
        if (language && CFStringGetCString(language, buffer, sizeof(buffer), kCFStringEncodingUTF8)
            && string(buffer).rfind("zh", 0) == 0) {
            CFRelease(languages);
            SetLanguage("zh_CN");
            return;
        }
    }
    if (languages) CFRelease(languages);
    
    SetLanguage("en_US");
}

std::string GetRegistryValue(const std::string& key, const std::string& valueName) {
    (void)key;
    (void)valueName;
    return std::string();
}

void OpenLinkInBrowser(const std::wstring& url) {
    (void)MacOpenUri(url);
}

void OpenFolder(const std::wstring& folderPath) {
    (void)MacOpenFolder(fs::path(folderPath));
}

void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus) {
    (void)MacRevealInFolder(fs::path(folderPath), fs::path(focus));
}

void ReStartApplication() {
    MessageBoxWin("MineBackup", "Please close and reopen MineBackup to complete this operation.", 0);
}

void SetFileAttributesWin(const std::wstring& path, bool isHidden) {
	(void)path;
	(void)isHidden;
}

void EnableDarkModeWin(bool enable) {
	(void)enable;
}

bool Extract7zToTempFile(std::wstring& extractedPath) {
	const auto resolved = ExternalToolManager::ResolveSevenZip({}, GetAppPaths());
	if (!resolved.available) return false;
	extractedPath = resolved.executable.wstring();
	return true;
}

bool ExtractFontToTempFile(std::wstring& extractedPath) {
	const auto resourcesRoot = GetAppPaths().resourcesRoot;
    const fs::path bundledCandidates[] = {
		resourcesRoot / "fontawesome-sp.otf",
		resourcesRoot / "fa-solid-900.ttf",
		resourcesRoot / "fa-regular-400.ttf",
		resourcesRoot / "Assets" / "fontawesome-sp.otf"
    };
	for (const auto& path : bundledCandidates) {
		std::error_code error;
		if (fs::is_regular_file(path, error)) {
			extractedPath = path.wstring();
			return true;
		}
	}
    return false;
}

bool ConfirmMessageBox(const std::string& title, const std::string& message) {
    return MacConfirmAlert(title, message);
}

bool IsFileLocked(const std::wstring& path) {
    // 以下通过AI进行修复，不能保证完全正确，尤其是对于Bedrock Edition的LevelDB锁定机制。需要实际测试验证。
    // On macOS, Minecraft Java Edition locks session.lock via Java's FileChannel.lock()
    // which uses fcntl() F_SETLK underneath. We detect this by checking for existing locks.
    // For Bedrock Edition (LevelDB), flock() is used on db/LOCK.

    std::string u8path = wstring_to_utf8(path);
    if (u8path.empty()) return false;

    std::error_code ec;
    if (!fs::exists(u8path, ec) || ec) return false;

    int fd = open(u8path.c_str(), O_RDWR);
    if (fd < 0) {
        fd = open(u8path.c_str(), O_RDONLY);
        if (fd < 0) return false;
    }

    // Try fcntl advisory lock (what Java's FileChannel.lock() uses)
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd, F_GETLK, &fl) == 0) {
        close(fd);
        return fl.l_type != F_UNLCK;
    }

    // fcntl failed, try flock() as fallback (used by LevelDB / Bedrock)
    int ret = flock(fd, LOCK_EX | LOCK_NB);
    if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        close(fd);
        return true;
    }
    if (ret == 0) {
        flock(fd, LOCK_UN);
    }
    close(fd);
    return false;
}
