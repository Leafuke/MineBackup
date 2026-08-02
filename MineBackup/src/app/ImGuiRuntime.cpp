#define STB_IMAGE_IMPLEMENTATION
#include "ImGuiRuntime.h"

#include "imgui-all.h"

#include <utility>

ImGuiRuntime::~ImGuiRuntime()
{
	Shutdown();
}

void ImGuiRuntime::AdoptWindow(GLFWwindow* window) noexcept
{
	window_ = window;
}

void ImGuiRuntime::MarkImGuiContextCreated() noexcept
{
	imguiContextCreated_ = true;
}

void ImGuiRuntime::MarkPlatformBackendInitialized() noexcept
{
	platformBackendInitialized_ = true;
}

void ImGuiRuntime::MarkRendererBackendInitialized() noexcept
{
	rendererBackendInitialized_ = true;
}

void ImGuiRuntime::SetUiResourceReleaser(std::function<void()> releaser)
{
	uiResourceReleaser_ = std::move(releaser);
}

void ImGuiRuntime::RetainFontSourceMapping(
	minebackup::infra::ReadOnlyMappedFile mapping)
{
	fontSourceMappings_.push_back(std::move(mapping));
}

std::size_t ImGuiRuntime::MappedFontBytes() const noexcept
{
	std::size_t total = 0;
	for (const auto& mapping : fontSourceMappings_) total += mapping.Size();
	return total;
}

void ImGuiRuntime::Shutdown() noexcept
{
	// ImGui 控制器持有的纹理必须在 OpenGL 后端和 Context 之前释放。
	if (uiResourceReleaser_) {
		try {
			uiResourceReleaser_();
		}
		catch (...) {
			// 析构路径不允许异常越过平台入口。
		}
		uiResourceReleaser_ = {};
	}
	if (rendererBackendInitialized_) {
		ImGui_ImplOpenGL3_Shutdown();
		rendererBackendInitialized_ = false;
	}
	if (platformBackendInitialized_) {
		ImGui_ImplGlfw_Shutdown();
		platformBackendInitialized_ = false;
	}
	if (imguiContextCreated_) {
		ImGui::DestroyContext();
		imguiContextCreated_ = false;
	}
	// ImGui 1.92 rasterizes glyphs from source data on demand. The context and
	// its font atlas must be gone before mapped font files are closed.
	fontSourceMappings_.clear();
	if (window_ != nullptr) {
		glfwDestroyWindow(window_);
		window_ = nullptr;
	}
}

bool LoadTextureFromFileGL(const char* filename, GLuint* out_texture, int* out_width, int* out_height)
{
	int image_width = 0;
	int image_height = 0;
	unsigned char* image_data = stbi_load(filename, &image_width, &image_height, NULL, 4);
	if (image_data == NULL)
		return false;

	// 创建一个 OpenGL 纹理
	GLuint image_texture;
	glGenTextures(1, &image_texture);
	glBindTexture(GL_TEXTURE_2D, image_texture);

	// 设置纹理参数
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // 避免边缘伪影
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // 避免边缘伪影x2

#if defined(GL_UNPACK_ROW_LENGTH)
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); // 确保没有行对齐问题
#endif

	// 上传纹理数据
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_width, image_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
	stbi_image_free(image_data);

	*out_texture = image_texture;
	*out_width = image_width;
	*out_height = image_height;

	return true;
}
