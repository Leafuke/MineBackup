#pragma once

#include <filesystem>
#include <memory>
#include <string>

enum class CapabilityState {
    Available,
    Unavailable,
    PermissionRequired,
    Failed
};

struct CapabilityStatus {
    CapabilityState state = CapabilityState::Unavailable;
    std::wstring diagnostic;

    [[nodiscard]] bool IsAvailable() const noexcept {
        return state == CapabilityState::Available;
    }

    static CapabilityStatus Ready(std::wstring diagnostic = {});
    static CapabilityStatus Unavailable(std::wstring diagnostic);
    static CapabilityStatus PermissionRequired(std::wstring diagnostic);
    static CapabilityStatus Failed(std::wstring diagnostic);
};

struct PlatformCapabilities {
    CapabilityStatus fileDialogs;
    CapabilityStatus openUri;
    CapabilityStatus notifications;
    CapabilityStatus tray;
    CapabilityStatus globalHotkeys;
    CapabilityStatus autostart;
    CapabilityStatus windowActivation;
};

struct DesktopPathResult {
    CapabilityStatus status;
    std::filesystem::path path;
    bool cancelled = false;
};

class DesktopServices {
public:
    virtual ~DesktopServices() = default;

    [[nodiscard]] virtual PlatformCapabilities Capabilities() const = 0;
    virtual void SetNativeWindow(void* nativeWindow) = 0;

    [[nodiscard]] virtual DesktopPathResult SelectFile() = 0;
    [[nodiscard]] virtual DesktopPathResult SelectFolder() = 0;
    [[nodiscard]] virtual DesktopPathResult SelectSaveFile(
        const std::wstring& defaultFileName = {}, const std::wstring& filter = {}) = 0;
    [[nodiscard]] virtual CapabilityStatus OpenUri(const std::wstring& uri) = 0;
    [[nodiscard]] virtual CapabilityStatus OpenFolder(const std::filesystem::path& folder) = 0;
    [[nodiscard]] virtual CapabilityStatus RevealInFolder(
        const std::filesystem::path& folder, const std::filesystem::path& item) = 0;
    [[nodiscard]] virtual CapabilityStatus Notify(
        const std::wstring& title, const std::wstring& message) = 0;
    [[nodiscard]] virtual CapabilityStatus SetTrayVisible(bool visible) = 0;
    [[nodiscard]] virtual CapabilityStatus RegisterGlobalHotkey(int hotkeyId, int key) = 0;
    [[nodiscard]] virtual CapabilityStatus UnregisterGlobalHotkey(int hotkeyId) = 0;
    [[nodiscard]] virtual CapabilityStatus SetAutostart(bool enabled) = 0;
    [[nodiscard]] virtual CapabilityStatus ActivateWindow() = 0;
    [[nodiscard]] virtual CapabilityStatus RestartApplication() = 0;
};

// The registry always owns a service instance. Before the native desktop layer is
// installed, callers receive an honest unavailable implementation instead of null.
[[nodiscard]] std::shared_ptr<DesktopServices> GetDesktopServices();
void InstallDesktopServices(std::shared_ptr<DesktopServices> services);
void ResetDesktopServices();

[[nodiscard]] bool CanHideToTray(const PlatformCapabilities& capabilities) noexcept;
