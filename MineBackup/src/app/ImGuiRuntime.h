#pragma once

#include <functional>

struct GLFWwindow;

class ImGuiRuntime {
public:
	ImGuiRuntime() = default;
	~ImGuiRuntime();

	ImGuiRuntime(const ImGuiRuntime&) = delete;
	ImGuiRuntime& operator=(const ImGuiRuntime&) = delete;

	void MarkGlfwInitialized() noexcept;
	void AdoptWindow(GLFWwindow* window) noexcept;
	void MarkImGuiContextCreated() noexcept;
	void MarkPlatformBackendInitialized() noexcept;
	void MarkRendererBackendInitialized() noexcept;
	void SetUiResourceReleaser(std::function<void()> releaser);
	void Shutdown() noexcept;

private:
	std::function<void()> uiResourceReleaser_;
	GLFWwindow* window_ = nullptr;
	bool glfwInitialized_ = false;
	bool imguiContextCreated_ = false;
	bool platformBackendInitialized_ = false;
	bool rendererBackendInitialized_ = false;
};

bool LoadTextureFromFileGL(
	const char* filename,
	unsigned int* outTexture,
	int* outWidth,
	int* outHeight);
