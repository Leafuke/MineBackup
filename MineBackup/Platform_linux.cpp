#include "Platform_linux.h"
#include "text_to_text.h"
#include "i18n.h"
#include "Console.h"
#include "AppState.h"
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
extern GLFWwindow* wc;

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
                                std::string rawNotes = result.substr(noteStart + 1, noteEnd - noteStart - 1);
                                g_ReleaseNotes = ExtractLocalizedContent(rawNotes);
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
    
    // 先尝试获取语言特定的 notice 文件
    std::string langSuffix = (g_CurrentLang == "zh_CN") ? "_zh" : "_en";
    std::string langCmd = "curl -s https://raw.githubusercontent.com/Leafuke/MineBackup/develop/notice" + langSuffix + " 2>/dev/null";
    
    std::string result;
    FILE* pipe = popen(langCmd.c_str(), "r");
    if (pipe) {
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        pclose(pipe);
    }
    
    // 如果语言特定文件为空或获取失败,回退到原始 notice 文件
    if (result.empty() || result.find("404") != std::string::npos) {
        result.clear();
        std::string fallbackCmd = "curl -s https://raw.githubusercontent.com/Leafuke/MineBackup/develop/notice 2>/dev/null";
        pipe = popen(fallbackCmd.c_str(), "r");
        if (pipe) {
            char buffer[4096];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                result += buffer;
            }
            pclose(pipe);
            
            // 对旧格式文件提取对应语言内容
            if (!result.empty()) {
                result = ExtractLocalizedContent(result);
            }
        }
    }
    
    if (!result.empty()) {
        std::hash<std::string> hasher;
        g_NoticeUpdatedAt = std::to_string(hasher(result));
        
        if (g_NoticeUpdatedAt != g_NoticeLastSeenVersion) {
            g_NoticeContent = result;
            g_NewNoticeAvailable = true;
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
            g_CurrentLang = "zh_CN";
            return;
        }
    }
    g_CurrentLang = "en_US";
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

void SetAutoStart(const std::string& appName, const std::wstring& appPath, bool configType, int& configId, bool& enable) {
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
        exeDir / "fa-regular-400.ttf"
    };
    for (const auto& p : bundledCandidates) {
        if (CopyBundledFontToTemp(p, extractedPath)) return true;
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