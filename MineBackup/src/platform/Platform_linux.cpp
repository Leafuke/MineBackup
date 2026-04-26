#include "Platform_linux.h"
#include "text_to_text.h"
#include "i18n.h"
#include "Console.h"
#include "AppState.h"
#include "Globals.h"
#include "json.hpp"
#include <GLFW/glfw3.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#ifdef MB_HAVE_APPINDICATOR
#include <gtk/gtk.h>
#include <libappindicator/app-indicator.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <thread>
#include <system_error>
#include <unistd.h>
#include <cctype>
#include <vector>
#include <sstream>
#include <fcntl.h>
#include <sys/file.h>
#include <cstring>
#include <tuple>
#include <cstdint>

using namespace std;
namespace fs = std::filesystem;

// 根据当前语言从文本中提取对应部分
// 文本格式: "中文内容\n---\n英文内容"
static string ExtractLocalizedContent(const string& content) {
	size_t sepPos = content.find("---");
	if (sepPos == string::npos) {
		return content;
	}
	
	size_t beforeSep = sepPos;
	while (beforeSep > 0 && (content[beforeSep - 1] == '\n' || content[beforeSep - 1] == '\r' || content[beforeSep - 1] == ' ')) {
		beforeSep--;
	}
	
	size_t afterSep = sepPos + 3;
	while (afterSep < content.size() && (content[afterSep] == '\n' || content[afterSep] == '\r' || content[afterSep] == ' ' || content[afterSep] == '-')) {
		afterSep++;
	}
	
	string chineseContent = content.substr(0, beforeSep);
	string englishContent = afterSep < content.size() ? content.substr(afterSep) : "";
	
	while (!chineseContent.empty() && (chineseContent.back() == '\n' || chineseContent.back() == '\r' || chineseContent.back() == ' ')) {
		chineseContent.pop_back();
	}
	while (!englishContent.empty() && (englishContent.back() == '\n' || englishContent.back() == '\r' || englishContent.back() == ' ')) {
		englishContent.pop_back();
	}
	
	if (g_CurrentLang == "zh_CN") {
		return chineseContent.empty() ? content : chineseContent;
	} else {
		return englishContent.empty() ? content : englishContent;
	}
}

static string ToLowerAscii(const string& value) {
    string lowered = value;
    for (char& ch : lowered) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    return lowered;
}

static string TrimAsciiWhitespace(const string& value) {
    size_t begin = 0;
    while (begin < value.size() && isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

static string NormalizeNoticeText(const string& value) {
    string normalized;
    normalized.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\r') {
            if (i + 1 < value.size() && value[i + 1] == '\n') {
                ++i;
            }
            normalized.push_back('\n');
        } else {
            normalized.push_back(value[i]);
        }
    }
    return TrimAsciiWhitespace(normalized);
}

static bool IsLikely404Body(const string& body) {
    string lowered = ToLowerAscii(TrimAsciiWhitespace(body));
    if (lowered.empty()) return true;
    if (lowered == "404" || lowered == "404: not found" || lowered == "not found") return true;
    if (lowered.find("<title>404") != string::npos) return true;
    if (lowered.find("404 not found") != string::npos) return true;
    if (lowered.find("error 404") != string::npos) return true;
    return false;
}

static string BuildMirrorUrl(const string& directUrl) {
    const string mirrorPrefix = "https://gh-proxy.org/";
    if (directUrl.rfind(mirrorPrefix, 0) == 0) {
        return directUrl;
    }
    return mirrorPrefix + directUrl;
}

static string BuildStableContentId(const string& text) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    ostringstream oss;
    oss << "notice-v1-" << hex << hash;
    return oss.str();
}

static tuple<int, int, int, int> ParseVersionTuple(const string& ver) {
    try {
        size_t p1 = ver.find('.');
        size_t p2 = (p1 != string::npos) ? ver.find('.', p1 + 1) : string::npos;
        size_t p3 = ver.find('-');
        if (p1 == string::npos || p2 == string::npos) return {0, 0, 0, 0};

        int major = stoi(ver.substr(0, p1));
        int minor = stoi(ver.substr(p1 + 1, p2 - p1 - 1));
        int patch = (p3 == string::npos) ? stoi(ver.substr(p2 + 1)) : stoi(ver.substr(p2 + 1, p3 - p2 - 1));
        int sp = 0;
        if (p3 != string::npos) {
            size_t spPos = ver.find("sp", p3);
            if (spPos != string::npos) {
                sp = stoi(ver.substr(spPos + 2));
            }
        }
        return {major, minor, patch, sp};
    } catch (...) {
        return {0, 0, 0, 0};
    }
}

static bool FetchHttpText(const string& url, string& outBody) {
    outBody.clear();
    string cmd = "curl -L -s --connect-timeout 8 --max-time 20 -w \"\\n%{http_code}\" -H 'User-Agent: MineBackup' '" + url + "' 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    char buffer[4096];
    string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);

    size_t splitPos = result.rfind('\n');
    if (splitPos == string::npos) return false;
    string statusText = TrimAsciiWhitespace(result.substr(splitPos + 1));
    string body = result.substr(0, splitPos);
    if (statusText != "200") return false;

    body = TrimAsciiWhitespace(body);
    if (body.empty()) return false;
    outBody = body;
    return true;
}

static bool FetchWithMirrorFallback(const string& directUrl, bool reject404Body, string& outBody) {
    vector<string> candidates = {directUrl, BuildMirrorUrl(directUrl)};
    for (const string& candidate : candidates) {
        string body;
        if (!FetchHttpText(candidate, body)) {
            continue;
        }
        if (reject404Body && IsLikely404Body(body)) {
            continue;
        }
        outBody = body;
        return true;
    }
    return false;
}

static atomic<bool> g_trayThreadRunning(false);
static thread g_trayThread;

#ifdef MB_HAVE_APPINDICATOR
static AppIndicator* g_indicator = nullptr;
static gboolean TrayQuitIdle(gpointer) {
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void TrayMenuOpen(GtkMenuItem*, gpointer) {
    g_appState.showMainApp = true;
    if (wc) {
        glfwShowWindow(wc);
        glfwFocusWindow(wc);
        glfwPostEmptyEvent();
    }
}

static void TrayMenuExit(GtkMenuItem*, gpointer) {
    g_appState.done = true;
    glfwPostEmptyEvent();
}
#endif

static atomic<bool> g_hotkeyThreadRunning(false);
static thread g_hotkeyThread;
static Display* g_hotkeyDisplay = nullptr;
static Window g_hotkeyRoot = 0;
static int g_backupKeycode = 0;
static int g_restoreKeycode = 0;

static int X11KeycodeFromAscii(Display* display, int key) {
    if (!display) return 0;
    char c = static_cast<char>(std::toupper(key));
    std::string s(1, c);
    KeySym sym = XStringToKeysym(s.c_str());
    if (sym == NoSymbol) return 0;
    return XKeysymToKeycode(display, sym);
}

static void GrabKeyWithMask(Display* display, Window root, int keycode, unsigned int mask) {
    XGrabKey(display, keycode, mask, root, True, GrabModeAsync, GrabModeAsync);
}

static void UngrabKeyWithMask(Display* display, Window root, int keycode, unsigned int mask) {
    XUngrabKey(display, keycode, mask, root);
}

static void ApplyGrabMasks(Display* display, Window root, int keycode, bool grab) {
    const unsigned int baseMask = ControlMask | Mod1Mask;
    const unsigned int masks[] = {
        baseMask,
        baseMask | LockMask,
        baseMask | Mod2Mask,
        baseMask | LockMask | Mod2Mask
    };
    for (unsigned int mask : masks) {
        if (grab) GrabKeyWithMask(display, root, keycode, mask);
        else UngrabKeyWithMask(display, root, keycode, mask);
    }
}

static void HotkeyEventLoop() {
    while (g_hotkeyThreadRunning) {
        if (!g_hotkeyDisplay) break;
        while (XPending(g_hotkeyDisplay)) {
            XEvent ev;
            XNextEvent(g_hotkeyDisplay, &ev);
            if (ev.type == KeyPress) {
                unsigned int state = ev.xkey.state;
                if ((state & ControlMask) && (state & Mod1Mask)) {
                    if (ev.xkey.keycode == g_backupKeycode) {
                        TriggerHotkeyBackup();
                    } else if (ev.xkey.keycode == g_restoreKeycode) {
                        TriggerHotkeyRestore();
                    }
                }
            }
        }
        this_thread::sleep_for(chrono::milliseconds(50));
    }
}

void MessageBoxWin(const std::string& title, const std::string& message, int iconType) {
    (void)iconType;
    std::cout << "[" << title << "] " << message << std::endl;
}

void CheckForUpdatesThread() {
    g_NewVersionAvailable = false;
    g_LatestVersionStr.clear();
    g_ReleaseNotes.clear();

    const string apiUrl = "https://api.github.com/repos/Leafuke/MineBackup/releases/latest";
    vector<string> candidates = {apiUrl, BuildMirrorUrl(apiUrl)};
    for (const string& candidate : candidates) {
        string body;
        if (!FetchHttpText(candidate, body) || IsLikely404Body(body)) {
            continue;
        }

        try {
            nlohmann::json parsed = nlohmann::json::parse(body);
            if (!parsed.contains("tag_name") || !parsed["tag_name"].is_string()) {
                continue;
            }

            string version = parsed["tag_name"].get<string>();
            if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) {
                version = version.substr(1);
            }
            if (version.empty()) {
                continue;
            }

            if (ParseVersionTuple(version) > ParseVersionTuple(CURRENT_VERSION)) {
                g_LatestVersionStr = "v" + version;
                g_NewVersionAvailable = true;

                string rawNotes;
                if (parsed.contains("body") && parsed["body"].is_string()) {
                    rawNotes = parsed["body"].get<string>();
                }
                for (size_t i = 0; i + 1 < rawNotes.size(); ++i) {
                    if (rawNotes[i] == '#') {
                        rawNotes[i] = ' ';
                    } else if (rawNotes[i] == '\\' && rawNotes[i + 1] == 'n') {
                        rawNotes[i] = '\n';
                        rawNotes[i + 1] = ' ';
                    } else if (rawNotes[i] == '\\') {
                        rawNotes[i] = ' ';
                        rawNotes[i + 1] = ' ';
                    }
                }
                g_ReleaseNotes = ExtractLocalizedContent(rawNotes);
            }
            break;
        } catch (...) {
            continue;
        }
    }
    
    g_UpdateCheckDone = true;
}

void CheckForNoticesThread() {
    g_NewNoticeAvailable = false;
    g_NoticeContent.clear();
    g_NoticeUpdatedAt.clear();
    
    string langUrl = string("https://raw.githubusercontent.com/Leafuke/MineBackup/develop/notice") + ((g_CurrentLang == "zh_CN") ? "_zh" : "_en");
    string fallbackUrl = "https://raw.githubusercontent.com/Leafuke/MineBackup/develop/notice";

    string result;
    bool fromFallback = false;
    if (!FetchWithMirrorFallback(langUrl, true, result)) {
        if (FetchWithMirrorFallback(fallbackUrl, true, result)) {
            fromFallback = true;
        }
    }

    if (!result.empty()) {
        string shownNotice = fromFallback ? ExtractLocalizedContent(result) : result;
        shownNotice = NormalizeNoticeText(shownNotice);
        if (!shownNotice.empty() && !IsLikely404Body(shownNotice)) {
            g_NoticeUpdatedAt = BuildStableContentId(shownNotice);
            if (g_NoticeUpdatedAt != g_NoticeLastSeenVersion) {
                g_NoticeContent = shownNotice;
                g_NewNoticeAvailable = true;
            }
        }
    }
    
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

std::wstring SelectSaveFileDialog(const std::wstring& defaultFileName, const std::wstring& filter) {
    std::string args = "--save --confirm-overwrite --title=\"Save File\"";
    if (!defaultFileName.empty()) {
        args += " --filename=\"" + wstring_to_utf8(defaultFileName) + "\"";
    }
    return RunZenity(args);
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

void CreateTrayIcon() {
    if (g_trayThreadRunning) return;
#ifdef MB_HAVE_APPINDICATOR
    g_trayThreadRunning = true;
    g_trayThread = thread([]() {
        int argc = 0;
        char** argv = nullptr;
        gtk_init(&argc, &argv);
        g_indicator = app_indicator_new("minebackup", "applications-system", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
        app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_ACTIVE);

        GtkWidget* menu = gtk_menu_new();
        GtkWidget* open_item = gtk_menu_item_new_with_label(L("OPEN"));
        GtkWidget* exit_item = gtk_menu_item_new_with_label(L("EXIT"));

        g_signal_connect(open_item, "activate", G_CALLBACK(TrayMenuOpen), nullptr);
        g_signal_connect(exit_item, "activate", G_CALLBACK(TrayMenuExit), nullptr);

        gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), exit_item);
        gtk_widget_show_all(menu);

        app_indicator_set_menu(g_indicator, GTK_MENU(menu));
        gtk_main();
        g_indicator = nullptr;
    });
#else
    if (!isatty(STDIN_FILENO)) return;
    g_trayThreadRunning = true;
    g_trayThread = thread([]() {
        while (g_trayThreadRunning) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(STDIN_FILENO, &set);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
            int res = select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv);
            if (!g_trayThreadRunning) break;
            if (res > 0 && FD_ISSET(STDIN_FILENO, &set)) {
                char buf[64];
                (void)read(STDIN_FILENO, buf, sizeof(buf));
                g_appState.showMainApp = true;
                if (wc) {
                    glfwShowWindow(wc);
                    glfwFocusWindow(wc);
                    glfwPostEmptyEvent();
                }
            }
        }
    });
#endif
}

void RemoveTrayIcon() {
    g_trayThreadRunning = false;
#ifdef MB_HAVE_APPINDICATOR
    if (g_indicator) {
        g_idle_add(TrayQuitIdle, nullptr);
    }
#endif
    if (g_trayThread.joinable()) {
        g_trayThread.join();
    }
}

void RegisterHotkeys(int hotkeyId, int key) {
    if (!g_hotkeyDisplay) {
        g_hotkeyDisplay = XOpenDisplay(nullptr);
        if (!g_hotkeyDisplay) return;
        g_hotkeyRoot = DefaultRootWindow(g_hotkeyDisplay);
    }
    if (!g_hotkeyThreadRunning) {
        g_hotkeyThreadRunning = true;
        g_hotkeyThread = thread(HotkeyEventLoop);
    }
    int keycode = X11KeycodeFromAscii(g_hotkeyDisplay, key);
    if (keycode == 0) return;
    if (hotkeyId == MINEBACKUP_HOTKEY_ID) g_backupKeycode = keycode;
    if (hotkeyId == MINERESTORE_HOTKEY_ID) g_restoreKeycode = keycode;
    ApplyGrabMasks(g_hotkeyDisplay, g_hotkeyRoot, keycode, true);
    XSync(g_hotkeyDisplay, False);
}

void UnregisterHotkeys(int hotkeyId) {
    if (!g_hotkeyDisplay) return;
    int keycode = 0;
    if (hotkeyId == MINEBACKUP_HOTKEY_ID) {
        keycode = g_backupKeycode;
        g_backupKeycode = 0;
    }
    if (hotkeyId == MINERESTORE_HOTKEY_ID) {
        keycode = g_restoreKeycode;
        g_restoreKeycode = 0;
    }
    if (keycode != 0) {
        ApplyGrabMasks(g_hotkeyDisplay, g_hotkeyRoot, keycode, false);
        XSync(g_hotkeyDisplay, False);
    }
    if (g_backupKeycode == 0 && g_restoreKeycode == 0) {
        g_hotkeyThreadRunning = false;
        if (g_hotkeyThread.joinable()) g_hotkeyThread.join();
        XCloseDisplay(g_hotkeyDisplay);
        g_hotkeyDisplay = nullptr;
        g_hotkeyRoot = 0;
    }
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
    SetLanguage("en_US");
}

std::string GetRegistryValue(const std::string& key, const std::string& valueName) {
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
    OpenFolder(folderPath);
}

void ReStartApplication() {
}

void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable, bool silentStartupToTray) {
}

void SetFileAttributesWin(const std::wstring& path, bool isHidden) {
}

void EnableDarkModeWin(bool enable) {
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
        exeDir / "fa-regular-400.ttf",
        exeDir / "Assets" / "fontawesome-sp.otf"
    };
    for (const auto& p : bundledCandidates) {
        if (CopyBundledFontToTemp(p, extractedPath)) return true;
    }

    return false;
}

bool IsFileLocked(const std::wstring& path) {
	// 以下通过AI进行修复，不能保证完全正确，尤其是对于Bedrock Edition的LevelDB锁定机制。需要实际测试验证。
    // On Linux, Minecraft Java Edition locks session.lock via Java's FileChannel.lock()
    // which uses fcntl() F_SETLK underneath. We can detect this by trying to acquire
    // a write lock on the same file.
    // For Bedrock Edition (LevelDB), flock() is used on db/LOCK.

    std::string u8path = wstring_to_utf8(path);
    if (u8path.empty()) return false;

    std::error_code ec;
    if (!fs::exists(u8path, ec) || ec) return false;

    int fd = open(u8path.c_str(), O_RDWR);
    if (fd < 0) {
        // If we can't open for writing, try read-only and check fcntl lock
        fd = open(u8path.c_str(), O_RDONLY);
        if (fd < 0) return false;
    }

    // Try fcntl advisory lock (what Java's FileChannel.lock() uses)
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;   // Try to acquire a write lock
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;          // Lock the entire file

    // F_GETLK checks if the lock *would* conflict — if it would, the file is locked
    if (fcntl(fd, F_GETLK, &fl) == 0) {
        close(fd);
        // If l_type is not F_UNLCK, another process holds a lock
        return fl.l_type != F_UNLCK;
    }

    // fcntl failed, try flock() as fallback (used by LevelDB / Bedrock)
    int ret = flock(fd, LOCK_EX | LOCK_NB);
    if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        close(fd);
        return true; // File is locked by another process
    }
    // We acquired the lock; release it immediately
    if (ret == 0) {
        flock(fd, LOCK_UN);
    }
    close(fd);
    return false;
}

bool RunCommandInBackground(const std::wstring& command, Console& console, bool useLowPriority, const std::wstring& workingDirectory) {
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

bool RunCommandWithResult(const std::wstring& command, Console& console, bool useLowPriority, int timeoutSeconds, int& exitCode, bool& timedOut, std::string& errorMessage, const std::wstring& workingDirectory) {
    (void)timeoutSeconds;
    timedOut = false;
    errorMessage.clear();
    bool success = RunCommandInBackground(command, console, useLowPriority, workingDirectory);
    exitCode = success ? 0 : 1;
    if (!success) {
        errorMessage = "Command failed.";
    }
    return success;
}
