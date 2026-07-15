#include "Platform_linux.h"
#include "text_to_text.h"
#include "i18n.h"
#include "Console.h"
#include "AppState.h"
#include "AppPaths.h"
#include "Globals.h"
#include "ProcessRunner.h"
#include "ExternalToolManager.h"
#include <GLFW/glfw3.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#ifdef MB_HAVE_GTK
#include <gtk/gtk.h>
#endif
#ifdef MB_HAVE_APPINDICATOR
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

using namespace std;
namespace fs = std::filesystem;

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

static std::wstring RunZenity(vector<wstring> arguments) {
    ProcessSpec spec;
    spec.executable = fs::exists("/usr/bin/zenity") ? "/usr/bin/zenity" : "/usr/local/bin/zenity";
    spec.arguments = {L"--file-selection"};
    spec.arguments.insert(spec.arguments.end(), arguments.begin(), arguments.end());
    spec.maximumCapturedBytes = 4096;
    const auto process = ProcessRunner::Run(spec);
    if (process.status != ProcessStatus::Succeeded) return L"";
    std::string output = process.standardOutput;
    if (output.empty()) return L"";
    if (!output.empty() && output.back() == '\n') output.pop_back();
    return utf8_to_wstring(output);
}

std::wstring SelectFileDialog() {
    return RunZenity({L"--title=Select File"});
}

std::wstring SelectFolderDialog() {
    return RunZenity({L"--directory", L"--title=Select Folder"});
}

std::wstring SelectSaveFileDialog(const std::wstring& defaultFileName, const std::wstring& filter) {
    vector<wstring> arguments = {L"--save", L"--confirm-overwrite", L"--title=Save File"};
    if (!defaultFileName.empty()) {
        arguments.push_back(L"--filename=" + defaultFileName);
    }
    return RunZenity(std::move(arguments));
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
    ProcessSpec spec;
    spec.executable = L"/usr/bin/xdg-open";
    spec.arguments = {url};
    ProcessRunner::Run(spec);
}

void OpenFolder(const std::wstring& folderPath) {
    if (folderPath.empty()) return;
    ProcessSpec spec;
    spec.executable = L"/usr/bin/xdg-open";
    spec.arguments = {folderPath};
    ProcessRunner::Run(spec);
}

void OpenFolderWithFocus(const std::wstring folderPath, const std::wstring focus) {
    OpenFolder(folderPath);
}

void ReStartApplication() {
}

void SetFileAttributesWin(const std::wstring& path, bool isHidden) {
}

void EnableDarkModeWin(bool enable) {
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
#ifdef MB_HAVE_GTK
    if (!gtk_init_check(nullptr, nullptr)) return false;
    GtkWidget* dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO, "%s", message.c_str());
    gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());
    const bool accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES;
    gtk_widget_destroy(dialog);
    while (gtk_events_pending()) gtk_main_iteration();
    return accepted;
#else
    std::cout << "[" << title << "] " << message << std::endl;
    return false;
#endif
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
