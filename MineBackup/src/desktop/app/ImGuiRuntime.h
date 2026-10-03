#pragma once

#include "ReadOnlyMappedFile.h"

#include <cstddef>
#include <functional>
#include <vector>

struct GLFWwindow;

class ImGuiRuntime {
public:
	ImGuiRuntime() = default;
	~ImGuiRuntime();

	ImGuiRuntime(const ImGuiRuntime&) = delete;
	ImGuiRuntime& operator=(const ImGuiRuntime&) = delete;

	void AdoptWindow(GLFWwindow* window) noexcept;
	void MarkImGuiContextCreated() noexcept;
	void MarkPlatformBackendInitialized() noexcept;
	void MarkRendererBackendInitialized() noexcept;
	void SetUiResourceReleaser(std::function<void()> releaser);
	void RetainFontSourceMapping(minebackup::infra::ReadOnlyMappedFile mapping);
	[[nodiscard]] std::size_t MappedFontBytes() const noexcept;
	[[nodiscard]] GLFWwindow* Window() const noexcept { return window_; }
	void Shutdown() noexcept;

private:
	std::function<void()> uiResourceReleaser_;
	std::vector<minebackup::infra::ReadOnlyMappedFile> fontSourceMappings_;
	GLFWwindow* window_ = nullptr;
	bool imguiContextCreated_ = false;
	bool platformBackendInitialized_ = false;
	bool rendererBackendInitialized_ = false;
};

bool LoadTextureFromFileGL(
	const char* filename,
	unsigned int* outTexture,
	int* outWidth,
	int* outHeight);
