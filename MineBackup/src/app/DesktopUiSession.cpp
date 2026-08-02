#include "DesktopUiSession.h"

#include "AppearanceRuntime.h"
#include "Globals.h"
#include "IconsFontAwesome6.h"
#include "Logging.h"
#include "MainUI.h"
#include "SettingsUI.h"
#include "i18n.h"
#include "imgui-all.h"
#include "imgui_style.h"
#include "text_to_text.h"

#include <chrono>
#include <cstdio>
#include <limits>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace std;

#define APP_PRINTF_INFO(eventId, ...) \
	MB_LOG_PRINTF_INFO(minebackup::logging::LogCategory::Application, eventId, __VA_ARGS__)
#define APP_PRINTF_WARNING(eventId, ...) \
	MB_LOG_PRINTF_WARNING(minebackup::logging::LogCategory::Application, eventId, __VA_ARGS__)

namespace {

void ApplyLinuxWindowIdentity() {
#ifdef __linux__
	glfwWindowHintString(GLFW_WAYLAND_APP_ID, "io.github.leafuke.MineBackup");
	glfwWindowHintString(GLFW_X11_CLASS_NAME, "MineBackup");
	glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "minebackup");
#endif
}

void ApplyCommonWindowHints(bool visible, bool firstRun) {
	glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
#ifndef _WIN32
#ifdef GLFW_FOCUS_ON_SHOW
	if (firstRun) glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
#endif
#else
	(void)firstRun;
#endif
	ApplyLinuxWindowIdentity();
}

GLFWwindow* CreateWindowWithFallbacks(
	int width,
	int height,
	bool visible,
	bool firstRun,
	const char*& glslVersion) {
	glfwDefaultWindowHints();
#if defined(IMGUI_IMPL_OPENGL_ES2)
	glslVersion = "#version 100";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(IMGUI_IMPL_OPENGL_ES3)
	glslVersion = "#version 300 es";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
	glslVersion = "#version 150";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
	glslVersion = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
	ApplyCommonWindowHints(visible, firstRun);
	GLFWwindow* window = glfwCreateWindow(width, height, "MineBackup", nullptr, nullptr);

#if !defined(IMGUI_IMPL_OPENGL_ES2) && !defined(IMGUI_IMPL_OPENGL_ES3) && !defined(__APPLE__)
	struct Fallback {
		int major;
		int minor;
		const char* glsl;
	};
	constexpr Fallback fallbacks[] = {
		{2, 1, "#version 120"},
		{2, 0, "#version 110"},
		{1, 1, "#version 110"},
	};
	for (const Fallback& fallback : fallbacks) {
		if (window != nullptr) break;
		fprintf(stderr, "OpenGL context creation failed, trying %d.%d fallback...\n",
			fallback.major, fallback.minor);
		glfwDefaultWindowHints();
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, fallback.major);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, fallback.minor);
		ApplyCommonWindowHints(visible, firstRun);
		glslVersion = fallback.glsl;
		window = glfwCreateWindow(width, height, "MineBackup", nullptr, nullptr);
	}
#endif
	return window;
}

#ifdef _WIN32
void ApplyWindowsWindowIcon(GLFWwindow* window, void* nativeInstance) {
	const HINSTANCE instance = static_cast<HINSTANCE>(nativeInstance);
	if (instance == nullptr) return;
	HRSRC groupResource = FindResourceW(
		instance, MAKEINTRESOURCEW(102), reinterpret_cast<LPCWSTR>(RT_GROUP_ICON));
	if (groupResource == nullptr) return;
	HGLOBAL groupMemory = LoadResource(instance, groupResource);
	const auto* groupData = static_cast<const BYTE*>(LockResource(groupMemory));
	if (groupData == nullptr) return;
	const int iconId = LookupIconIdFromDirectoryEx(
		const_cast<PBYTE>(groupData), TRUE, 0, 0, LR_DEFAULTCOLOR);
	HRSRC iconResource = FindResourceW(
		instance, MAKEINTRESOURCEW(iconId), reinterpret_cast<LPCWSTR>(RT_ICON));
	if (iconResource == nullptr) return;
	HGLOBAL iconMemory = LoadResource(instance, iconResource);
	const auto* iconData = static_cast<const stbi_uc*>(LockResource(iconMemory));
	const DWORD iconSize = SizeofResource(instance, iconResource);
	if (iconData == nullptr || iconSize == 0) return;

	int width = 0;
	int height = 0;
	int channels = 0;
	unsigned char* pixels = stbi_load_from_memory(
		iconData, static_cast<int>(iconSize), &width, &height, &channels, 4);
	if (pixels == nullptr) return;
	GLFWimage image{width, height, pixels};
	glfwSetWindowIcon(window, 1, &image);
	stbi_image_free(pixels);
}
#endif

void LoadFonts(const DesktopUiSessionOptions& options, ImGuiRuntime& runtime) {
	ImGuiIO& io = ImGui::GetIO();
	static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};

	if (!Fontss.empty() && filesystem::exists(Fontss)) {
		ImFontConfig config;
		config.PixelSnapH = true;
		if (options.iconFontAvailable) config.GlyphExcludeRanges = iconRanges;

		ImFont* mainFont = nullptr;
		minebackup::infra::ReadOnlyMappedFile mappedFont;
		error_code mappingError;
		const bool mapped = mappedFont.Open(Fontss, mappingError);
		if (mapped && mappedFont.Size() > 100
			&& mappedFont.Size() <= static_cast<size_t>((numeric_limits<int>::max)())) {
			config.FontDataOwnedByAtlas = false;
			mainFont = io.Fonts->AddFontFromMemoryTTF(
				const_cast<void*>(mappedFont.Data()),
				static_cast<int>(mappedFont.Size()), 20.0f, &config);
			if (mainFont != nullptr) {
				runtime.RetainFontSourceMapping(std::move(mappedFont));
				APP_PRINTF_INFO("font.source.mapped",
					"Mapped font source without a private heap copy: %zu bytes",
					runtime.MappedFontBytes());
			}
		}
		else if (!mapped) {
			APP_PRINTF_WARNING("font.source.mapping_failed",
				"Could not map the font source; using the file loader: %s",
				mappingError.message().c_str());
		}
		else {
			APP_PRINTF_WARNING("font.source.mapping_unsupported_size",
				"Font source size cannot be mapped into ImGui: %zu bytes",
				mappedFont.Size());
		}

		if (mainFont == nullptr) {
			config.FontDataOwnedByAtlas = true;
			mainFont = io.Fonts->AddFontFromFileTTF(
				wstring_to_utf8(Fontss).c_str(), 20.0f, &config);
		}
		if (mainFont == nullptr) io.Fonts->AddFontDefaultVector();
	}
	else {
		io.Fonts->AddFontDefaultVector();
	}

	if (!options.iconFontAvailable) return;
	ImFontConfig iconConfig;
	iconConfig.MergeMode = true;
	iconConfig.PixelSnapH = true;
	iconConfig.GlyphMinAdvanceX = 20.0f;
#ifdef _WIN32
	iconConfig.FontDataOwnedByAtlas = false;
	io.Fonts->AddFontFromMemoryTTF(
		const_cast<void*>(options.bundledIconFontData),
		static_cast<int>(options.bundledIconFontSize), 20.0f, &iconConfig);
#else
	io.Fonts->AddFontFromFileTTF(
		wstring_to_utf8(options.extractedIconFontPath.wstring()).c_str(),
		20.0f, &iconConfig);
#endif
}

} // namespace

DesktopUiSession::~DesktopUiSession() {
	Shutdown();
}

bool DesktopUiSession::Create(
	const DesktopUiSessionOptions& options,
	wstring& error) {
	if (active_ || options.paths == nullptr) {
		error = L"Invalid desktop UI session state.";
		return false;
	}
	const auto started = chrono::steady_clock::now();
	error.clear();

	const char* glslVersion = nullptr;
	GLFWwindow* window = CreateWindowWithFallbacks(
		options.width, options.height, options.initiallyVisible,
		options.firstRun, glslVersion);
	if (window == nullptr) {
		error = L"Unable to create an OpenGL window.";
		return false;
	}
	runtime_.AdoptWindow(window);
	// Appearance hooks on Windows resolve the native HWND through the process
	// main-window handle, so publish it as soon as this session owns the window.
	wc = window;
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);
#ifdef _WIN32
	ApplyWindowsWindowIcon(window, options.nativeInstance);
#endif

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	runtime_.MarkImGuiContextCreated();
	ImGuiIO& io = ImGui::GetIO();
	iniPath_ = wstring_to_utf8((options.paths->stateRoot / L"imgui.ini").wstring());
	logPath_ = wstring_to_utf8((options.paths->logsRoot / L"imgui_log.txt").wstring());
	io.IniFilename = iniPath_.c_str();
	io.LogFilename = logPath_.c_str();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	bool enableMultiViewport = true;
#ifdef __linux__
	const int selectedPlatform = glfwGetPlatform();
	enableMultiViewport = selectedPlatform != GLFW_PLATFORM_WAYLAND;
	const char* platformName = selectedPlatform == GLFW_PLATFORM_WAYLAND ? "Wayland"
		: selectedPlatform == GLFW_PLATFORM_X11 ? "X11" : "Other";
	fprintf(stderr, "[Desktop] GLFW selected platform: %s\n", platformName);
	APP_PRINTF_INFO("platform.glfw.selected", "GLFW selected platform: %s", platformName);
#endif
	if (enableMultiViewport) {
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		io.ConfigViewportsNoAutoMerge = false;
	}
	io.ConfigErrorRecoveryEnableAssert = true;
	io.ConfigErrorRecoveryEnableDebugLog = true;
	io.ConfigErrorRecoveryEnableTooltip = true;
	io.ConfigDpiScaleFonts = true;

	if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
		error = L"Unable to initialize the ImGui GLFW backend.";
		Shutdown();
		return false;
	}
	runtime_.MarkPlatformBackendInitialized();
#ifdef __EMSCRIPTEN__
	ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#endif
	if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
		error = L"Unable to initialize the ImGui OpenGL backend.";
		Shutdown();
		return false;
	}
	runtime_.MarkRendererBackendInitialized();
	runtime_.SetUiResourceReleaser(ReleaseMainUiGraphicsResources);

	ApplyTheme();
	if (options.firstRun) {
		GetUserDefaultUILanguageWin();
		Fontss = GetDefaultUIFontPath();
	}
	LoadFonts(options, runtime_);
	ResetSettingsWindowRuntimeState();
	ResetHistoryWindowRuntimeState();

#ifndef _WIN32
	if (options.firstRun) {
		glfwShowWindow(window);
		glfwFocusWindow(window);
	}
#endif
	active_ = true;
	lastCreateMilliseconds_ = chrono::duration<double, milli>(
		chrono::steady_clock::now() - started).count();
	APP_PRINTF_INFO("ui.session.created",
		"Desktop UI session created in %.2f ms; mapped_font_bytes=%zu",
		lastCreateMilliseconds_, runtime_.MappedFontBytes());
	return true;
}

void DesktopUiSession::DestroySecondaryPlatformWindows() noexcept {
	if (!active_ || ImGui::GetCurrentContext() == nullptr) return;
	glfwMakeContextCurrent(runtime_.Window());
	if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
		ImGui::DestroyPlatformWindows();
	}
}

void DesktopUiSession::SaveWindowState(int& width, int& height) noexcept {
	if (!active_ || runtime_.Window() == nullptr) return;
	glfwGetWindowSize(runtime_.Window(), &width, &height);
	if (ImGui::GetCurrentContext() != nullptr && !iniPath_.empty()) {
		ImGui::SaveIniSettingsToDisk(iniPath_.c_str());
	}
}

void DesktopUiSession::Shutdown() noexcept {
	GLFWwindow* window = runtime_.Window();
	if (window != nullptr) glfwMakeContextCurrent(window);
	DestroySecondaryPlatformWindows();
	runtime_.Shutdown();
	if (wc == window) wc = nullptr;
	active_ = false;
	iniPath_.clear();
	logPath_.clear();
}
