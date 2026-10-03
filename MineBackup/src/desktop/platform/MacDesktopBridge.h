#pragma once

#include "DesktopServices.h"

#include <filesystem>
#include <string>

[[nodiscard]] DesktopPathResult MacSelectFile();
[[nodiscard]] DesktopPathResult MacSelectFolder();
[[nodiscard]] DesktopPathResult MacSelectSaveFile(
    const std::wstring& defaultFileName, const std::wstring& filter);

[[nodiscard]] CapabilityStatus MacOpenUri(const std::wstring& uri);
[[nodiscard]] CapabilityStatus MacOpenFolder(const std::filesystem::path& folder);
[[nodiscard]] CapabilityStatus MacRevealInFolder(
    const std::filesystem::path& folder, const std::filesystem::path& item);
[[nodiscard]] CapabilityStatus MacNotificationCapability();
[[nodiscard]] CapabilityStatus MacNotify(
    const std::wstring& title, const std::wstring& message);
[[nodiscard]] CapabilityStatus MacAutostartCapability();
[[nodiscard]] CapabilityStatus MacSetAutostart(bool enabled);
[[nodiscard]] CapabilityStatus MacOpenAutostartSettings();
[[nodiscard]] CapabilityStatus MacActivateApplication();

void MacShowAlert(const std::string& title, const std::string& message, int iconType);
[[nodiscard]] bool MacConfirmAlert(const std::string& title, const std::string& message);
