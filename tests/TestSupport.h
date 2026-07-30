#pragma once

#include "DesktopServices.h"
#include "NetworkService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

struct TestContext {
	int failures = 0;

	void Expect(bool condition, const char* message) {
		if (condition) return;
		++failures;
		std::cerr << "[FAIL] " << message << '\n';
	}
};

struct TemporaryDirectory {
	std::filesystem::path path;

	TemporaryDirectory() {
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		// macOS 的 /var 是系统别名；先规范化，避免路径安全测试把它当作用户符号链接。
		path = std::filesystem::canonical(std::filesystem::temp_directory_path())
			/ ("MineBackupDataTests-" + std::to_string(stamp));
		std::filesystem::create_directories(path);
	}

	~TemporaryDirectory() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
};

inline std::string ReadText(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	return {
		std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>()};
}

class FakeNetworkBackend final : public NetworkBackend {
public:
	NetworkResult configuredResult{
		NetworkStatus::Succeeded,
		200,
		"https://example.test/final",
		0,
		{}};
	std::string body;
	std::size_t syntheticChunkSize = 0;

	NetworkResult Get(
		const NetworkRequest&,
		const NetworkChunkSink& sink,
		std::stop_token stopToken) override {
		if (stopToken.stop_requested()) {
			auto cancelled = configuredResult;
			cancelled.status = NetworkStatus::Cancelled;
			return cancelled;
		}
		if (configuredResult.status != NetworkStatus::Succeeded) return configuredResult;
		const std::size_t size = syntheticChunkSize ? syntheticChunkSize : body.size();
		if (size && !sink(body.empty() ? "x" : body.data(), size)) {
			auto rejected = configuredResult;
			rejected.status = NetworkStatus::SinkRejected;
			return rejected;
		}
		auto result = configuredResult;
		result.transferredBytes = size;
		return result;
	}
};

class MockDesktopServices final : public DesktopServices {
public:
	PlatformCapabilities capabilities{
		CapabilityStatus::Ready(),
		CapabilityStatus::Ready(),
		CapabilityStatus::Unavailable(L"notifications disabled by test"),
		CapabilityStatus::PermissionRequired(L"tray permission required"),
		CapabilityStatus::Unavailable(L"hotkeys disabled by test"),
		CapabilityStatus::Ready(),
		CapabilityStatus::Ready()};
	std::filesystem::path selectedPath = L"mock/selected.txt";
	bool autostartEnabled = false;
	int autostartSettingsOpenCount = 0;
	int activationCount = 0;
	std::vector<GlobalHotkeyBinding> configuredHotkeys;
	void* window = nullptr;

	PlatformCapabilities Capabilities() const override { return capabilities; }
	void SetNativeWindow(void* nativeWindow) override { window = nativeWindow; }
	DesktopPathResult SelectFile() override {
		return {capabilities.fileDialogs, selectedPath, false};
	}
	DesktopPathResult SelectFolder() override {
		return {capabilities.fileDialogs, selectedPath.parent_path(), false};
	}
	DesktopPathResult SelectSaveFile(const std::wstring&, const std::wstring&) override {
		return {capabilities.fileDialogs, selectedPath, false};
	}
	CapabilityStatus OpenUri(const std::wstring&) override { return capabilities.openUri; }
	CapabilityStatus OpenFolder(const std::filesystem::path&) override {
		return capabilities.openUri;
	}
	CapabilityStatus RevealInFolder(
		const std::filesystem::path&,
		const std::filesystem::path&) override {
		return capabilities.openUri;
	}
	CapabilityStatus Notify(const std::wstring&, const std::wstring&) override {
		return capabilities.notifications;
	}
	CapabilityStatus SetTrayVisible(bool) override { return capabilities.tray; }
	CapabilityStatus ConfigureGlobalHotkeys(
		const std::vector<GlobalHotkeyBinding>& bindings) override {
		configuredHotkeys = bindings;
		return capabilities.globalHotkeys;
	}
	CapabilityStatus SetAutostart(bool enabled) override {
		autostartEnabled = enabled;
		return capabilities.autostart;
	}
	CapabilityStatus OpenAutostartSettings() override {
		++autostartSettingsOpenCount;
		return capabilities.autostart;
	}
	CapabilityStatus ActivateWindow() override {
		++activationCount;
		return capabilities.windowActivation;
	}
	CapabilityStatus RestartApplication() override {
		return capabilities.windowActivation;
	}
};
