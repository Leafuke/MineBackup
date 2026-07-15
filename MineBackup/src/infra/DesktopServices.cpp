#include "DesktopServices.h"

#include <mutex>
#include <utility>

using namespace std;

namespace {

class UnavailableDesktopServices final : public DesktopServices {
public:
    PlatformCapabilities Capabilities() const override {
        const auto unavailable = CapabilityStatus::Unavailable(
            L"The native desktop service has not been initialized.");
        return {unavailable, unavailable, unavailable, unavailable,
            unavailable, unavailable, unavailable};
    }

    void SetNativeWindow(void*) override {}

    DesktopPathResult SelectFile() override { return UnavailablePath(); }
    DesktopPathResult SelectFolder() override { return UnavailablePath(); }
    DesktopPathResult SelectSaveFile(const wstring&, const wstring&) override {
        return UnavailablePath();
    }
    CapabilityStatus OpenUri(const wstring&) override { return Unavailable(); }
    CapabilityStatus OpenFolder(const filesystem::path&) override { return Unavailable(); }
    CapabilityStatus RevealInFolder(const filesystem::path&, const filesystem::path&) override {
        return Unavailable();
    }
    CapabilityStatus Notify(const wstring&, const wstring&) override { return Unavailable(); }
    CapabilityStatus SetTrayVisible(bool) override { return Unavailable(); }
    CapabilityStatus ConfigureGlobalHotkeys(const vector<GlobalHotkeyBinding>&) override {
        return Unavailable();
    }
    CapabilityStatus SetAutostart(bool) override { return Unavailable(); }
    CapabilityStatus ActivateWindow() override { return Unavailable(); }
    CapabilityStatus RestartApplication() override { return Unavailable(); }

private:
    static CapabilityStatus Unavailable() {
        return CapabilityStatus::Unavailable(
            L"The native desktop service has not been initialized.");
    }
    static DesktopPathResult UnavailablePath() {
        return {Unavailable(), {}, false};
    }
};

mutex g_servicesMutex;
shared_ptr<DesktopServices> g_services = make_shared<UnavailableDesktopServices>();

} // namespace

CapabilityStatus CapabilityStatus::Ready(wstring diagnostic) {
    return {CapabilityState::Available, std::move(diagnostic)};
}

CapabilityStatus CapabilityStatus::Unavailable(wstring diagnostic) {
    return {CapabilityState::Unavailable, std::move(diagnostic)};
}

CapabilityStatus CapabilityStatus::PermissionRequired(wstring diagnostic) {
    return {CapabilityState::PermissionRequired, std::move(diagnostic)};
}

CapabilityStatus CapabilityStatus::Failed(wstring diagnostic) {
    return {CapabilityState::Failed, std::move(diagnostic)};
}

shared_ptr<DesktopServices> GetDesktopServices() {
    lock_guard lock(g_servicesMutex);
    return g_services;
}

void InstallDesktopServices(shared_ptr<DesktopServices> services) {
    lock_guard lock(g_servicesMutex);
    g_services = services ? std::move(services) : make_shared<UnavailableDesktopServices>();
}

void ResetDesktopServices() {
    InstallDesktopServices(nullptr);
}

bool CanHideToTray(const PlatformCapabilities& capabilities) noexcept {
    return capabilities.tray.IsAvailable();
}
