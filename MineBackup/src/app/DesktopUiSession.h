#pragma once

#include "AppPaths.h"
#include "ImGuiRuntime.h"

#include <cstddef>
#include <filesystem>
#include <string>

struct GLFWwindow;

struct DesktopUiSessionOptions {
	const AppPaths* paths = nullptr;
	int width = 1280;
	int height = 720;
	bool initiallyVisible = true;
	bool firstRun = false;
	void* nativeInstance = nullptr;
	bool iconFontAvailable = false;
	const void* bundledIconFontData = nullptr;
	std::size_t bundledIconFontSize = 0;
	std::filesystem::path extractedIconFontPath;
};

class DesktopUiSession {
public:
	DesktopUiSession() = default;
	~DesktopUiSession();

	DesktopUiSession(const DesktopUiSession&) = delete;
	DesktopUiSession& operator=(const DesktopUiSession&) = delete;

	bool Create(const DesktopUiSessionOptions& options, std::wstring& error);
	void DestroySecondaryPlatformWindows() noexcept;
	void SaveWindowState(int& width, int& height) noexcept;
	void Shutdown() noexcept;

	[[nodiscard]] bool IsActive() const noexcept { return active_; }
	[[nodiscard]] GLFWwindow* Window() const noexcept { return runtime_.Window(); }
	[[nodiscard]] std::size_t MappedFontBytes() const noexcept {
		return runtime_.MappedFontBytes();
	}
	[[nodiscard]] double LastCreateMilliseconds() const noexcept {
		return lastCreateMilliseconds_;
	}

private:
	ImGuiRuntime runtime_;
	std::string iniPath_;
	std::string logPath_;
	bool active_ = false;
	double lastCreateMilliseconds_ = 0.0;
};
