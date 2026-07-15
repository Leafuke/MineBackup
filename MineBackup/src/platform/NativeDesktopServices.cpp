#include "NativeDesktopServices.h"

#include "PlatformCompat.h"

#include <GLFW/glfw3.h>

#include <cstdlib>
#include <set>
#include <utility>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

namespace {

CapabilityStatus InvalidArgument(const wchar_t* subject) {
    return CapabilityStatus::Failed(wstring(subject) + L" must not be empty.");
}

#ifdef _WIN32
CapabilityStatus ConfigureWindowsAutostart(const fs::path& executablePath, bool enabled) {
    if (executablePath.empty()) return InvalidArgument(L"The application path");

    HKEY key = nullptr;
    const auto openStatus = RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr,
        &key, nullptr);
    if (openStatus != ERROR_SUCCESS) {
        return CapabilityStatus::Failed(
            L"MineBackup could not open the current-user startup registry key (error "
            + to_wstring(openStatus) + L").");
    }

    vector<wstring> legacyValues;
    DWORD index = 0;
    for (;;) {
        wchar_t valueName[16384] = {};
        DWORD valueNameLength = static_cast<DWORD>(size(valueName));
        const auto enumStatus = RegEnumValueW(key, index, valueName, &valueNameLength,
            nullptr, nullptr, nullptr, nullptr);
        if (enumStatus == ERROR_NO_MORE_ITEMS) break;
        if (enumStatus != ERROR_SUCCESS) {
            RegCloseKey(key);
            return CapabilityStatus::Failed(
                L"MineBackup could not inspect existing startup entries (error "
                + to_wstring(enumStatus) + L").");
        }
        const wstring name(valueName, valueNameLength);
        if (name.rfind(L"MineBackup_AutoTask_", 0) == 0) legacyValues.push_back(name);
        ++index;
    }

    for (const auto& name : legacyValues) {
        const auto deleteStatus = RegDeleteValueW(key, name.c_str());
        if (deleteStatus != ERROR_SUCCESS && deleteStatus != ERROR_FILE_NOT_FOUND) {
            RegCloseKey(key);
            return CapabilityStatus::Failed(
                L"MineBackup could not remove a legacy startup entry (error "
                + to_wstring(deleteStatus) + L").");
        }
    }

    LSTATUS writeStatus = ERROR_SUCCESS;
    if (enabled) {
        const wstring command = L"\"" + executablePath.wstring() + L"\" --autostart";
        writeStatus = RegSetValueExW(key, L"MineBackup", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else {
        writeStatus = RegDeleteValueW(key, L"MineBackup");
        if (writeStatus == ERROR_FILE_NOT_FOUND) writeStatus = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (writeStatus != ERROR_SUCCESS) {
        return CapabilityStatus::Failed(
            L"MineBackup could not update its startup entry (error "
            + to_wstring(writeStatus) + L").");
    }
    return CapabilityStatus::Ready(
        enabled ? L"MineBackup will be started with --autostart."
                : L"MineBackup was removed from current-user startup.");
}
#endif

class NativeDesktopServices final : public DesktopServices {
public:
    explicit NativeDesktopServices(NativeDesktopContext context) : context_(std::move(context)) {}

    PlatformCapabilities Capabilities() const override {
        PlatformCapabilities result;
#ifdef _WIN32
        result.fileDialogs = CapabilityStatus::Ready();
        result.openUri = CapabilityStatus::Ready();
        result.notifications = CapabilityStatus::Unavailable(
            L"Native Windows notifications are not implemented yet.");
        result.tray = context_.messageWindow && context_.nativeInstance
            ? CapabilityStatus::Ready()
            : CapabilityStatus::Failed(L"The Windows tray message window is unavailable.");
        result.globalHotkeys = context_.messageWindow
            ? CapabilityStatus::Ready()
            : CapabilityStatus::Failed(L"The Windows hotkey message window is unavailable.");
        result.autostart = context_.autostartAllowed
            ? CapabilityStatus::Ready()
            : CapabilityStatus::Unavailable(
                L"Autostart is unavailable for an explicit --data-dir profile because the startup entry intentionally contains only --autostart.");
#elif defined(__APPLE__)
        result.fileDialogs = fs::exists("/usr/bin/osascript")
            ? CapabilityStatus::Ready()
            : CapabilityStatus::Unavailable(L"The macOS file-dialog helper is unavailable.");
        result.openUri = fs::exists("/usr/bin/open")
            ? CapabilityStatus::Ready()
            : CapabilityStatus::Unavailable(L"The macOS open service is unavailable.");
        result.notifications = CapabilityStatus::Unavailable(
            L"Native macOS notifications are not implemented yet.");
        result.tray = CapabilityStatus::Ready();
        result.globalHotkeys = CapabilityStatus::Ready();
        result.autostart = CapabilityStatus::PermissionRequired(
            L"Login-item integration will be provided through SMAppService.");
#else
        result.fileDialogs = (fs::exists("/usr/bin/zenity") || fs::exists("/usr/local/bin/zenity"))
            ? CapabilityStatus::Ready()
            : CapabilityStatus::Unavailable(L"Install zenity to enable file dialogs.");
        result.openUri = fs::exists("/usr/bin/xdg-open")
            ? CapabilityStatus::Ready()
            : CapabilityStatus::Unavailable(L"xdg-open is required to open links and folders.");
        result.notifications = CapabilityStatus::Unavailable(
            L"A desktop notification backend is not available in this build.");
#ifdef MB_HAVE_APPINDICATOR
        result.tray = CapabilityStatus::Ready();
#else
        result.tray = CapabilityStatus::Unavailable(
            L"This build does not include Ayatana AppIndicator support.");
#endif
        result.globalHotkeys = std::getenv("DISPLAY")
            ? CapabilityStatus::Ready(L"Global hotkeys currently use the X11 backend.")
            : CapabilityStatus::Unavailable(
                L"Global hotkeys require an X11 display until the Portal backend is available.");
        result.autostart = CapabilityStatus::Unavailable(
            L"Desktop autostart integration is not implemented on Linux yet.");
#endif
        result.windowActivation = context_.appWindow
            ? CapabilityStatus::Ready()
            : CapabilityStatus::Unavailable(L"The application window has not been created yet.");
        return result;
    }

    void SetNativeWindow(void* nativeWindow) override {
        context_.appWindow = nativeWindow;
    }

    DesktopPathResult SelectFile() override {
        const auto status = Capabilities().fileDialogs;
        if (!status.IsAvailable()) return {status, {}, false};
        const auto path = ::SelectFileDialog();
        return {status, fs::path(path), path.empty()};
    }

    DesktopPathResult SelectFolder() override {
        const auto status = Capabilities().fileDialogs;
        if (!status.IsAvailable()) return {status, {}, false};
        const auto path = ::SelectFolderDialog();
        return {status, fs::path(path), path.empty()};
    }

    DesktopPathResult SelectSaveFile(const wstring& defaultFileName, const wstring& filter) override {
        const auto status = Capabilities().fileDialogs;
        if (!status.IsAvailable()) return {status, {}, false};
        const auto path = ::SelectSaveFileDialog(defaultFileName, filter);
        return {status, fs::path(path), path.empty()};
    }

    CapabilityStatus OpenUri(const wstring& uri) override {
        const auto status = Capabilities().openUri;
        if (!status.IsAvailable()) return status;
        if (uri.empty()) return InvalidArgument(L"The URI");
        ::OpenLinkInBrowser(uri);
        return status;
    }

    CapabilityStatus OpenFolder(const fs::path& folder) override {
        const auto status = Capabilities().openUri;
        if (!status.IsAvailable()) return status;
        if (folder.empty()) return InvalidArgument(L"The folder path");
        ::OpenFolder(folder.wstring());
        return status;
    }

    CapabilityStatus RevealInFolder(const fs::path& folder, const fs::path& item) override {
        const auto status = Capabilities().openUri;
        if (!status.IsAvailable()) return status;
        if (folder.empty()) return InvalidArgument(L"The folder path");
#ifdef _WIN32
        const wstring focus = item.empty() ? wstring() : L"/select,\"" + item.wstring() + L"\"";
        ::OpenFolderWithFocus(folder.wstring(), focus);
#else
        ::OpenFolderWithFocus(folder.wstring(), item.wstring());
#endif
        return status;
    }

    CapabilityStatus Notify(const wstring&, const wstring&) override {
        return Capabilities().notifications;
    }

    CapabilityStatus SetTrayVisible(bool visible) override {
        const auto status = Capabilities().tray;
        if (!status.IsAvailable()) return status;
        if (visible == trayVisible_) return status;
#ifdef _WIN32
        if (visible) ::CreateTrayIcon(static_cast<HWND>(context_.messageWindow),
            static_cast<HINSTANCE>(context_.nativeInstance));
        else ::RemoveTrayIcon();
#else
        if (visible) ::CreateTrayIcon();
        else ::RemoveTrayIcon();
#endif
        trayVisible_ = visible;
        return status;
    }

    CapabilityStatus RegisterGlobalHotkey(int hotkeyId, int key) override {
        const auto status = Capabilities().globalHotkeys;
        if (!status.IsAvailable()) return status;
        if (key <= 0) return CapabilityStatus::Failed(L"The hotkey must be a valid key code.");
#ifdef _WIN32
        if (!::RegisterHotKey(static_cast<HWND>(context_.messageWindow), hotkeyId,
                MOD_ALT | MOD_CONTROL, static_cast<UINT>(key))) {
            return CapabilityStatus::Failed(
                L"The requested hotkey is already in use or could not be registered.");
        }
#else
        ::RegisterHotkeys(hotkeyId, key);
#endif
        registeredHotkeys_.insert(hotkeyId);
        return status;
    }

    CapabilityStatus UnregisterGlobalHotkey(int hotkeyId) override {
        const auto status = Capabilities().globalHotkeys;
        if (!status.IsAvailable()) return status;
        if (!registeredHotkeys_.erase(hotkeyId)) return status;
#ifdef _WIN32
        ::UnregisterHotKey(static_cast<HWND>(context_.messageWindow), hotkeyId);
#else
        ::UnregisterHotkeys(hotkeyId);
#endif
        return status;
    }

    CapabilityStatus SetAutostart(bool enabled) override {
        const auto status = Capabilities().autostart;
        if (!status.IsAvailable()) return status;
#ifdef _WIN32
        return ConfigureWindowsAutostart(context_.executablePath, enabled);
#else
        (void)enabled;
        return status;
#endif
    }

    CapabilityStatus ActivateWindow() override {
        const auto status = Capabilities().windowActivation;
        if (!status.IsAvailable()) return status;
        auto* window = static_cast<GLFWwindow*>(context_.appWindow);
        glfwShowWindow(window);
        glfwRestoreWindow(window);
        glfwFocusWindow(window);
        glfwPostEmptyEvent();
        return status;
    }

    CapabilityStatus RestartApplication() override {
#ifdef _WIN32
        ::ReStartApplication();
        return CapabilityStatus::Ready();
#elif defined(__APPLE__)
        ::ReStartApplication();
        return CapabilityStatus::PermissionRequired(
            L"Close and reopen MineBackup to finish switching modes.");
#else
        return CapabilityStatus::Unavailable(
            L"Automatic application restart is not available on this desktop.");
#endif
    }

private:
    NativeDesktopContext context_;
    bool trayVisible_ = false;
    set<int> registeredHotkeys_;
};

} // namespace

shared_ptr<DesktopServices> CreateNativeDesktopServices(NativeDesktopContext context) {
    return make_shared<NativeDesktopServices>(std::move(context));
}
