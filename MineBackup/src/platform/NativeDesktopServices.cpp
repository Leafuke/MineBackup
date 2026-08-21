#include "NativeDesktopServices.h"

#include "PlatformCompat.h"
#ifdef __linux__
#include "LinuxDesktopPortal.h"
#elif defined(__APPLE__)
#include "MacDesktopBridge.h"
#endif

#include <GLFW/glfw3.h>

#include <cstdlib>
#include <cctype>
#include <map>
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

    DWORD existingType = REG_NONE;
    DWORD existingBytes = 0;
    LSTATUS queryStatus = RegQueryValueExW(key, L"MineBackup", nullptr,
        &existingType, nullptr, &existingBytes);
    bool existingValue = queryStatus == ERROR_SUCCESS;
    wstring existingCommand;
    if (existingValue) {
        vector<wchar_t> value(existingBytes / sizeof(wchar_t) + 1, L'\0');
        queryStatus = RegQueryValueExW(key, L"MineBackup", nullptr,
            &existingType, reinterpret_cast<BYTE*>(value.data()), &existingBytes);
        if (queryStatus != ERROR_SUCCESS) {
            RegCloseKey(key);
            return CapabilityStatus::Failed(
                L"MineBackup could not read its existing startup entry (error "
                + to_wstring(queryStatus) + L").");
        }
        const size_t characterCount = existingBytes / sizeof(wchar_t);
        existingCommand.assign(value.data(), characterCount);
        while (!existingCommand.empty() && existingCommand.back() == L'\0') {
            existingCommand.pop_back();
        }
    }
    else if (queryStatus != ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return CapabilityStatus::Failed(
            L"MineBackup could not inspect its existing startup entry (error "
            + to_wstring(queryStatus) + L").");
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
    wstring diagnostic;
    if (enabled) {
        // --autostart 是 GUI 登录启动的内部标记；特殊任务由 CLI 的
        // Job 由外部调度器调用 CLI；GUI 登录启动项不携带任务配置。
        const wstring command = L"\"" + executablePath.wstring() + L"\" --autostart";
        const bool unchanged = existingValue && existingType == REG_SZ
            && existingCommand.size() == command.size()
            && _wcsicmp(existingCommand.c_str(), command.c_str()) == 0;
        if (!unchanged) {
            writeStatus = RegSetValueExW(key, L"MineBackup", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            diagnostic = existingValue
                ? L"MineBackup repaired its current-user startup entry after the application path or arguments changed."
                : L"MineBackup created its current-user startup entry.";
        }
    }
    else {
        if (existingValue) {
            writeStatus = RegDeleteValueW(key, L"MineBackup");
            if (writeStatus == ERROR_FILE_NOT_FOUND) writeStatus = ERROR_SUCCESS;
            if (writeStatus == ERROR_SUCCESS) {
                diagnostic = L"MineBackup removed its obsolete current-user startup entry.";
            }
        }
    }
    RegCloseKey(key);
    if (writeStatus != ERROR_SUCCESS) {
        return CapabilityStatus::Failed(
            L"MineBackup could not update its startup entry (error "
            + to_wstring(writeStatus) + L").");
    }
    if (!legacyValues.empty()) {
        if (!diagnostic.empty()) diagnostic += L" ";
        diagnostic += L"Legacy per-task startup entries were removed.";
    }
    return CapabilityStatus::Ready(std::move(diagnostic));
}
#endif

class NativeDesktopServices final : public DesktopServices {
public:
    explicit NativeDesktopServices(NativeDesktopContext context) : context_(std::move(context)) {}

    ~NativeDesktopServices() override {
        if (trayVisible_) ::RemoveTrayIcon();
        for (const auto& [hotkeyId, key] : registeredHotkeys_) {
            (void)key;
#ifdef _WIN32
            ::UnregisterHotKey(static_cast<HWND>(context_.messageWindow), hotkeyId);
#else
            ::UnregisterHotkeys(hotkeyId);
#endif
        }
        registeredHotkeys_.clear();
#ifdef __linux__
        ShutdownLinuxDesktopPortal();
#endif
    }

    PlatformCapabilities Capabilities() const override {
        PlatformCapabilities result;
#ifdef _WIN32
        result.fileDialogs = CapabilityStatus::Ready();
        result.openUri = CapabilityStatus::Ready();
        result.notifications = context_.messageWindow
            ? CapabilityStatus::Ready(L"Notifications use the native Windows tray balloon service.")
            : CapabilityStatus::Failed(L"The Windows notification message window is unavailable.");
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
        result.fileDialogs = CapabilityStatus::Ready(
            L"File dialogs use native NSOpenPanel and NSSavePanel services.");
        result.openUri = CapabilityStatus::Ready(
            L"Links and files use the native NSWorkspace service.");
        result.notifications = MacNotificationCapability();
        result.tray = CapabilityStatus::Ready(L"The menu-bar item uses NSStatusItem.");
        result.globalHotkeys = CapabilityStatus::Ready(
            L"Global hotkeys use the isolated macOS EventHotKey bridge.");
        result.autostart = context_.autostartAllowed
            ? MacAutostartCapability()
            : CapabilityStatus::Unavailable(
                L"Autostart is unavailable for an explicit --data-dir profile because the login item opens the default profile.");
#else
#ifdef MB_HAVE_GTK
        result.fileDialogs = CapabilityStatus::Ready(
            L"File dialogs use GtkFileChooserNative and the desktop portal when required.");
#else
        result.fileDialogs = CapabilityStatus::Unavailable(
            L"This build does not include GTK3 native file-dialog support.");
#endif
        result.openUri = ProbeLinuxPortalInterface("org.freedesktop.portal.OpenURI");
        result.notifications = ProbeLinuxPortalInterface(
            "org.freedesktop.portal.Notification");
#ifdef MB_HAVE_APPINDICATOR
        result.tray = ProbeLinuxStatusNotifierHost();
#else
        result.tray = CapabilityStatus::Unavailable(
            L"This build does not include Ayatana AppIndicator support.");
#endif
        if (!context_.appWindow) {
            result.globalHotkeys = CapabilityStatus::Unavailable(
                L"The GLFW desktop backend has not been selected yet.");
        }
        else if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
            result.globalHotkeys = CapabilityStatus::Ready(
                L"Global hotkeys use the X11 backend selected by GLFW.");
        }
        else if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            result.globalHotkeys = portalHotkeyStatus_.diagnostic.empty()
                ? ProbeLinuxPortalInterface("org.freedesktop.portal.GlobalShortcuts")
                : portalHotkeyStatus_;
        }
        else {
            result.globalHotkeys = CapabilityStatus::Unavailable(
                L"Global hotkeys are unavailable on the selected GLFW backend.");
        }
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
#ifdef __APPLE__
        return MacSelectFile();
#else
        const auto status = Capabilities().fileDialogs;
        if (!status.IsAvailable()) return {status, {}, false};
        const auto path = ::SelectFileDialog();
        return {status, fs::path(path), path.empty()};
#endif
    }

    DesktopPathResult SelectFolder() override {
#ifdef __APPLE__
        return MacSelectFolder();
#else
        const auto status = Capabilities().fileDialogs;
        if (!status.IsAvailable()) return {status, {}, false};
        const auto path = ::SelectFolderDialog();
        return {status, fs::path(path), path.empty()};
#endif
    }

    DesktopPathResult SelectSaveFile(const wstring& defaultFileName, const wstring& filter) override {
#ifdef __APPLE__
        return MacSelectSaveFile(defaultFileName, filter);
#else
        const auto status = Capabilities().fileDialogs;
        if (!status.IsAvailable()) return {status, {}, false};
        const auto path = ::SelectSaveFileDialog(defaultFileName, filter);
        return {status, fs::path(path), path.empty()};
#endif
    }

    CapabilityStatus OpenUri(const wstring& uri) override {
        const auto status = Capabilities().openUri;
        if (!status.IsAvailable()) return status;
        if (uri.empty()) return InvalidArgument(L"The URI");
#ifdef __linux__
        return OpenUriWithLinuxPortal(uri);
#elif defined(__APPLE__)
        return MacOpenUri(uri);
#else
        ::OpenLinkInBrowser(uri);
        return status;
#endif
    }

    CapabilityStatus OpenFolder(const fs::path& folder) override {
        const auto status = Capabilities().openUri;
        if (!status.IsAvailable()) return status;
        if (folder.empty()) return InvalidArgument(L"The folder path");
#ifdef __linux__
        return OpenPathWithLinuxPortal(folder, false);
#elif defined(__APPLE__)
        return MacOpenFolder(folder);
#else
        ::OpenFolder(folder.wstring());
        return status;
#endif
    }

    CapabilityStatus RevealInFolder(const fs::path& folder, const fs::path& item) override {
        const auto status = Capabilities().openUri;
        if (!status.IsAvailable()) return status;
        if (folder.empty()) return InvalidArgument(L"The folder path");
#if defined(__linux__)
        return OpenPathWithLinuxPortal(item.empty() ? folder : item, true);
#elif defined(_WIN32)
        const wstring focus = item.empty() ? wstring() : L"/select,\"" + item.wstring() + L"\"";
        ::OpenFolderWithFocus(folder.wstring(), focus);
#elif defined(__APPLE__)
        return MacRevealInFolder(folder, item);
#else
        ::OpenFolderWithFocus(folder.wstring(), item.wstring());
#endif
        return status;
    }

    CapabilityStatus Notify(const wstring& title, const wstring& message) override {
#ifdef __linux__
        return NotifyWithLinuxPortal(title, message);
#elif defined(__APPLE__)
        return MacNotify(title, message);
#elif defined(_WIN32)
        const auto status = Capabilities().notifications;
        if (!status.IsAvailable()) return status;
        if (title.empty() || message.empty()) {
            return CapabilityStatus::Failed(
                L"Notification title and message must not be empty.");
        }
        return ::ShowTrayNotification(title, message)
            ? status
            : CapabilityStatus::Failed(
                L"Windows could not display the tray notification. Ensure the tray icon is available.");
#else
        (void)title;
        (void)message;
        return Capabilities().notifications;
#endif
    }

    CapabilityStatus SetTrayVisible(bool visible) override {
        const auto status = Capabilities().tray;
        if (!visible) {
            if (!trayVisible_) return status;
            ::RemoveTrayIcon();
            trayVisible_ = false;
            return CapabilityStatus::Ready();
        }
        if (!status.IsAvailable()) return status;
        if (trayVisible_) return status;
#ifdef _WIN32
        if (!::CreateTrayIcon(static_cast<HWND>(context_.messageWindow),
                static_cast<HINSTANCE>(context_.nativeInstance))) {
            return CapabilityStatus::Failed(
                L"The native Windows tray icon could not be created.");
        }
#elif defined(__APPLE__)
        if (!::CreateTrayIcon()) {
            return CapabilityStatus::Failed(
                L"The native macOS menu-bar item could not be created.");
        }
#else
        if (!::CreateTrayIcon()) {
            return CapabilityStatus::Failed(
                L"Ayatana AppIndicator could not initialize in this desktop session.");
        }
#endif
        trayVisible_ = true;
        return status;
    }

    CapabilityStatus ConfigureGlobalHotkeys(
        const vector<GlobalHotkeyBinding>& bindings) override {
        set<int> hotkeyIds;
        set<int> keys;
        for (const auto& binding : bindings) {
            if (binding.hotkeyId <= 0 || binding.key <= 0) {
                return CapabilityStatus::Failed(
                    L"Every global hotkey must have a valid identifier and key code.");
            }
            if (!hotkeyIds.insert(binding.hotkeyId).second) {
                return CapabilityStatus::Failed(
                    L"Global hotkey identifiers must be unique.");
            }
            if (!keys.insert(binding.key).second) {
                return CapabilityStatus::Failed(
                    L"Each action must use a different global hotkey.");
            }
        }
#ifdef __linux__
        if (context_.appWindow && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            vector<LinuxPortalShortcutBinding> portalBindings;
            portalBindings.reserve(bindings.size());
            for (const auto& binding : bindings) {
                string preferred = "CTRL+ALT+";
                preferred.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(binding.key))));
                portalBindings.push_back({binding.hotkeyId,
                    binding.hotkeyId == MINEBACKUP_HOTKEY_ID ? "backup"
                        : binding.hotkeyId == MINERESTORE_HOTKEY_ID ? "restore"
                        : "hotkey_" + to_string(binding.hotkeyId),
                    binding.description, std::move(preferred)});
            }
            auto result = ConfigureLinuxPortalShortcuts(portalBindings, [](int hotkeyId) {
                if (hotkeyId == MINEBACKUP_HOTKEY_ID) TriggerHotkeyBackup();
                else if (hotkeyId == MINERESTORE_HOTKEY_ID) TriggerHotkeyRestore();
            });
            portalHotkeyStatus_ = result.status;
            registeredHotkeys_.clear();
            if (result.status.IsAvailable()) {
                for (const auto& binding : bindings) {
                    if (result.actualTriggers.count(binding.hotkeyId)) {
                        registeredHotkeys_[binding.hotkeyId] = binding.key;
                    }
                }
            }
            return result.status;
        }
#endif

        const auto status = Capabilities().globalHotkeys;
        if (!bindings.empty() && !status.IsAvailable()) return status;

        const auto previous = registeredHotkeys_;
        for (const auto& [hotkeyId, key] : registeredHotkeys_) {
            (void)key;
#ifdef _WIN32
            ::UnregisterHotKey(static_cast<HWND>(context_.messageWindow), hotkeyId);
#else
            ::UnregisterHotkeys(hotkeyId);
#endif
        }
        registeredHotkeys_.clear();

        for (const auto& binding : bindings) {
#ifdef _WIN32
            if (!::RegisterHotKey(static_cast<HWND>(context_.messageWindow), binding.hotkeyId,
                    MOD_ALT | MOD_CONTROL, static_cast<UINT>(binding.key))) {
                for (const auto& [hotkeyId, key] : registeredHotkeys_) {
                    (void)key;
                    ::UnregisterHotKey(static_cast<HWND>(context_.messageWindow), hotkeyId);
                }
                registeredHotkeys_.clear();
                for (const auto& [hotkeyId, key] : previous) {
                    if (::RegisterHotKey(static_cast<HWND>(context_.messageWindow), hotkeyId,
                            MOD_ALT | MOD_CONTROL, static_cast<UINT>(key))) {
                        registeredHotkeys_[hotkeyId] = key;
                    }
                }
                return CapabilityStatus::Failed(
                    L"A requested hotkey is already in use; the previous bindings were restored.");
            }
#elif defined(__linux__)
            if (!::RegisterHotkeys(binding.hotkeyId, binding.key)) {
                for (const auto& [hotkeyId, key] : registeredHotkeys_) {
                    (void)key;
                    ::UnregisterHotkeys(hotkeyId);
                }
                registeredHotkeys_.clear();
                for (const auto& [hotkeyId, key] : previous) {
                    if (::RegisterHotkeys(hotkeyId, key)) {
                        registeredHotkeys_[hotkeyId] = key;
                    }
                }
                return CapabilityStatus::Failed(
                    L"The X11 hotkey backend could not register the requested key; the previous bindings were restored.");
            }
#elif defined(__APPLE__)
            if (!::RegisterHotkeys(binding.hotkeyId, binding.key)) {
                for (const auto& [hotkeyId, key] : registeredHotkeys_) {
                    (void)key;
                    ::UnregisterHotkeys(hotkeyId);
                }
                registeredHotkeys_.clear();
                for (const auto& [hotkeyId, key] : previous) {
                    if (::RegisterHotkeys(hotkeyId, key)) {
                        registeredHotkeys_[hotkeyId] = key;
                    }
                }
                return CapabilityStatus::Failed(
                    L"A macOS global hotkey could not be registered; the previous bindings were restored.");
            }
#else
            ::RegisterHotkeys(binding.hotkeyId, binding.key);
#endif
            registeredHotkeys_[binding.hotkeyId] = binding.key;
        }
        return status;
    }

    CapabilityStatus SetAutostart(bool enabled) override {
#ifdef __APPLE__
        if (!context_.autostartAllowed) return Capabilities().autostart;
        return MacSetAutostart(enabled);
#else
        const auto status = Capabilities().autostart;
        if (!status.IsAvailable()) return status;
#ifdef _WIN32
        return ConfigureWindowsAutostart(context_.executablePath, enabled);
#else
        (void)enabled;
        return status;
#endif
#endif
    }

    CapabilityStatus OpenAutostartSettings() override {
#ifdef __APPLE__
        return MacOpenAutostartSettings();
#else
        return CapabilityStatus::Unavailable(
            L"This platform does not provide a separate autostart settings entry.");
#endif
    }

    CapabilityStatus ActivateWindow() override {
        const auto status = Capabilities().windowActivation;
        if (!status.IsAvailable()) return status;
#ifdef __APPLE__
        const auto activation = MacActivateApplication();
        if (!activation.IsAvailable()) return activation;
#endif
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
    map<int, int> registeredHotkeys_;
    CapabilityStatus portalHotkeyStatus_;
};

} // namespace

shared_ptr<DesktopServices> CreateNativeDesktopServices(NativeDesktopContext context) {
    return make_shared<NativeDesktopServices>(std::move(context));
}
