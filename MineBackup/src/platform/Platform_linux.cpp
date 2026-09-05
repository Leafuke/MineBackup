#include "Platform_linux.h"
#include "PlatformCompat.h"
#include "text_to_text.h"
#include "i18n.h"
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
static bool g_gtkInitialized = false;
static bool EnsureGtkInitialized() {
    if (g_gtkInitialized) return true;
    int argc = 0;
    char** argv = nullptr;
    g_gtkInitialized = gtk_init_check(&argc, &argv) != FALSE;
    return g_gtkInitialized;
}
#endif
#ifdef MB_HAVE_APPINDICATOR
#ifdef MB_USE_AYATANA_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#else
#include <libappindicator/app-indicator.h>
#endif
#endif

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <cstdio>
#include <algorithm>
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

#ifdef MB_HAVE_APPINDICATOR
static AppIndicator* g_indicator = nullptr;
static GtkWidget* g_trayMenu = nullptr;

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

static Display* g_hotkeyDisplay = nullptr;
static Window g_hotkeyRoot = 0;
static int g_backupKeycode = 0;
static int g_restoreKeycode = 0;
static Display* g_hotkeyErrorDisplay = nullptr;
static bool g_hotkeyGrabFailed = false;
static XErrorHandler g_previousXErrorHandler = nullptr;

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

static int HotkeyXErrorHandler(Display* display, XErrorEvent* event) {
    (void)event;
    if (display == g_hotkeyErrorDisplay) {
        g_hotkeyGrabFailed = true;
        return 0;
    }
    return g_previousXErrorHandler ? g_previousXErrorHandler(display, event) : 0;
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

void MessageBoxWin(const std::string& title, const std::string& message, int iconType) {
    (void)iconType;
    std::cout << "[" << title << "] " << message << std::endl;
}

static std::wstring RunNativeFileChooser(
    int action, const char* title, const std::wstring& defaultFileName = {}) {
#ifdef MB_HAVE_GTK
    if (!EnsureGtkInitialized()) return L"";
    auto* dialog = gtk_file_chooser_native_new(title, nullptr,
        static_cast<GtkFileChooserAction>(action), "Select", "Cancel");
    if (!dialog) return L"";
    if (!defaultFileName.empty()) {
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog),
            wstring_to_utf8(defaultFileName).c_str());
    }
    const int response = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog));
    std::wstring selected;
    if (response == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            selected = utf8_to_wstring(filename);
            g_free(filename);
        }
    }
    g_object_unref(dialog);
    return selected;
#else
    (void)action;
    (void)title;
    (void)defaultFileName;
    return L"";
#endif
}

std::wstring SelectFileDialog() {
#ifdef MB_HAVE_GTK
    return RunNativeFileChooser(GTK_FILE_CHOOSER_ACTION_OPEN, "Select File");
#else
    return L"";
#endif
}

std::wstring SelectFolderDialog() {
#ifdef MB_HAVE_GTK
    return RunNativeFileChooser(GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Select Folder");
#else
    return L"";
#endif
}

std::wstring SelectSaveFileDialog(const std::wstring& defaultFileName, const std::wstring& filter) {
    (void)filter;
#ifdef MB_HAVE_GTK
    return RunNativeFileChooser(GTK_FILE_CHOOSER_ACTION_SAVE, "Save File", defaultFileName);
#else
    return L"";
#endif
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

bool CreateTrayIcon() {
#ifdef MB_HAVE_APPINDICATOR
    if (g_indicator) return true;
    if (!EnsureGtkInitialized()) return false;
    g_indicator = app_indicator_new("minebackup", "minebackup",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    if (!g_indicator) return false;
    app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_ACTIVE);

    g_trayMenu = gtk_menu_new();
    GtkWidget* open_item = gtk_menu_item_new_with_label(L("OPEN"));
    GtkWidget* exit_item = gtk_menu_item_new_with_label(L("EXIT"));

    g_signal_connect(open_item, "activate", G_CALLBACK(TrayMenuOpen), nullptr);
    g_signal_connect(exit_item, "activate", G_CALLBACK(TrayMenuExit), nullptr);

    gtk_menu_shell_append(GTK_MENU_SHELL(g_trayMenu), open_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_trayMenu), exit_item);
    gtk_widget_show_all(g_trayMenu);

    app_indicator_set_menu(g_indicator, GTK_MENU(g_trayMenu));
    return true;
#else
    return false;
#endif
}

void RemoveTrayIcon() {
#ifdef MB_HAVE_APPINDICATOR
    if (g_indicator) {
        app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_PASSIVE);
        g_clear_object(&g_indicator);
    }
    if (g_trayMenu) {
        gtk_widget_destroy(g_trayMenu);
        g_trayMenu = nullptr;
    }
#endif
}

void PumpLinuxDesktopEvents() {
#ifdef MB_HAVE_GTK
    GMainContext* context = g_main_context_default();
    while (g_main_context_pending(context)) {
        g_main_context_iteration(context, FALSE);
    }
#endif
    // The dedicated hotkey Display is opened, read and closed exclusively by
    // the GLFW main thread. This avoids relying on process-wide XInitThreads.
    while (g_hotkeyDisplay && XPending(g_hotkeyDisplay)) {
        XEvent event;
        XNextEvent(g_hotkeyDisplay, &event);
        if (event.type != KeyPress) continue;
        const unsigned int state = event.xkey.state;
        if ((state & ControlMask) == 0 || (state & Mod1Mask) == 0) continue;
        if (event.xkey.keycode == g_backupKeycode) TriggerHotkeyBackup();
        else if (event.xkey.keycode == g_restoreKeycode) TriggerHotkeyRestore();
    }
}

static bool GrabKeyChecked(Display* display, Window root, int keycode) {
    // Drain older errors before installing a short-lived process-wide handler;
    // XSync below guarantees all errors from these grabs are observed here.
    XSync(display, False);
    g_hotkeyErrorDisplay = display;
    g_hotkeyGrabFailed = false;
    g_previousXErrorHandler = XSetErrorHandler(HotkeyXErrorHandler);
    ApplyGrabMasks(display, root, keycode, true);
    XSync(display, False);
    XSetErrorHandler(g_previousXErrorHandler);
    g_previousXErrorHandler = nullptr;
    g_hotkeyErrorDisplay = nullptr;
    if (!g_hotkeyGrabFailed) return true;

    ApplyGrabMasks(display, root, keycode, false);
    XSync(display, False);
    return false;
}

static void CloseHotkeyDisplayIfUnused() {
    if (!g_hotkeyDisplay || g_backupKeycode != 0 || g_restoreKeycode != 0) return;
    XCloseDisplay(g_hotkeyDisplay);
    g_hotkeyDisplay = nullptr;
    g_hotkeyRoot = 0;
}

bool RegisterHotkeys(int hotkeyId, int key) {
    if (!g_hotkeyDisplay) {
        g_hotkeyDisplay = XOpenDisplay(nullptr);
        if (!g_hotkeyDisplay) return false;
        g_hotkeyRoot = DefaultRootWindow(g_hotkeyDisplay);
    }
    int keycode = X11KeycodeFromAscii(g_hotkeyDisplay, key);
    if (keycode == 0
        || (hotkeyId != MINEBACKUP_HOTKEY_ID && hotkeyId != MINERESTORE_HOTKEY_ID)
        || !GrabKeyChecked(g_hotkeyDisplay, g_hotkeyRoot, keycode)) {
        CloseHotkeyDisplayIfUnused();
        return false;
    }
    if (hotkeyId == MINEBACKUP_HOTKEY_ID) g_backupKeycode = keycode;
    else g_restoreKeycode = keycode;
    return true;
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
    CloseHotkeyDisplayIfUnused();
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
	(void)enable;
}

bool IsSystemDarkMode() {
	FILE* pipe = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
	if (pipe) {
		char buffer[128];
		bool isDark = false;
		if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
			if (strstr(buffer, "prefer-dark") != nullptr || strstr(buffer, "dark") != nullptr) {
				isDark = true;
			}
		}
		pclose(pipe);
		return isDark;
	}
	return false;
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
