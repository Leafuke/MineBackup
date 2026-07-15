#pragma once

#include "DesktopServices.h"

#include <filesystem>
#include <memory>

struct NativeDesktopContext {
    void* nativeInstance = nullptr;
    void* messageWindow = nullptr;
    void* appWindow = nullptr;
    std::filesystem::path executablePath;
    bool autostartAllowed = false;
};

[[nodiscard]] std::shared_ptr<DesktopServices> CreateNativeDesktopServices(
    NativeDesktopContext context);
