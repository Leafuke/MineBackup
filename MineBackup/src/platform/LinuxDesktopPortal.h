#pragma once

#include "DesktopServices.h"

#include <functional>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct LinuxPortalShortcutBinding {
    int hotkeyId = 0;
    std::string shortcutId;
    std::wstring description;
    std::string preferredTrigger;
};

struct LinuxPortalShortcutResult {
    CapabilityStatus status;
    std::map<int, std::wstring> actualTriggers;
};

[[nodiscard]] CapabilityStatus ProbeLinuxPortalInterface(const char* interfaceName);
[[nodiscard]] CapabilityStatus ProbeLinuxStatusNotifierHost();
[[nodiscard]] CapabilityStatus OpenUriWithLinuxPortal(const std::wstring& uri);
[[nodiscard]] CapabilityStatus OpenPathWithLinuxPortal(
    const std::filesystem::path& path, bool revealInFolder);
[[nodiscard]] CapabilityStatus NotifyWithLinuxPortal(
    const std::wstring& title, const std::wstring& message);
[[nodiscard]] LinuxPortalShortcutResult ConfigureLinuxPortalShortcuts(
    const std::vector<LinuxPortalShortcutBinding>& bindings,
    std::function<void(int)> activatedCallback);
void ShutdownLinuxDesktopPortal();
