#define STB_IMAGE_IMPLEMENTATION
#include "Broadcast.h"
#include "Globals.h"
#include "UIHelpers.h"
#include "MainUI.h"
#include "imgui-all.h"
#include "imgui_style.h"
#include "i18n.h"
#include "AppState.h"
#include "TaskSystem.h"
#ifdef _WIN32
#include "Platform_win.h"
#elif defined(__APPLE__)
#include "Platform_macos.h"
#else
#include "Platform_linux.h"
#endif
#include "Console.h"
#include "ConfigManager.h"
#include "text_to_text.h"
#include "HistoryManager.h"
#include "BackupManager.h"

#ifdef _WIN32
#include <conio.h>
#else
#include <cstdio>
#include <unistd.h>
inline int _getch() { return std::getchar(); }
#endif
#include <fstream>
#include <system_error>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <limits.h>
#include <CoreText/CoreText.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

using namespace std;

GLFWwindow* wc = nullptr;
static map<wstring, GLuint> g_worldIconTextures;
static map<wstring, ImVec2> g_worldIconDimensions;
static vector<int> worldIconWidths, worldIconHeights;
string CURRENT_VERSION = "1.13.1";
atomic<bool> g_UpdateCheckDone(false);
atomic<bool> g_NewVersionAvailable(false);
atomic<bool> g_NoticeCheckDone(false);
atomic<bool> g_NewNoticeAvailable(false);
string g_LatestVersionStr;
string g_ReleaseNotes;
string g_NoticeContent;
string g_NoticeUpdatedAt;
string g_NoticeLastSeenVersion;
int g_windowWidth = 1280, g_windowHeight = 800;
float g_uiScale = 1.0f;

int last_interval = 15;

// 设置项变量（全局）
ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
wstring Fontss;
bool showSettings = false;
bool isSilence = false, isSafeDelete = true;
bool specialSetting = false;
bool g_CheckForUpdates = true, g_ReceiveNotices = true, g_StopAutoBackupOnExit = false, g_RunOnStartup = false, g_AutoScanForWorlds = false, g_autoLogEnabled = true;
bool showHistoryWindow = false;
bool g_enableKnotLink = true;
int g_hotKeyBackupId = 'S', g_hotKeyRestoreId = 'Z';

// 关闭行为设置
// 0 = 每次询问, 1 = 直接最小化到托盘, 2 = 直接退出
int g_closeAction = 0;
bool g_rememberCloseAction = false;
bool g_showCloseConfirmDialog = false;

atomic<bool> specialTasksRunning = false;
atomic<bool> specialTasksComplete = false;
thread g_exitWatcherThread;
atomic<bool> g_stopExitWatcher(false);
wstring g_worldToFocusInHistory = L"";
vector<wstring> restoreWhitelist;

wstring GetDefaultUIFontPath() {
#ifdef _WIN32
	// 动态获取 Windows 字体目录
	wstring fontsDir = L"C:\\Windows\\Fonts\\";
	{
		wchar_t winDir[MAX_PATH] = {};
		if (GetWindowsDirectoryW(winDir, MAX_PATH) > 0)
			fontsDir = wstring(winDir) + L"\\Fonts\\";
	}
	// 用户字体目录
	wstring userFontsDir;
	{
		wchar_t localAppData[MAX_PATH] = {};
		if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0)
			userFontsDir = wstring(localAppData) + L"\\Microsoft\\Windows\\Fonts\\";
	}

	if (g_CurrentLang == "zh_CN") {
		const wchar_t* cn_names[] = { L"msyh.ttc", L"msyh.ttf", L"msjh.ttc", L"msjh.ttf", L"SegoeUI.ttf", nullptr };
		for (const wchar_t** fn = cn_names; *fn; ++fn) {
			wstring p = fontsDir + *fn;
			if (filesystem::exists(p)) return p;
			if (!userFontsDir.empty()) {
				p = userFontsDir + *fn;
				if (filesystem::exists(p)) return p;
			}
		}
	}
	const wchar_t* en_names[] = { L"SegoeUI.ttf", L"segoeui.ttf", L"arial.ttf", nullptr };
	for (const wchar_t** fn = en_names; *fn; ++fn) {
		wstring p = fontsDir + *fn;
		if (filesystem::exists(p)) return p;
		if (!userFontsDir.empty()) {
			p = userFontsDir + *fn;
			if (filesystem::exists(p)) return p;
		}
	}
	return L"";

#elif defined(__APPLE__)
	// 使用 Core Text 按名称查找字体文件路径
	auto findFontPath = [](const char* fontName) -> wstring {
		CFStringRef cfName = CFStringCreateWithCString(kCFAllocatorDefault, fontName, kCFStringEncodingUTF8);
		if (!cfName) return L"";
		CTFontRef font = CTFontCreateWithName(cfName, 12.0, nullptr);
		CFRelease(cfName);
		if (!font) return L"";
		CFURLRef url = (CFURLRef)CTFontCopyAttribute(font, kCTFontURLAttribute);
		CFRelease(font);
		if (!url) return L"";
		char path[PATH_MAX];
		Boolean ok = CFURLGetFileSystemRepresentation(url, true, (UInt8*)path, sizeof(path));
		CFRelease(url);
		if (!ok) return L"";
		return utf8_to_wstring(string(path));
	};

	if (g_CurrentLang == "zh_CN") {
		// 中文字体：苹方 > 华文黑体 > Hiragino
		const char* cnFontNames[] = {
			"PingFangSC-Regular", "PingFang SC",
			"STHeitiSC-Light", "STHeiti",
			"Hiragino Sans GB", "Hiragino Sans",
			nullptr
		};
		for (const char** fn = cnFontNames; *fn; ++fn) {
			wstring p = findFontPath(*fn);
			if (!p.empty() && filesystem::exists(p)) return p;
		}
	}
	// 英文/默认字体
	const char* enFontNames[] = {
		"Helvetica Neue", "Helvetica",
		".AppleSystemUIFont",
		"Lucida Grande", "Arial",
		nullptr
	};
	for (const char** fn = enFontNames; *fn; ++fn) {
		wstring p = findFontPath(*fn);
		if (!p.empty() && filesystem::exists(p)) return p;
	}
	// 回退到硬编码路径
	const wstring fallbacks[] = {
		L"/System/Library/Fonts/PingFang.ttc",
		L"/System/Library/Fonts/STHeiti Light.ttc",
		L"/System/Library/Fonts/Helvetica.ttc",
		L"/System/Library/Fonts/Supplemental/Arial.ttf",
		L"/Library/Fonts/Arial.ttf"
	};
	for (const auto& cand : fallbacks) {
		if (filesystem::exists(cand)) return cand;
	}
	return L"";

#else
	// Linux: 使用 fontconfig (fc-match) 动态查找字体
	auto findFontByFc = [](const char* pattern) -> wstring {
		string cmd = "fc-match -f '%{file}' '";
		cmd += pattern;
		cmd += "' 2>/dev/null";
		FILE* pipe = popen(cmd.c_str(), "r");
		if (!pipe) return L"";
		char buf[4096] = {};
		string output;
		if (fgets(buf, sizeof(buf), pipe)) {
			output = buf;
		}
		pclose(pipe);
		if (!output.empty() && output.back() == '\n') output.pop_back();
		if (!output.empty() && filesystem::exists(output))
			return utf8_to_wstring(output);
		return L"";
	};

	if (g_CurrentLang == "zh_CN") {
		const char* cnPatterns[] = {
			"Noto Sans CJK SC:style=Regular",
			"Noto Sans CJK:style=Regular",
			"WenQuanYi Zen Hei",
			"WenQuanYi Micro Hei",
			"Droid Sans Fallback",
			nullptr
		};
		for (const char** p = cnPatterns; *p; ++p) {
			wstring path = findFontByFc(*p);
			if (!path.empty()) return path;
		}
	}
	// 默认字体
	const char* defaultPatterns[] = {
		"sans-serif",
		"DejaVu Sans",
		"Liberation Sans",
		"Noto Sans",
		nullptr
	};
	for (const char** p = defaultPatterns; *p; ++p) {
		wstring path = findFontByFc(*p);
		if (!path.empty()) return path;
	}
	// 回退到硬编码路径
	const wstring fallbacks[] = {
		L"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
		L"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
		L"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
		L"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
	};
	for (const auto& cand : fallbacks) {
		if (filesystem::exists(cand)) return cand;
	}
	return L"";
#endif
}

#ifdef __APPLE__
static void SetWorkingDirectoryToExecutable() {
	char path[PATH_MAX];
	uint32_t size = sizeof(path);
	if (_NSGetExecutablePath(path, &size) == 0) {
		std::error_code ec;
		filesystem::current_path(filesystem::path(path).parent_path(), ec);
	}
}
#endif

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

void CheckForConfigConflicts();
bool IsPureASCII(const wstring& s);
wstring SanitizeFileName(const wstring& input);
void CheckForNoticesThread();

void OpenLinkInBrowser(const wstring& url);
//bool LoadTextureFromFile(const char* filename, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height);
bool LoadTextureFromFileGL(const char* filename, GLuint* out_texture, int* out_width, int* out_height);
bool ExtractFontToTempFile(wstring& extractedPath);
bool Extract7zToTempFile(wstring& extractedPath);

void GameSessionWatcherThread();

void ShowSettingsWindowV2();
string ProcessCommand(const string& commandStr, Console* console);
void DoExportForSharing(Config tempConfig, wstring worldName, wstring worldPath, wstring outputPath, wstring description, Console& console);
void ConsoleLog(Console* console, const char* format, ...);

#ifdef _WIN32
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
	// 设置当前工作目录为可执行文件所在目录，避免开机自启寻找config错误
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(NULL, exePath, MAX_PATH);
	SetCurrentDirectoryW(filesystem::path(exePath).parent_path().c_str());

	LoadConfigs("config.ini");

	if (g_autoLogEnabled)
	{
		time_t now = time(0);
		char time_buf[100];
		ctime_s(time_buf, sizeof(time_buf), &now);
		ConsoleLog(&console, L("AUTO_LOG_START"), time_buf);
	}

	HWND hwnd_hidden = CreateHiddenWindow(hInstance);
	CreateTrayIcon(hwnd_hidden, hInstance);
	RegisterHotkeys(hwnd_hidden, MINEBACKUP_HOTKEY_ID, g_hotKeyBackupId);
	RegisterHotkeys(hwnd_hidden, MINERESTORE_HOTKEY_ID, g_hotKeyRestoreId);
#else
int main(int argc, char** argv)
{
	#ifdef __APPLE__
	SetWorkingDirectoryToExecutable();
	#endif
	LoadConfigs("config.ini");
#endif


	
	wstring g_7zTempPath, g_FontTempPath;
	bool sevenZipExtracted = Extract7zToTempFile(g_7zTempPath);
	bool fontExtracted = ExtractFontToTempFile(g_FontTempPath);

	if (!sevenZipExtracted || !fontExtracted) {
		MessageBoxWin("Error", L("LOG_ERROR_7Z_NOT_FOUND"), 2);
	}

	CheckForConfigConflicts();
	LoadHistory();
	if (g_CheckForUpdates) {
		thread update_thread(CheckForUpdatesThread);
		update_thread.detach();
	}
	if (g_ReceiveNotices) {
		g_NoticeCheckDone = false;
		g_NewNoticeAvailable = false;
		thread notice_thread(CheckForNoticesThread);
		notice_thread.detach();
	}
	g_stopExitWatcher = false;
	g_exitWatcherThread = thread(GameSessionWatcherThread);
	BroadcastEvent("event=app_startup;version=" + CURRENT_VERSION);

	if (g_enableKnotLink) {
		// 初始化 KnotLink （异步进行避免卡顿）
		thread linkLoaderThread([]() {
#ifndef _WIN32
			InitKnotLink();
#endif
			g_signalSender = new SignalSender("0x00000020", "0x00000020");
			// 初始化命令响应器，并将 ProcessCommand 设为回调
			try {
				g_commandResponser = new OpenSocketResponser("0x00000020", "0x00000010");
				g_commandResponser->setQuestionHandler(
					[](const string& q) {
						// 灏嗘敹鍒扮殑闂浜ょ粰鍛戒护澶勭悊鍣?
						console.AddLog("[KnotLink] Received: %s", q.c_str());
						string response = ProcessCommand(q, &console);
						console.AddLog("[KnotLink] Responded: %s", response.c_str());
						return response;
					}
				);
			}
			catch (const exception& e) {
				console.AddLog("[ERROR] Failed to start KnotLink Responser: %s", e.what());
			}
		});
		linkLoaderThread.detach();
	}


	if (g_appState.specialConfigMode)
	{
		bool hide = false;
		if (g_appState.specialConfigs.count(g_appState.currentConfigIndex)) {
			hide = g_appState.specialConfigs[g_appState.currentConfigIndex].hideWindow;
		}

		#ifdef _WIN32
		if (!hide) {
			AllocConsole(); // Create a console window
			// Redirect standard I/O to the new console
			FILE* pCout, * pCerr, * pCin;
			freopen_s(&pCout, "CONOUT$", "w", stdout);
			freopen_s(&pCerr, "CONOUT$", "w", stderr);
			freopen_s(&pCin, "CONIN$", "r", stdin);
			// 将 stdout 和 stderr 设置为 UTF-8 编码
			SetConsoleOutputCP(CP_UTF8);
		}
		#endif

		RunSpecialMode(g_appState.currentConfigIndex);

		// 将捕获到的所有日志写入文件
		ofstream log_file("special_mode_log.txt", ios::app | ios::binary);
		if (log_file.is_open()) {
			for (const char* item : console.Items) {
				log_file << (item) << endl;
			}
			log_file << L("SPECIAL_MODE_LOG_END") << endl << endl;
			log_file.close();
		}
		else {
			ConsoleLog(nullptr, L("SPECIAL_MODE_LOG_FILE_ERROR"));
		}

		#ifdef _WIN32
		if (!hide) {
			FreeConsole();
		}
		#endif
		Sleep(3000);
		return 0;
	}

	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit()) {
		MessageBoxWin("Fatal Error", "Failed to initialize GLFW. The graphics driver may not support the required features.", 2);
		return 1;
	}

	// Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
	// GL ES 2.0 + GLSL 100 (WebGL 1.0)
	const char* glsl_version = "#version 100";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(IMGUI_IMPL_OPENGL_ES3)
	// GL ES 3.0 + GLSL 300 es (WebGL 2.0)
	const char* glsl_version = "#version 300 es";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
	// GL 3.2 + GLSL 150
	const char* glsl_version = "#version 150";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
	// GL 3.0 + GLSL 130 (default; will try fallbacks below on failure)
	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif


	float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor()); // Valid on GLFW 3.3+ only
	bool errorShow = false;
	bool isFirstRun = !filesystem::exists("config.ini");
	static bool showConfigWizard = isFirstRun;
	g_appState.showMainApp = !isFirstRun;
	if (isFirstRun) {
		g_windowWidth *= main_scale, g_windowHeight *= main_scale;
		g_uiScale = main_scale;
	}

#ifndef _WIN32
	if (isFirstRun) {
#ifdef GLFW_FOCUS_ON_SHOW
		glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
#endif
	}
#endif

	wc = glfwCreateWindow(g_windowWidth, g_windowHeight, "MineBackup", nullptr, nullptr);

#if !defined(IMGUI_IMPL_OPENGL_ES2) && !defined(IMGUI_IMPL_OPENGL_ES3) && !defined(__APPLE__)
	if (wc == nullptr) {
		fprintf(stderr, "OpenGL 3.0 context creation failed, trying OpenGL 2.1 fallback...\n");
		glfwDefaultWindowHints();
		glfwSetErrorCallback(glfw_error_callback);
		glsl_version = "#version 120";
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
		wc = glfwCreateWindow(g_windowWidth, g_windowHeight, "MineBackup", nullptr, nullptr);
	}
	if (wc == nullptr) {
		fprintf(stderr, "OpenGL 2.1 context creation failed, trying OpenGL 2.0 fallback...\n");
		glfwDefaultWindowHints();
		glfwSetErrorCallback(glfw_error_callback);
		glsl_version = "#version 110";
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
		wc = glfwCreateWindow(g_windowWidth, g_windowHeight, "MineBackup", nullptr, nullptr);
	}
	if (wc == nullptr) {
		fprintf(stderr, "OpenGL 2.0 context creation failed, trying default version...\n");
		glfwDefaultWindowHints();
		glfwSetErrorCallback(glfw_error_callback);
		glsl_version = "#version 110";
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 1);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
		wc = glfwCreateWindow(g_windowWidth, g_windowHeight, "MineBackup", nullptr, nullptr);
	}
#endif

	if (wc == nullptr) {
		MessageBoxWin("Fatal Error",
			"Failed to create window. Your graphics driver may not support OpenGL.\n"
			"Please update your graphics drivers or install a remote desktop solution that supports OpenGL.",
			2);
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(wc);
	glfwSwapInterval(1); // Enable vsync

	#ifdef __APPLE__
	CreateTrayIcon();
	#endif
	#ifndef _WIN32
	RegisterHotkeys(MINEBACKUP_HOTKEY_ID, g_hotKeyBackupId);
	RegisterHotkeys(MINERESTORE_HOTKEY_ID, g_hotKeyRestoreId);
	#endif

	// 设置窗口关闭回调，用于拦截关闭按钮
	glfwSetWindowCloseCallback(wc, [](GLFWwindow* window) {
		if (g_closeAction == 1) {
			glfwSetWindowShouldClose(window, GLFW_FALSE);
#if defined(__APPLE__)
			CreateTrayIcon();
			g_appState.showMainApp = false;
			glfwHideWindow(window);
#elif defined(__linux__)
			// Linux仅最小化窗口，不使用系统托盘
			glfwIconifyWindow(window);
#else
			g_appState.showMainApp = false;
			glfwHideWindow(window);
#endif
		} else if (g_closeAction == 2) {
			SaveConfigs();
			g_appState.done = true;
		} else {
			glfwSetWindowShouldClose(window, GLFW_FALSE);
			g_showCloseConfirmDialog = true;
		}
	});

#ifndef _WIN32
	if (isFirstRun) {
		glfwShowWindow(wc);
		glfwFocusWindow(wc);
	}
#endif


#ifdef _WIN32
	int width, height, channels;
	// 为了跨平台，更好的方式是直接加载一个png文件 - 写cmake的时候再替换吧
	// unsigned char* pixels = stbi_load("icon.png", &width, &height, 0, 4); 
	HRSRC hRes = FindResourceW(hInstance, MAKEINTRESOURCEW(104), RT_GROUP_ICON);
	HGLOBAL hMem = LoadResource(hInstance, hRes);
	void* pMem = LockResource(hMem);
	int nId = LookupIconIdFromDirectoryEx((PBYTE)pMem, TRUE, 0, 0, LR_DEFAULTCOLOR);
	hRes = FindResourceW(hInstance, MAKEINTRESOURCEW(nId), RT_ICON);
	hMem = LoadResource(hInstance, hRes);;
	pMem = LockResource(hMem);

	// 从内存中的图标数据加载
	unsigned char* pixels = stbi_load_from_memory((const stbi_uc*)pMem, SizeofResource(hInstance, hRes), &width, &height, &channels, 4);

	if (pixels) {
		GLFWimage images[1];
		images[0].width = width;
		images[0].height = height;
		images[0].pixels = pixels;
		glfwSetWindowIcon(wc, 1, images);
		stbi_image_free(pixels);
	}
#endif


	// Setup Dear ImGui context (参照 ImGui Wiki: Getting Started - GLFW + OpenGL)
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewports
	io.ConfigViewportsNoAutoMerge = true;                     // 不自动合并视口

	// Error Recovery
	io.ConfigErrorRecoveryEnableAssert = true;
	io.ConfigErrorRecoveryEnableDebugLog = true;
	io.ConfigErrorRecoveryEnableTooltip = true;

	// DPI 缩放
	io.FontGlobalScale = g_uiScale;
	io.ConfigDpiScaleFonts = true;

	ImGuiStyle& style = ImGui::GetStyle();
	style.ScaleAllSizes(g_uiScale);

	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		style.WindowRounding = 0.0f;
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
	}

	// Setup Platform/Renderer backends
	ImGui_ImplGlfw_InitForOpenGL(wc, true);
#ifdef __EMSCRIPTEN__
	ImGui_ImplGlfw_InstallEmscriptenCallbacks(wc, "#canvas");
#endif
	ImGui_ImplOpenGL3_Init(glsl_version);

	// Load Fonts
	// - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
	// - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
	// - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
	// - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
	// - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
	// - Read 'docs/FONTS.md' for more instructions and details.
	// - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
	//io.Fonts->AddFontDefault();
	//io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
	//IM_ASSERT(font != nullptr);


	//float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);

	if (g_appState.configs.count(g_appState.currentConfigIndex))
		ApplyTheme(g_appState.configs[g_appState.currentConfigIndex].theme); // 把主题加载放在这里了
	else if (g_appState.specialConfigs.count(g_appState.currentConfigIndex))
		ApplyTheme(g_appState.specialConfigs[g_appState.currentConfigIndex].theme);

	if (isFirstRun) {
		GetUserDefaultUILanguageWin();
		Fontss = GetDefaultUIFontPath();
	}

	if (!Fontss.empty() && filesystem::exists(Fontss)) {
		ImFontConfig fontCfg;
		fontCfg.PixelSnapH = true;
		if (g_CurrentLang == "zh_CN") {
			fontCfg.OversampleH = 1;  // CJK 鍏ㄥ瓧绗﹂泦瀛楁暟澶氾紝闄嶄綆杩囬噰鏍疯妭鐪佺汗鐞嗗唴瀛?
			fontCfg.OversampleV = 1;
			io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f, &fontCfg, io.Fonts->GetGlyphRangesChineseFull());
		} else {
			io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f, &fontCfg, io.Fonts->GetGlyphRangesDefault());
		}
	} else {
		io.Fonts->AddFontDefaultVector();
	}

	// 准备合并图标字体
	ImFontConfig config2;
	config2.MergeMode = true;
	config2.PixelSnapH = true;
	config2.GlyphMinAdvanceX = 20.0f; // 图标的宽度
	// 定义要从图标字体中加载的图标范围
	static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };

	// 加载并合并
	io.Fonts->AddFontFromFileTTF(wstring_to_utf8(g_FontTempPath).c_str(), 20.0f, &config2, icon_ranges);

	// 构建字体图谱
	io.Fonts->Build();

	console.AddLog(L("CONSOLE_WELCOME"));

	// 记录注释
	static char backupComment[CONSTANT1] = "";


	// 如果开了自动扫描，那么就检查一下，然后添加
	if (g_AutoScanForWorlds) {
		for (auto& [idx, config] : g_appState.configs) {
			if (config.saveRoot.empty()) continue;
			filesystem::path parent = filesystem::path(config.saveRoot).lexically_normal().parent_path().parent_path();
			if (parent.empty()) continue;
			std::error_code parent_ec;
			if (!filesystem::exists(parent, parent_ec) || parent_ec) continue;

			std::error_code iter_ec;
			for (filesystem::directory_iterator it(parent, filesystem::directory_options::skip_permission_denied, iter_ec);
				!iter_ec && it != filesystem::directory_iterator(); ++it) {
				const auto& entry = *it;
				if (!entry.is_directory()) continue;
				std::error_code saves_ec;
				if (!filesystem::exists(entry.path() / "saves", saves_ec) || saves_ec)
					continue;
				// 检查是否已经在配置中了
				bool alreadyExists = false;
				for (auto& [i, c] : g_appState.configs) {
					if (c.saveRoot == (entry.path() / "saves").wstring()) {
						alreadyExists = true;
						break;
					}
				}
				if (alreadyExists)
					continue;

				// 没有的话添加为新的配置
				int index = CreateNewNormalConfig();
				g_appState.configs[index] = config;
				g_appState.configs[index].name = wstring_to_utf8(entry.path().filename().wstring());
				g_appState.configs[index].saveRoot = (entry.path() / "saves").wstring();
				g_appState.configs[index].worlds.clear();
			}
		}
	}

	if (isFirstRun)
	{
		ImGuiTheme::ApplyNord(false);
	}

	// 没有交互时降低帧率，减少 CPU/GPU 占用
	struct FpsIdling {
		float fpsIdle = 10.0f;        // 空闲时的 FPS
		float fpsActive = 60.0f;      // 活跃时的 FPS
		bool  enableIdling = true;     // 是否启用空闲节能
		bool  isIdling = false;        // 输出：当前是否处于空闲状态
		double lastActivityTime = 0.0; // 上次交互的时间
		double idleTimeout = 3.0;      // 多少秒无交互后进入空闲
	} fpsIdling;

	auto ClockSeconds = []() -> double {
		static const auto start = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		std::chrono::duration<double> elapsed = now - start;
		return elapsed.count();
	};

	fpsIdling.lastActivityTime = ClockSeconds();

	// Main loop
	while (!g_appState.done && !glfwWindowShouldClose(wc))
	{
		if (glfwGetWindowAttrib(wc, GLFW_ICONIFIED) != 0 || (!g_appState.showMainApp && !showConfigWizard)) {
			glfwWaitEventsTimeout(1.0);
			continue;
		}

		// Power Save：基于 ImGui 输入事件检测空闲状态
		double nowTime = ClockSeconds();
		{
			ImGuiContext& g = *GImGui;
			bool hasInputEvent = !g.InputEventsQueue.empty();
			if (hasInputEvent || g.ActiveId != 0 || g.MovingWindow != nullptr) {
				fpsIdling.lastActivityTime = nowTime;
			}
		}

		bool shouldIdle = fpsIdling.enableIdling && 
			((nowTime - fpsIdling.lastActivityTime) > fpsIdling.idleTimeout);
		fpsIdling.isIdling = shouldIdle;

		// 根据空闲状态选择不同的等待策略
		if (shouldIdle) {
			double waitTimeout = 1.0 / (double)fpsIdling.fpsIdle;
			glfwWaitEventsTimeout(waitTimeout);
		} else {
			double waitTimeout = 1.0 / (double)fpsIdling.fpsActive;
			glfwWaitEventsTimeout(waitTimeout);
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		if (showConfigWizard) {
			ShowConfigWizard(showConfigWizard, errorShow, sevenZipExtracted, g_7zTempPath);
		}
		else if (g_appState.showMainApp) {


			ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->WorkPos);
			ImGui::SetNextWindowSize(viewport->WorkSize);
			ImGui::SetNextWindowViewport(viewport->ID);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

			ImGuiWindowFlags host_window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
			host_window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
			host_window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

			ImGui::Begin("MainDockSpaceHost", nullptr, host_window_flags);
			ImGui::PopStyleVar(3);

			static bool showAboutWindow = false;
			static bool showImportConfigConfirm = false;
			static bool showImportHistoryConfirm = false;
			static wstring pendingImportPath;
			// --- 顶部菜单栏 ---
			if (ImGui::BeginMenuBar()) {

				if (ImGui::BeginMenu(L("MENU_FILE"))) {
					// 导出配置
					if (ImGui::MenuItem(L("MENU_EXPORT_CONFIG"))) {
						wstring exportPath = SelectSaveFileDialog(L"config_export.ini", L"INI Files (*.ini)\0*.ini\0All Files (*.*)\0*.*\0");
						if (!exportPath.empty()) {
							SaveConfigs(exportPath);
							console.AddLog(L("LOG_CONFIG_EXPORTED"), wstring_to_utf8(exportPath).c_str());
						}
					}
					// 导入配置
					if (ImGui::MenuItem(L("MENU_IMPORT_CONFIG"))) {
						wstring importPath = SelectFileDialog();
						if (!importPath.empty() && filesystem::exists(importPath)) {
							pendingImportPath = importPath;
							showImportConfigConfirm = true;
						}
					}
					ImGui::Separator();
					// 导出历史记录
					if (ImGui::MenuItem(L("MENU_EXPORT_HISTORY"))) {
						wstring exportPath = SelectSaveFileDialog(L"history_export.dat", L"DAT Files (*.dat)\0*.dat\0All Files (*.*)\0*.*\0");
						if (!exportPath.empty()) {
							try {
								filesystem::copy_file(L"history.dat", exportPath, filesystem::copy_options::overwrite_existing);
								console.AddLog(L("LOG_HISTORY_EXPORTED"), wstring_to_utf8(exportPath).c_str());
							} catch (const exception& e) {
								console.AddLog("[Error] Failed to export history: %s", e.what());
							}
						}
					}
					// 导入历史记录
					if (ImGui::MenuItem(L("MENU_IMPORT_HISTORY"))) {
						wstring importPath = SelectFileDialog();
						if (!importPath.empty() && filesystem::exists(importPath)) {
							pendingImportPath = importPath;
							showImportHistoryConfirm = true;
						}
					}
					ImGui::Separator();
					if (ImGui::MenuItem(L("EXIT"))) {
						g_appState.done = true;
						SaveConfigs();
					}
					ImGui::EndMenu();
				}

				// 导入配置确认对话框
				if (showImportConfigConfirm) {
					ImGui::OpenPopup(L("CONFIRM_IMPORT_CONFIG_TITLE"));
				}
				ImGui::SetNextWindowViewport(viewport->ID);
				if (ImGui::BeginPopupModal(L("CONFIRM_IMPORT_CONFIG_TITLE"), &showImportConfigConfirm, ImGuiWindowFlags_AlwaysAutoResize)) {
					ImGui::TextWrapped(L("CONFIRM_IMPORT_CONFIG_MSG"));
					ImGui::Separator();
					float importBtnW = CalcPairButtonWidth(L("BUTTON_CONFIRM"), L("BUTTON_CANCEL"));
					if (ImGui::Button(L("BUTTON_CONFIRM"), ImVec2(importBtnW, 0))) {
						LoadConfigs(wstring_to_utf8(pendingImportPath));
						SaveConfigs(); // 保存到默认位置
						console.AddLog(L("LOG_CONFIG_IMPORTED"), wstring_to_utf8(pendingImportPath).c_str());
						showImportConfigConfirm = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(importBtnW, 0))) {
						showImportConfigConfirm = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}

				// 导入历史记录确认对话框
				if (showImportHistoryConfirm) {
					ImGui::OpenPopup(L("CONFIRM_IMPORT_HISTORY_TITLE"));
				}
				ImGui::SetNextWindowViewport(viewport->ID);
				if (ImGui::BeginPopupModal(L("CONFIRM_IMPORT_HISTORY_TITLE"), &showImportHistoryConfirm, ImGuiWindowFlags_AlwaysAutoResize)) {
					ImGui::TextWrapped(L("CONFIRM_IMPORT_HISTORY_MSG"));
					ImGui::Separator();
					float histBtnW = CalcPairButtonWidth(L("BUTTON_CONFIRM"), L("BUTTON_CANCEL"));
					if (ImGui::Button(L("BUTTON_CONFIRM"), ImVec2(histBtnW, 0))) {
						try {
							filesystem::copy_file(pendingImportPath, L"history.dat", filesystem::copy_options::overwrite_existing);
							LoadHistory();
							console.AddLog(L("LOG_HISTORY_IMPORTED"), wstring_to_utf8(pendingImportPath).c_str());
						} catch (const exception& e) {
							console.AddLog("[Error] Failed to import history: %s", e.what());
						}
						showImportHistoryConfirm = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(histBtnW, 0))) {
						showImportHistoryConfirm = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}

				if (ImGui::BeginMenu(L("SETTINGS"))) {

					if (ImGui::Checkbox(L("RUN_ON_WINDOWS_STARTUP"), &g_RunOnStartup)) {
						wchar_t selfPath[MAX_PATH];
						GetModuleFileNameW(NULL, selfPath, MAX_PATH);
						SetAutoStart("MineBackup_AutoTask_" + to_string(g_appState.currentConfigIndex), selfPath, false, g_appState.currentConfigIndex, g_RunOnStartup);
					}
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", L("TIP_GLOBAL_STARTUP"));
					ImGui::Checkbox(L("BUTTON_AUTO_LOG"), &g_autoLogEnabled);
					ImGui::Checkbox(L("BUTTON_AUTO_SCAN_WORLDS"), &g_AutoScanForWorlds);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_BUTTON_AUTO_SCAN_WORLDS"));
					ImGui::Checkbox(L("RECEIVE_NOTICES"), &g_ReceiveNotices);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_RECEIVE_NOTICES"));
					ImGui::Checkbox(L("STOP_AUTOBACKUP_ON_EXIT"), &g_StopAutoBackupOnExit);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_STOP_AUTOBACKUP_ON_EXIT"));
					ImGui::Separator();
					// 热键设置右拉栏（鼠标放上去会向右展开两个）
					static bool waitingForHotkey = false;
					static int whichFunc = 0;
					if (ImGui::BeginMenu(L("HOTKEY_SETTINGS"))) {
						if (ImGui::MenuItem(L("BUTTON_BACKUP_SELECTED"))) {
							console.AddLog(L("HOTKEY_INSTRUCTION"));
							waitingForHotkey = true;
							whichFunc = 1;
						}
						if (ImGui::MenuItem(L("BUTTON_RESTORE_SELECTED"))) {
							console.AddLog(L("HOTKEY_INSTRUCTION"));
							waitingForHotkey = true;
							whichFunc = 2;
						}
						if (waitingForHotkey) {
							ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Waiting...");
							for (int key = ImGuiKey_0; key <= ImGuiKey_Z; ++key) {
								if (ImGui::IsKeyPressed((ImGuiKey)key)) {
									waitingForHotkey = false;
									if (whichFunc == 1) {
										g_hotKeyBackupId = ImGuiKeyToVK((ImGuiKey)key);
#ifdef _WIN32
										UnregisterHotkeys(hwnd_hidden, MINEBACKUP_HOTKEY_ID);
										RegisterHotkeys(hwnd_hidden, MINEBACKUP_HOTKEY_ID, g_hotKeyBackupId);
#else
										UnregisterHotkeys(MINEBACKUP_HOTKEY_ID);
										RegisterHotkeys(MINEBACKUP_HOTKEY_ID, g_hotKeyBackupId);
#endif
										console.AddLog(L("HOTKEY_SET_TO"), (char)g_hotKeyBackupId);
										break;
									}
									else if (whichFunc == 2) {
										g_hotKeyRestoreId = ImGuiKeyToVK((ImGuiKey)key);
#ifdef _WIN32
										UnregisterHotkeys(hwnd_hidden, MINERESTORE_HOTKEY_ID);
										RegisterHotkeys(hwnd_hidden, MINERESTORE_HOTKEY_ID, g_hotKeyRestoreId);
#else
										UnregisterHotkeys(MINERESTORE_HOTKEY_ID);
										RegisterHotkeys(MINERESTORE_HOTKEY_ID, g_hotKeyRestoreId);
#endif
										console.AddLog(L("HOTKEY_SET_TO"), (char)g_hotKeyRestoreId);
										break;
									}
									break;
								}
							}
						}
						ImGui::EndMenu();
					}
					ImGui::Separator();
					ImGui::Checkbox(L("ENABLE_KNOTLINK"), &g_enableKnotLink);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_ENABLE_KNOTLINK"));
					ImGui::Checkbox(L("CHECK_FOR_UPDATES_ON_STARTUP"), &g_CheckForUpdates);
					ImGui::Separator();
					if (ImGui::MenuItem(L("DETAILED_SETTINGS_BUTTON"))) {
						showSettings = true;
					}
					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu(L("MENU_TOOLS"))) {
					if (ImGui::MenuItem(L("HISTORY_BUTTON"))) { showHistoryWindow = true; }
					ImGui::Separator();
					if (ImGui::MenuItem(L("BUTTON_BACKUP_MODS"))) { ImGui::OpenPopup(L("CONFIRM_BACKUP_OTHERS_TITLE")); }
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu(L("MENU_HELP"))) {
					if (ImGui::MenuItem(L("MENU_GITHUB"))) {
						OpenLinkInBrowser(L"https://github.com/Leafuke/MineBackup");
					}
					if (ImGui::MenuItem(L("MENU_ISSUE"))) {
						OpenLinkInBrowser(L"https://github.com/Leafuke/MineBackup/issues");
					}
					if (ImGui::MenuItem(L("HELP_DOCUMENT"))) {
						OpenLinkInBrowser(L"https://docs.qq.com/doc/DUUp4UVZOYmZWcm5M");
					}
					if (ImGui::MenuItem(L("SPONSOR_ME"))) {
						OpenLinkInBrowser(L"https://afdian.com/a/MineBackup");
					}
					if (ImGui::MenuItem(L("MENU_ABOUT"))) {
						showAboutWindow = true;
						ImGui::OpenPopup(L("MENU_ABOUT"));
					}
					ImGui::EndMenu();
				}
			

				// 在菜单栏右侧显示更新按钮
				if (g_NewVersionAvailable) {
					ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(L("UPDATE_AVAILABLE_BUTTON")).x - ImGui::GetStyle().FramePadding.x * 2 - 100);
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.902f, 0.6f, 1.0f));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.9f, 0.5f, 1.0f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
					static bool open_update_popup = false;
					if (ImGui::Button(L("UPDATE_AVAILABLE_BUTTON"))) {
						ImGui::OpenPopup(L("UPDATE_POPUP_TITLE"));
						open_update_popup = true;
					}
					ImGui::PopStyleColor(3);
					ImGui::SetNextWindowViewport(viewport->ID);
					if (ImGui::BeginPopupModal(L("UPDATE_POPUP_TITLE"), &open_update_popup, ImGuiWindowFlags_AlwaysAutoResize)) {
						ImGui::Text(L("UPDATE_POPUP_HEADER"), g_LatestVersionStr.c_str());
						ImGui::Separator();
						ImGui::TextWrapped(L("UPDATE_POPUP_NOTES"));

						ImGui::BeginChild("ReleaseNotes", ImVec2(ImGui::GetContentRegionAvail().x, 450), true);
						ImGui::TextWrapped("%s", g_ReleaseNotes.c_str());
						ImGui::EndChild();
						ImGui::Separator();
						if (ImGui::Button(L("UPDATE_POPUP_DOWNLOAD_BUTTON"), ImVec2(180, 0))) {
#ifdef _WIN32
							OpenLinkInBrowser(L"https://github.com/Leafuke/MineBackup/releases/download/" + utf8_to_wstring(g_LatestVersionStr) + L"/MineBackup.exe");
#elif defined(__APPLE__)
							OpenLinkInBrowser(L"https://github.com/Leafuke/MineBackup/releases/download/" + utf8_to_wstring(g_LatestVersionStr) + L"/MineBackup-macos.zip");
#else
							OpenLinkInBrowser(L"https://github.com/Leafuke/MineBackup/releases/download/" + utf8_to_wstring(g_LatestVersionStr) + L"/MineBackup-linux.7z");
#endif
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						if (ImGui::Button(L("UPDATE_POPUP_DOWNLOAD_BUTTON_2"), ImVec2(180, 0))) {
#ifdef _WIN32
							OpenLinkInBrowser(L"https://gh-proxy.com/https://github.com/Leafuke/MineBackup/releases/download/" + utf8_to_wstring(g_LatestVersionStr) + L"/MineBackup.exe");
#elif defined(__APPLE__)
							OpenLinkInBrowser(L"https://gh-proxy.com/github.com/Leafuke/MineBackup/releases/download/" + utf8_to_wstring(g_LatestVersionStr) + L"/MineBackup-macos.zip");
#else
							OpenLinkInBrowser(L"https://gh-proxy.com/https://github.com/Leafuke/MineBackup/releases/download/" + utf8_to_wstring(g_LatestVersionStr) + L"/MineBackup-linux.7z");
#endif
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						if (ImGui::Button(L("UPDATE_POPUP_DOWNLOAD_BUTTON_3"), ImVec2(180, 0))) {
							OpenLinkInBrowser(L"https://www.123865.com/s/Zsyijv-UTuGd?pwd=mine#");
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 0))) {
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						if (ImGui::Button(L("CHECK_FOR_UPDATES"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
							OpenLinkInBrowser(L"https://github.com/Leafuke/MineBackup/releases");
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::EndPopup();
					}
				}

				static bool notice_popup_opened = false;
				static bool notice_snoozed_this_session = false;
				if (g_ReceiveNotices && g_NoticeCheckDone && g_NewNoticeAvailable && !notice_popup_opened && !notice_snoozed_this_session) {
					ImGui::OpenPopup(L("NOTICE_POPUP_TITLE"));
					notice_popup_opened = true;
				}

				ImGui::SetNextWindowViewport(viewport->ID);
				if (ImGui::BeginPopupModal(L("NOTICE_POPUP_TITLE"), &notice_popup_opened, ImGuiWindowFlags_AlwaysAutoResize)) {
					ImGui::TextWrapped(L("NOTICE_POPUP_DESC"));
					ImGui::Separator();
					ImGui::BeginChild("NoticeContent", ImVec2(ImGui::GetContentRegionAvail().x, 320), true);
					ImGui::TextWrapped("%s", g_NoticeContent.c_str());
					ImGui::EndChild();
					ImGui::Separator();
					float noticeBtnWidth = CalcPairButtonWidth(L("NOTICE_CONFIRM"), L("NOTICE_LATER"));
					if (noticeBtnWidth < 250) noticeBtnWidth = 250;
					if (ImGui::Button(L("NOTICE_CONFIRM"), ImVec2(noticeBtnWidth, 0))) {
						g_NoticeLastSeenVersion = g_NoticeUpdatedAt;
						g_NewNoticeAvailable = false;
						notice_snoozed_this_session = true;
						SaveConfigs();
						notice_popup_opened = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					if (ImGui::Button(L("NOTICE_LATER"), ImVec2(noticeBtnWidth, 0))) {
						notice_snoozed_this_session = true;
						notice_popup_opened = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
				

				//{ 已弃用
				//	float buttonSize = ImGui::GetFrameHeight();
				//	// 将按钮推到菜单栏的最右边
				//	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttonSize * 3);

				//	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
				//	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.4f));
				//	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.1f, 0.1f, 0.5f));

				//	if (ImGui::Button("-", ImVec2(buttonSize, buttonSize))) {
				//		g_appState.showMainApp = false;
				//		glfwHideWindow(wc);
				//	}
				//	if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("MINIMIZE_TO_TRAY_TIP"));

				//	ImGui::PopStyleColor(3);
				//}


				ImGui::EndMenuBar();
			}

			// 关闭确认对话框
			if (g_showCloseConfirmDialog) {
				ImGui::OpenPopup(L("CLOSE_CONFIRM_TITLE"));
				g_showCloseConfirmDialog = false;
			}
			
			ImGui::SetNextWindowViewport(viewport->ID);
			if (ImGui::BeginPopupModal(L("CLOSE_CONFIRM_TITLE"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::TextWrapped(L("CLOSE_CONFIRM_MSG"));
				ImGui::Separator();
				
				static bool tempRememberChoice = false;
				ImGui::Checkbox(L("CLOSE_REMEMBER_CHOICE"), &tempRememberChoice);
				
				ImGui::Dummy(ImVec2(0, 10));

#ifdef __linux__
				if (ImGui::Button(L("CLOSE_MINIMIZE_WINDOW"), ImVec2(200, 0))) {
					if (tempRememberChoice) {
						g_closeAction = 1;
						g_rememberCloseAction = true;
					}
					glfwIconifyWindow(wc);
					ImGui::CloseCurrentPopup();
				}
#else
				if (ImGui::Button(L("CLOSE_MINIMIZE_TO_TRAY"), ImVec2(200, 0))) {
					if (tempRememberChoice) {
						g_closeAction = 1;
						g_rememberCloseAction = true;
					}
#ifdef __APPLE__
					CreateTrayIcon();
#endif
					g_appState.showMainApp = false;
					glfwHideWindow(wc);
					ImGui::CloseCurrentPopup();
				}
#endif
				ImGui::SameLine();
				if (ImGui::Button(L("CLOSE_EXIT_APP"), ImVec2(200, 0))) {
					if (tempRememberChoice) {
						g_closeAction = 2;
						g_rememberCloseAction = true;
					}
					SaveConfigs();
					g_appState.done = true;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(100, 0))) {
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}

			if (showAboutWindow)
				ImGui::OpenPopup(L("MENU_ABOUT"));

			ImGui::SetNextWindowViewport(viewport->ID);
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			if (ImGui::BeginPopupModal(L("MENU_ABOUT"), &showAboutWindow, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::Text("MineBackup v%s", CURRENT_VERSION.c_str());
				ImGui::Separator();
				ImGui::TextWrapped("%s %c %c", L("ABOUT_DESCRIPTION"), (char)g_hotKeyBackupId, (char)g_hotKeyRestoreId);
				ImGui::Text("%s", L("ABOUT_AUTHOR"));

				ImGui::Dummy(ImVec2(0.0f, 10.0f));

				if (ImGui::Button(L("ABOUT_VISIT_GITHUB")))
				{
					OpenLinkInBrowser(L"https://github.com/Leafuke/MineBackup");
				}
				ImGui::SameLine();
				if (ImGui::Button(L("ABOUT_VISIT_BILIBILI")))
				{
					OpenLinkInBrowser(L"https://space.bilibili.com/545429962");
				}
				if (ImGui::Button(L("ABOUT_VISIT_KNOTLINK")))
				{
					OpenLinkInBrowser(L"https://github.com/hxh230802/KnotLink");
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("ABOUT_VISIT_KNOTLINK_TIP"));
				if (ImGui::Button(L("ABOUT_VISIT_FOLDERREWIND")))
				{
					OpenLinkInBrowser(L"https://github.com/Leafuke/FolderRewind");
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("ABOUT_VISIT_FOLDERREWIND_TIP"));
				ImGui::Dummy(ImVec2(0.0f, 10.0f));
				ImGui::Text(L("ABOUT_QQ_GROUP"));
				ImGui::Dummy(ImVec2(0.0f, 10.0f));
				ImGui::SeparatorText(L("ABOUT_LICENSE_HEADER"));
				ImGui::Text("%s", L("ABOUT_LICENSE_TYPE"));
				ImGui::Text("%s", L("ABOUT_LICENSE_COPYRIGHT"));
				ImGui::Text("%s", L("ABOUT_LICENSE_TEXT"));

				ImGui::Dummy(ImVec2(0.0f, 10.0f));
				if (ImGui::Button(L("BUTTON_OK"), ImVec2(250, 0)))
				{
					showAboutWindow = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}



			ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
			ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_None);

			static bool first_time_layout = true;
			if (first_time_layout) {
				first_time_layout = false;
				ImGui::DockBuilderRemoveNode(dockspace_id); // clear any previous layout
				ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
				ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

				ImGuiID dock_main_id = dockspace_id;
				ImGuiID dock_right_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.4f, nullptr, &dock_main_id);
				ImGuiID dock_middle_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.45f, nullptr, &dock_main_id);
				ImGuiID dock_left_id = dock_main_id;

				ImGui::DockBuilderDockWindow(L("WORLD_LIST"), dock_left_id);
				ImGui::DockBuilderDockWindow(L("WORLD_DETAILS_PANE_TITLE"), dock_middle_id);
				ImGui::DockBuilderDockWindow(L("CONSOLE_TITLE"), dock_right_id);
				ImGui::DockBuilderFinish(dockspace_id);
			}

			ImGui::End(); // End of MainDockSpaceHost

			static int selectedWorldIndex = -1;       // 跟踪用户在列表中选择的世界
			static char backupComment[CONSTANT1] = "";// 备份注释输入框的内容
			// 获取当前配置
			if (!g_appState.configs.count(g_appState.currentConfigIndex)) { // 找不到，说明应该对应的是特殊配置
				specialSetting = true;
			}

			float totalW = ImGui::GetContentRegionAvail().x;
			float leftW = totalW * 0.32f;
			float midW = totalW * 0.25f;
			float rightW = totalW * 0.42f;
			// 缓存 DisplayWorlds，避免每帧重建（深拷贝 Config + mutex lock）
			static vector<DisplayWorld> displayWorlds;
			static int cachedConfigIndex = -999;
			static bool cachedSpecialSetting = false;
			static size_t cachedWorldCount = 0;
			static chrono::steady_clock::time_point lastDisplayWorldsRefresh{};
			auto now_dw = chrono::steady_clock::now();
			bool needsRebuild = (cachedConfigIndex != g_appState.currentConfigIndex)
				|| (cachedSpecialSetting != specialSetting)
				|| (chrono::duration_cast<chrono::milliseconds>(now_dw - lastDisplayWorldsRefresh).count() > 2000);
			{
				// 配置变了或者两秒没更新了，并且当前配置是普通配置
				lock_guard<mutex> lock(g_appState.configsMutex);
				if (!specialSetting && g_appState.configs.count(g_appState.currentConfigIndex)) {
					if (g_appState.configs[g_appState.currentConfigIndex].worlds.size() != cachedWorldCount)
						needsRebuild = true;
				}
			}
			if (needsRebuild) {
				displayWorlds = BuildDisplayWorldsForSelection();
				cachedConfigIndex = g_appState.currentConfigIndex;
				cachedSpecialSetting = specialSetting;
				cachedWorldCount = displayWorlds.size();
				lastDisplayWorldsRefresh = now_dw;
			}
			int worldCount = (int)displayWorlds.size();

			// 缓存 GetLastOpenTime / GetLastBackupTime，每5秒刷新一次
			static map<wstring, wstring> cachedOpenTimes;
			static map<wstring, wstring> cachedBackupTimes;
			static map<wstring, bool> cachedNeedsBackup;
			static chrono::steady_clock::time_point lastTimeCacheRefresh{};
			auto now_tc = chrono::steady_clock::now();
			bool refreshTimeCache = chrono::duration_cast<chrono::seconds>(now_tc - lastTimeCacheRefresh).count() >= 5;
			if (refreshTimeCache || needsRebuild) {
				cachedOpenTimes.clear();
				cachedBackupTimes.clear();
				cachedNeedsBackup.clear();
				for (int i = 0; i < worldCount; ++i) {
					const auto& dw_t = displayWorlds[i];
					wstring wf = JoinPath(dw_t.effectiveConfig.saveRoot, dw_t.name).wstring();
					wstring bf = JoinPath(dw_t.effectiveConfig.backupPath, dw_t.name).wstring();
					wstring ot = GetLastOpenTime(wf);
					wstring bt = GetLastBackupTime(bf);
					cachedOpenTimes[wf] = ot;
					cachedBackupTimes[bf] = bt;
					cachedNeedsBackup[wf] = (ot > bt);
				}
				lastTimeCacheRefresh = now_tc;
			}

			// 一次性获取所有任务运行状态
			static map<pair<int,int>, bool> cachedTaskRunning;
			{
				lock_guard<mutex> taskLock(g_appState.task_mutex);
				cachedTaskRunning.clear();
				for (int i = 0; i < worldCount; ++i) {
					auto key = make_pair(displayWorlds[i].baseConfigIndex, i);
					cachedTaskRunning[key] = g_appState.g_active_auto_backups.count(key) > 0;
				}
			}


			if (ImGui::Begin(L("WORLD_LIST"))) {
				ImGui::SeparatorText(L("QUICK_CONFIG_SWITCHER"));
				ImGui::SetNextItemWidth(-1);
				string current_config_label = "None";
				if (specialSetting && g_appState.specialConfigs.count(g_appState.currentConfigIndex)) {
					current_config_label = "[Sp." + to_string(g_appState.currentConfigIndex) + "] " + g_appState.specialConfigs[g_appState.currentConfigIndex].name;
				}
				else if (!specialSetting && g_appState.configs.count(g_appState.currentConfigIndex)) {
					current_config_label = "[No." + to_string(g_appState.currentConfigIndex) + "] " + g_appState.configs[g_appState.currentConfigIndex].name;
				}
				//string(L("CONFIG_N")) + to_string(g_appState.currentConfigIndex)
				static bool showAddConfigPopup = false, showDeleteConfigPopup = false;

				if (ImGui::BeginCombo("##ConfigSwitcher", current_config_label.c_str())) {
					// 普通配置
					for (auto const& [idx, val] : g_appState.configs) {
						const bool is_selected = (g_appState.currentConfigIndex == idx);
						string label = "[No." + to_string(idx) + "] " + val.name;

						if (ImGui::Selectable(label.c_str(), is_selected)) {
							g_appState.currentConfigIndex = idx;
							specialSetting = false;
							ApplyTheme(val.theme);
						}
						if (is_selected) {
							ImGui::SetItemDefaultFocus();
						}
					}
					ImGui::Separator();
					// 特殊配置
					for (auto const& [idx, val] : g_appState.specialConfigs) {
						const bool is_selected = (g_appState.currentConfigIndex == (idx));
						string label = "[Sp." + to_string((idx)) + "] " + val.name;
						if (ImGui::Selectable(label.c_str(), is_selected)) {
							g_appState.currentConfigIndex = (idx);
							specialSetting = true;
							ApplyTheme(val.theme);
							//g_appState.specialConfigMode = true;
						}
						if (is_selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::Separator();
					if (ImGui::Selectable(L("BUTTON_ADD_CONFIG"))) {
						showAddConfigPopup = true;
					}

					if (ImGui::Selectable(L("BUTTON_DELETE_CONFIG"))) {
						if ((!specialSetting && g_appState.configs.size() > 1) || (specialSetting && !g_appState.specialConfigs.empty())) { // 至少保留一个
							showDeleteConfigPopup = true;
						}
					}


					ImGui::EndCombo();
				}

				// 删除配置弹窗
				if (showDeleteConfigPopup)
					ImGui::OpenPopup(L("CONFIRM_DELETE_TITLE"));
				ImGui::SetNextWindowViewport(viewport->ID);
				if (ImGui::BeginPopupModal(L("CONFIRM_DELETE_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
					showDeleteConfigPopup = false;
					if (specialSetting) {
						ImGui::Text("[Sp.]");
						ImGui::SameLine();
						ImGui::Text(L("CONFIRM_DELETE_MSG"), g_appState.currentConfigIndex, g_appState.specialConfigs[g_appState.currentConfigIndex].name.c_str());
					}
					else {
						ImGui::Text(L("CONFIRM_DELETE_MSG"), g_appState.currentConfigIndex, g_appState.configs[g_appState.currentConfigIndex].name.c_str());
					}
					ImGui::Separator();
					float delConfirmBtnWidth = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"));
					if (ImGui::Button(L("BUTTON_OK"), ImVec2(delConfirmBtnWidth, 0))) {
						if (specialSetting) {
							g_appState.specialConfigs.erase(g_appState.currentConfigIndex);
							g_appState.specialConfigMode = false;
							g_appState.currentConfigIndex = g_appState.configs.empty() ? 0 : g_appState.configs.begin()->first;
						}
						else {
							g_appState.configs.erase(g_appState.currentConfigIndex);
							g_appState.currentConfigIndex = g_appState.configs.begin()->first;
						}
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(delConfirmBtnWidth, 0))) {
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
				// 添加新配置弹窗
				if (showAddConfigPopup)
					ImGui::OpenPopup(L("ADD_NEW_CONFIG_POPUP_TITLE"));

				ImGui::SetNextWindowViewport(viewport->ID);
				if (ImGui::BeginPopupModal(L("ADD_NEW_CONFIG_POPUP_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					showAddConfigPopup = false;
					static int config_type = 0; // 0 for Normal, 1 for Special
					static char new_config_name[128] = "New Config";

					ImGui::Text(L("CONFIG_TYPE_LABEL"));
					ImGui::RadioButton(L("CONFIG_TYPE_NORMAL"), &config_type, 0); ImGui::SameLine();
					ImGui::RadioButton(L("CONFIG_TYPE_SPECIAL"), &config_type, 1);

					if (config_type == 0) {
						ImGui::TextWrapped(L("CONFIG_TYPE_NORMAL_DESC"));
					}
					else {
						ImGui::TextWrapped(L("CONFIG_TYPE_SPECIAL_DESC"));
					}

					ImGui::InputText(L("NEW_CONFIG_NAME_LABEL"), new_config_name, IM_ARRAYSIZE(new_config_name));
					ImGui::Separator();

					float createBtnWidth = CalcPairButtonWidth(L("CREATE_BUTTON"), L("BUTTON_CANCEL"));
					if (ImGui::Button(L("CREATE_BUTTON"), ImVec2(createBtnWidth, 0))) {
						if (strlen(new_config_name) > 0) {
							if (config_type == 0) {
								//int new_index = g_appState.configs.empty() ? 1 : g_appState.configs.rbegin()->first + 1;
								// 原本是 g_appState.configs.rbegin()->first + 1，这样不太好，现在统一成nextConfigId
								int new_index = CreateNewNormalConfig(new_config_name);
								// 继承当前配置（如果有），但保留路径为空
								if (g_appState.configs.count(g_appState.currentConfigIndex)) {
									g_appState.configs[new_index] = g_appState.configs[g_appState.currentConfigIndex];
									g_appState.configs[new_index].name = new_config_name;
									g_appState.configs[new_index].saveRoot.clear();
									g_appState.configs[new_index].backupPath.clear();
									g_appState.configs[new_index].worlds.clear();
								}
								g_appState.currentConfigIndex = new_index;
								specialSetting = false;
							}
							else { // Special
								int new_index = CreateNewSpecialConfig(new_config_name);
								g_appState.currentConfigIndex = new_index;
								specialSetting = true;
							}
							showSettings = true; // Open detailed settings for the new config
							ImGui::CloseCurrentPopup();
						}
					}
					ImGui::SameLine();
					if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(createBtnWidth, 0))) {
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}

				ImGui::SeparatorText(L("WORLD_LIST"));

				//ImGui::BeginChild("WorldListChild", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3), true); // 预留底部按钮空间
				ImGui::BeginChild("WorldListChild", ImVec2(0, 0), true);


				for (int i = 0; i < worldCount; ++i) {
					const auto& dw = displayWorlds[i];
					ImGui::PushID(i);
					bool is_selected = (selectedWorldIndex == i);

					// worldFolder / backupFolder 基于 effectiveConfig - 使用跨平台路径拼接
					wstring worldFolder = JoinPath(dw.effectiveConfig.saveRoot, dw.name).wstring();
					wstring backupFolder = JoinPath(dw.effectiveConfig.backupPath, dw.name).wstring();

					// --- 左侧图标区 ---
					ImDrawList* draw_list = ImGui::GetWindowDrawList();

					float iconSz = ImGui::GetTextLineHeightWithSpacing() * 2.5f;
					ImVec2 icon_pos = ImGui::GetCursorScreenPos();
					ImVec2 icon_end_pos = ImVec2(icon_pos.x + iconSz, icon_pos.y + iconSz);

					// 绘制占位符和边框
					draw_list->AddRectFilled(icon_pos, icon_end_pos, IM_COL32(50, 50, 50, 200), 4.0f);
					draw_list->AddRect(icon_pos, icon_end_pos, IM_COL32(200, 200, 200, 200), 4.0f);


					string iconKey_utf8 = wstring_to_utf8(worldFolder);
					wstring iconKey = worldFolder;

					// 迟加载逻辑
					if (g_worldIconTextures.find(iconKey) == g_worldIconTextures.end()) {
						// 标记为正在加载或失败，避免重复尝试
						g_worldIconTextures[iconKey] = 0; // 0 表示无效纹理

						filesystem::path iconPathFs = JoinPath(worldFolder, L"icon.png");
						filesystem::path bedrockIconPathFs = JoinPath(worldFolder, L"world_icon.jpeg");
#ifdef _WIN32
						string iconPath = utf8_to_gbk(wstring_to_utf8(iconPathFs.wstring()));
						string bedrockIconPath = utf8_to_gbk(wstring_to_utf8(bedrockIconPathFs.wstring()));
#else
						string iconPath = wstring_to_utf8(iconPathFs.wstring());
						string bedrockIconPath = wstring_to_utf8(bedrockIconPathFs.wstring());
#endif

						GLuint texture_id = 0;
						int tex_w = 0, tex_h = 0;

						if (filesystem::exists(iconPath)) {
							LoadTextureFromFileGL(iconPath.c_str(), &texture_id, &tex_w, &tex_h);
						}
						else if (filesystem::exists(bedrockIconPath)) {
							LoadTextureFromFileGL(bedrockIconPath.c_str(), &texture_id, &tex_w, &tex_h);
						}

						if (texture_id > 0) {
							g_worldIconTextures[iconKey] = texture_id;
							g_worldIconDimensions[iconKey] = ImVec2((float)tex_w, (float)tex_h);
						}
					}

					// 渲染逻辑
					GLuint current_texture = g_worldIconTextures[iconKey];
					if (current_texture > 0) {
						ImGui::Image((void*)(intptr_t)current_texture, ImVec2(iconSz, iconSz));
					}
					else {
						const char* placeholder_icon = ICON_FA_FOLDER;
						ImVec2 text_size = ImGui::CalcTextSize(placeholder_icon);
						ImVec2 text_pos = ImVec2(icon_pos.x + (iconSz - text_size.x) * 0.5f, icon_pos.y + (iconSz - text_size.y) * 0.5f);
						draw_list->AddText(text_pos, IM_COL32(200, 200, 200, 255), placeholder_icon);
					}


					// 将光标移过图标区域
					ImGui::Dummy(ImVec2(iconSz, iconSz));

					ImGui::SetCursorScreenPos(icon_pos);
					ImGui::InvisibleButton("##icon_button", ImVec2(iconSz, iconSz));
					// 点击更换图标
					if (ImGui::IsItemClicked()) {
						wstring sel = SelectFileDialog();
						if (!sel.empty()) {
							// 覆盖原 icon.png - 使用跨平台路径拼接
							wstring destPath = JoinPath(worldFolder, L"icon.png").wstring();
							CopyFileW(sel.c_str(), destPath.c_str(), FALSE);
							// 释放旧纹理并重新加载
							if (current_texture) {
								glDeleteTextures(1, &current_texture);
							}
							GLuint newTextureId = 0;
							int tex_w = 0, tex_h = 0;
#ifdef _WIN32
							LoadTextureFromFileGL(utf8_to_gbk(wstring_to_utf8(destPath)).c_str(), &newTextureId, &tex_w, &tex_h);
#else
							LoadTextureFromFileGL(wstring_to_utf8(destPath).c_str(), &newTextureId, &tex_w, &tex_h);
#endif
							g_worldIconTextures[iconKey] = newTextureId;
							g_worldIconDimensions[iconKey] = ImVec2((float)tex_w, (float)tex_h);
						}
					}

					ImGui::SameLine();
					// --- 状态逻辑 (使用预计算缓存，避免每帧每项加锁和文件IO)
					bool is_task_running = cachedTaskRunning[make_pair(displayWorlds[i].baseConfigIndex, i)];
					bool needs_backup = false;
					{
						auto it = cachedNeedsBackup.find(worldFolder);
						if (it != cachedNeedsBackup.end()) needs_backup = it->second;
					}

					// 整个区域作为一个可选项
					// ImGuiSelectableFlags_AllowItemOverlap 允许我们在可选项上面绘制其他控件
					if (ImGui::Selectable("##world_selectable", is_selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2.5f))) {
						selectedWorldIndex = i;
					}

					ImVec2 p_min = ImGui::GetItemRectMin();
					ImVec2 p_max = ImGui::GetItemRectMax();

					// --- 卡片背景和高亮 ---
					if (ImGui::IsItemHovered()) {
						draw_list->AddRectFilled(p_min, p_max, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
					}
					else if (is_selected) {
						draw_list->AddRectFilled(p_min, p_max, ImGui::GetColorU32(ImGuiCol_FrameBgActive, 0.5f), 4.0f);
					}

					if (is_selected) {
						draw_list->AddRect(p_min, p_max, ImGui::GetColorU32(ImGuiCol_ButtonActive), 4.0f, 0, 2.0f);
					}

					// 我们在可选项的相同位置开始绘制我们的自定义内容
					ImGui::SameLine();
					ImGui::BeginGroup(); // 将所有内容组合在一起

					// --- 第一行：世界名和描述 (自动换行) ---
					string name_utf8 = wstring_to_utf8(dw.name);
					string desc_utf8 = wstring_to_utf8(dw.desc);
					ImGui::TextWrapped("%s", name_utf8.c_str());

					//// --- 第二行：时间和状态 ---
					//wstring openTime = GetLastOpenTime(worldFolder);
					//wstring backupTime = GetLastBackupTime(backupFolder);

					//// 将次要信息颜色变灰，更具层次感
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
					if (desc_utf8.empty()) {
						ImGui::TextWrapped(L("CARD_WORLD_NO_DESC"));
					}
					else {
						ImGui::TextWrapped("%s", desc_utf8.c_str());
					}
					ImGui::PopStyleColor();

					ImGui::EndGroup();

					// --- 右侧的状态图标 ---
					float icon_pane_width = 40.0f;
					ImGui::SameLine(ImGui::GetContentRegionAvail().x - icon_pane_width);
					ImGui::BeginGroup();
					ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 0.25f)); // 垂直居中一点
					if (is_task_running) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 1.0f, 1.0f)); // 蓝色
						ImGui::Text(ICON_FA_ROTATE); // 旋转图标，表示正在运行
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TOOLTIP_AUTOBACKUP_RUNNING"));
						ImGui::PopStyleColor();
					}
					else if (needs_backup) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f)); // 黄色
						ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION); // 警告图标
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TOOLTIP_NEEDS_BACKUP"));
						ImGui::PopStyleColor();
					}
					else {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.6f, 1.0f)); // 绿色
						ImGui::Text(ICON_FA_CIRCLE_CHECK); // 对勾图标
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TOOLTIP_UP_TO_DATE"));
						ImGui::PopStyleColor();
					}
					ImGui::EndGroup();


					ImGui::PopID();
					ImGui::Separator();
				}

				ImGui::EndChild(); // 结束 WorldListChild

			}
			ImGui::End();			

			if (ImGui::Begin(L("WORLD_DETAILS_PANE_TITLE"))) {
				if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size()) {
					ImGui::SameLine();
					{
						ImGui::SeparatorText(L("CURRENT_CONFIG_INFO"));

						ImGui::Text("%s: %s", L("SAVES_PATH_LABEL"), wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot).c_str());
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot).c_str());
						ImGui::Text("%s: %s", L("BACKUP_PATH_LABEL"), wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath).c_str());
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath).c_str());

						ImGui::SeparatorText(L("WORLD_DETAILS_PANE_TITLE"));
						ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
						ImGui::Text("%s", wstring_to_utf8(displayWorlds[selectedWorldIndex].name).c_str());
						ImGui::PopTextWrapPos();
						ImGui::Separator();

						// -- 详细信息 --
						wstring worldFolder = JoinPath(displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot, displayWorlds[selectedWorldIndex].name).wstring();
						wstring backupFolder = JoinPath(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath, displayWorlds[selectedWorldIndex].name).wstring();
						{
							auto otIt = cachedOpenTimes.find(worldFolder);
							auto btIt = cachedBackupTimes.find(backupFolder);
							wstring openTimeStr = (otIt != cachedOpenTimes.end()) ? otIt->second : GetLastOpenTime(worldFolder);
							wstring backupTimeStr = (btIt != cachedBackupTimes.end()) ? btIt->second : GetLastBackupTime(backupFolder);
							ImGui::Text("%s: %s", L("TABLE_LAST_OPEN"), wstring_to_utf8(openTimeStr).c_str());
							ImGui::Text("%s: %s", L("TABLE_LAST_BACKUP"), wstring_to_utf8(backupTimeStr).c_str());
						}

						ImGui::Separator();

						// -- 注释输入框 --if (ImGui::InputText(L("WORLD_DESC"), desc, CONSTANT2))
						//cfg.worlds[i].second = utf8_to_wstring(desc);
						//ImGui::InputTextMultiline(L("COMMENT_HINT"), backupComment, IM_ARRAYSIZE(backupComment), ImVec2(-1, ImGui::GetTextLineHeight() * 3));
						char buffer[CONSTANT1] = "";
						// 增加检查，确保 selectedWorldIndex 仍然有效
						if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size()) {
							const auto& dw = displayWorlds[selectedWorldIndex];
							wstring desc = dw.desc;
							strncpy_s(buffer, wstring_to_utf8(desc).c_str(), sizeof(buffer));
							ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
							ImGui::InputTextWithHint("##backup_desc", L("HINT_BACKUP_DESC"), buffer, IM_ARRAYSIZE(buffer), ImGuiInputTextFlags_EnterReturnsTrue);

							// 在写入前，再次进行完整的检查
							if (g_appState.configs.count(dw.baseConfigIndex)) {
								Config& cfg = g_appState.configs.at(dw.baseConfigIndex);
								if (dw.baseWorldIndex >= 0 && dw.baseWorldIndex < cfg.worlds.size()) {
									if (desc.find(L"\"") != wstring::npos || desc.find(L":") != wstring::npos || desc.find(L"\\") != wstring::npos || desc.find(L"/") != wstring::npos || desc.find(L">") != wstring::npos || desc.find(L"<") != wstring::npos || desc.find(L"|") != wstring::npos || desc.find(L"?") != wstring::npos || desc.find(L"*") != wstring::npos) {
										memset(buffer, '\0', sizeof(buffer));
										cfg.worlds[dw.baseWorldIndex].second = L"";
									}
									else {
										cfg.worlds[dw.baseWorldIndex].second = utf8_to_wstring(buffer);
									}
								}
							}
						}
						else {
							// 如果索引无效，显示一个禁用的占位输入框
							strcpy_s(buffer, "N/A");
							ImGui::BeginDisabled();
							ImGui::InputTextWithHint("##backup_desc", L("HINT_BACKUP_DESC"), buffer, IM_ARRAYSIZE(buffer));
							ImGui::EndDisabled();
						}

						ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
						ImGui::InputTextWithHint("##backup_comment", L("HINT_BACKUP_COMMENT"), backupComment, IM_ARRAYSIZE(backupComment), ImGuiInputTextFlags_EnterReturnsTrue);

						// -- 主要操作按钮 --
						float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
						if (ImGui::Button(L("BUTTON_BACKUP_SELECTED"), ImVec2(button_width, 0))) {
							MyFolder world = { JoinPath(displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot, displayWorlds[selectedWorldIndex].name).wstring(), displayWorlds[selectedWorldIndex].name, displayWorlds[selectedWorldIndex].desc, displayWorlds[selectedWorldIndex].effectiveConfig, displayWorlds[selectedWorldIndex].baseConfigIndex, selectedWorldIndex };
							thread backup_thread(DoBackup, world, ref(console), utf8_to_wstring(backupComment));
							backup_thread.detach();
							strcpy_s(backupComment, "");
						}
						ImGui::SameLine();
						if (ImGui::Button(L("BUTTON_AUTO_BACKUP_SELECTED"), ImVec2(button_width, 0))) {
							//ImGui::SetNextWindowViewport(viewport->ID);
							ImGui::OpenPopup(L("AUTOBACKUP_SETTINGS"));
						}

						if (ImGui::Button(L("HISTORY_BUTTON"), ImVec2(-1, 0))) {
							g_worldToFocusInHistory = displayWorlds[selectedWorldIndex].name; // 设置要聚焦的世界
							showHistoryWindow = true; // 打开历史窗口
						}
						if (ImGui::Button(L("BUTTON_HIDE_WORLD"), ImVec2(-1, 0))) {
							// 先做最小范围的本地检查并拷贝要操作的 DisplayWorld（displayWorlds 是本地变量）
							if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size()) {
								DisplayWorld dw_copy = displayWorlds[selectedWorldIndex]; // 做一个值拷贝，之后在锁内用索引去改 g_appState.configs

								bool did_change = false;

								// 在修改全局 g_appState.configs 前加锁，防止其它线程并发读/写导致崩溃
								{
									lock_guard<mutex> cfg_lock(g_appState.configsMutex);

									auto it = g_appState.configs.find(dw_copy.baseConfigIndex);
									if (it != g_appState.configs.end()) {
										Config& cfg = it->second;
										if (dw_copy.baseWorldIndex >= 0 && dw_copy.baseWorldIndex < (int)cfg.worlds.size()) {
											cfg.worlds[dw_copy.baseWorldIndex].second = L"#";
											did_change = true;
										}
									}
								} // 解锁 g_appState.configsMutex 
							}
						}

						if (ImGui::Button(L("BUTTON_PIN_WORLD"), ImVec2(-1, 0))) {
							// 检查索引是否有效且不是第一个
							if (selectedWorldIndex > 0 && selectedWorldIndex < displayWorlds.size()) {
								DisplayWorld& dw = displayWorlds[selectedWorldIndex];
								int configIdx = dw.baseConfigIndex;
								int worldIdx = dw.baseWorldIndex;

								// 确保我们操作的是普通配置中的世界列表
								if (!specialSetting && g_appState.configs.count(configIdx)) {
									Config& cfg = g_appState.configs[configIdx];
									if (worldIdx < cfg.worlds.size()) {
										// 存储要移动的世界
										pair<wstring, wstring> worldToMove = cfg.worlds[worldIdx];

										// 从原位置删除
										cfg.worlds.erase(cfg.worlds.begin() + worldIdx);

										// 插入到列表顶部
										cfg.worlds.insert(cfg.worlds.begin(), worldToMove);

										// 更新选中项为新的顶部项
										selectedWorldIndex = 0;
									}
								}
							}
						}
						if (ImGui::Button(L("OPEN_BACKUP_FOLDER"), ImVec2(-1, 0))) {
							wstring path = JoinPath(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath, displayWorlds[selectedWorldIndex].name).wstring();
							if (filesystem::exists(path)) {
								OpenFolder(path);
							}
							else {
								OpenFolder(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath);
							}
						}
						if (ImGui::Button(L("OPEN_SAVEROOT_FOLDER"), ImVec2(-1, 0))) {
							wstring path = JoinPath(displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot, displayWorlds[selectedWorldIndex].name).wstring();
							OpenFolder(path);
						}

						// 模组备份
						if (ImGui::Button(L("BUTTON_BACKUP_MODS"), ImVec2(-1, 0))) {
							if (selectedWorldIndex != -1) {
								ImGui::OpenPopup(L("CONFIRM_BACKUP_OTHERS_TITLE"));
							}
						}

						ImGui::SetNextWindowViewport(viewport->ID);
						if (ImGui::BeginPopupModal(L("CONFIRM_BACKUP_OTHERS_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
							static char mods_comment[256] = "";
							ImGui::TextUnformatted(L("CONFIRM_BACKUP_OTHERS_MSG"));
							ImGui::InputText(L("HINT_BACKUP_COMMENT"), mods_comment, IM_ARRAYSIZE(mods_comment));
							ImGui::Separator();

							float modsConfirmBtnWidth = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"));
							if (ImGui::Button(L("BUTTON_OK"), ImVec2(modsConfirmBtnWidth, 0))) {
								if (g_appState.configs.count(g_appState.currentConfigIndex)) {
									filesystem::path tempPath = displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot;
									filesystem::path modsPath = tempPath.parent_path() / "mods";
									if (!filesystem::exists(modsPath) && filesystem::exists(tempPath / "mods")) { // 服务器的模组可能放在world同级文件夹下
										modsPath = tempPath / "mods";
									}
									thread backup_thread(DoOthersBackup, g_appState.configs[g_appState.currentConfigIndex], modsPath, utf8_to_wstring(mods_comment), ref(console));
									backup_thread.detach();
									strcpy_s(mods_comment, "");
								}
								ImGui::CloseCurrentPopup();
							}
							ImGui::SameLine();
							if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(modsConfirmBtnWidth, 0))) {
								strcpy_s(mods_comment, "");
								ImGui::CloseCurrentPopup();
							}
							ImGui::EndPopup();
						}

						// 其他备份
						float availWidth = ImGui::GetContentRegionAvail().x;
						float btnWidth = ImGui::CalcTextSize(L("BUTTON_BACKUP_OTHERS")).x + ImGui::GetStyle().FramePadding.x * 2;
						if (ImGui::Button(L("BUTTON_BACKUP_OTHERS"), ImVec2(btnWidth, 0))) {
							if (selectedWorldIndex != -1) {
								ImGui::OpenPopup("Others");
							}
						}
						ImGui::SameLine();
						ImGui::SetNextItemWidth((availWidth - btnWidth) * 0.97f);
						// 可以输入需要备份的其他内容的路径，比如 D:\Games\g_appState.configs
						static char buf[CONSTANT1] = "";
						strcpy_s(buf, wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.othersPath).c_str());
						if (ImGui::InputTextWithHint("##OTHERS", L("HINT_BACKUP_WHAT"), buf, IM_ARRAYSIZE(buf))) {
							displayWorlds[selectedWorldIndex].effectiveConfig.othersPath = utf8_to_wstring(buf);
							g_appState.configs[displayWorlds[selectedWorldIndex].baseConfigIndex].othersPath = displayWorlds[selectedWorldIndex].effectiveConfig.othersPath;
						}

						ImGui::SetNextWindowViewport(viewport->ID);
						if (ImGui::BeginPopupModal("Others", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
							static char others_comment[CONSTANT1] = "";
							ImGui::TextUnformatted(L("CONFIRM_BACKUP_OTHERS_MSG"));
							ImGui::InputText(L("HINT_BACKUP_COMMENT"), others_comment, IM_ARRAYSIZE(others_comment));
							ImGui::Separator();

							float othersConfirmBtnWidth = CalcPairButtonWidth(L("BUTTON_OK"), L("BUTTON_CANCEL"));
							if (ImGui::Button(L("BUTTON_OK"), ImVec2(othersConfirmBtnWidth, 0))) {
								thread backup_thread(DoOthersBackup, displayWorlds[selectedWorldIndex].effectiveConfig, utf8_to_wstring(buf), utf8_to_wstring(others_comment), ref(console));
								backup_thread.detach();
								strcpy_s(others_comment, "");
								SaveConfigs(); // 保存一下路径
								ImGui::CloseCurrentPopup();
							}
							ImGui::SameLine();
							if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(othersConfirmBtnWidth, 0))) {
								strcpy_s(others_comment, "");
								ImGui::CloseCurrentPopup();
							}
							ImGui::EndPopup();
						}


						if (ImGui::Button(L("CLOUD_SYNC_BUTTOM"), ImVec2(-1, 0))) {
							// 云同步逻辑
							const Config& config = g_appState.configs[displayWorlds[selectedWorldIndex].baseConfigIndex];
							if (!config.rclonePath.empty() && !config.rcloneRemotePath.empty() && filesystem::exists(config.rclonePath)) {
								console.AddLog(L("CLOUD_SYNC_START"));
								wstring localBackupPath = JoinPath(config.backupPath, displayWorlds[selectedWorldIndex].name).wstring();
								wstring rclone_command = L"\"" + config.rclonePath + L"\" copy \"" + localBackupPath + L"\" \"" + config.rcloneRemotePath + L"\" --progress";
								// 另起一个线程来执行云同步，避免阻塞后续操作
								thread([rclone_command, config]() {
									RunCommandInBackground(rclone_command, console, config.useLowPriority);
									console.AddLog(L("CLOUD_SYNC_FINISH"));
									}).detach();
							}
							else {
								console.AddLog(L("CLOUD_SYNC_INVALID"));
							}
						}

						// 导出分享
						if (ImGui::Button(L("BUTTON_EXPORT_FOR_SHARING"), ImVec2(-1, 0))) {
							if (selectedWorldIndex != -1) {
								ImGui::OpenPopup(L("EXPORT_WINDOW_TITLE"));
							}
						}
						ImGui::SetNextWindowViewport(viewport->ID);
						if (ImGui::BeginPopupModal(L("EXPORT_WINDOW_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
							// 使用 static 变量来持有一次性配置，它们只在弹窗首次打开时被初始化
							static Config tempExportConfig;
							static char outputPathBuf[MAX_PATH];
							static char descBuf[CONSTANT2];
							static char blacklistAddItemBuf[CONSTANT1];
							static int selectedBlacklistItem = -1;
							static int selectedFormat = 0;

							// 弹窗首次打开时，进行初始化
							if (ImGui::IsWindowAppearing()) {
								const auto& dw = displayWorlds[selectedWorldIndex];
								tempExportConfig = dw.effectiveConfig; // 复制当前配置作为基础

								// 智能设置默认输出路径为MineBackup当前位置
								wchar_t currentPath[MAX_PATH];
								GetCurrentDirectoryW(MAX_PATH, currentPath);
								wstring cleanWorldName = SanitizeFileName(dw.name);
								wstring finalPath = wstring(currentPath) + L"\\" + cleanWorldName + L"_shared." + tempExportConfig.zipFormat;
								strncpy_s(outputPathBuf, wstring_to_utf8(finalPath).c_str(), sizeof(outputPathBuf));

								// 预设默认黑名单
								tempExportConfig.blacklist.clear();
								tempExportConfig.blacklist.push_back(L"playerdata");
								tempExportConfig.blacklist.push_back(L"stats");
								tempExportConfig.blacklist.push_back(L"advancements");
								tempExportConfig.blacklist.push_back(L"session.lock");
								tempExportConfig.blacklist.push_back(L"level.dat_old");


								// 清空上次的输入
								memset(descBuf, 0, sizeof(descBuf));
								memset(blacklistAddItemBuf, 0, sizeof(blacklistAddItemBuf));
								selectedBlacklistItem = -1;
							}

							// 如果取消勾选 "包含数据包"，则动态添加/移除 datapacks
							/*bool datapacksInBlacklist = find(tempBlacklist.begin(), tempBlacklist.end(), L"datapacks") != tempBlacklist.end();
							if (includeDatapacks && datapacksInBlacklist) {
								tempBlacklist.erase(remove(tempBlacklist.begin(), tempBlacklist.end(), L"datapacks"), tempBlacklist.end());
							}
							else if (!includeDatapacks && !datapacksInBlacklist) {
								tempBlacklist.push_back(L"datapacks");
							}*/

							// --- UI 渲染 ---
							ImGui::SeparatorText(L("GROUP_EXPORT_OPTIONS"));
							ImGui::InputText(L("LABEL_EXPORT_PATH"), outputPathBuf, sizeof(outputPathBuf));
							ImGui::SameLine();
							if (ImGui::Button(L("BUTTON_BROWSE"))) {
								strcpy_s(outputPathBuf, MAX_PATH, wstring_to_utf8(SelectFolderDialog() + L"\\" + displayWorlds[selectedWorldIndex].name + L"_shared." + tempExportConfig.zipFormat).c_str());
							}

							if (ImGui::RadioButton("7z", &selectedFormat, 0)) { tempExportConfig.zipFormat = L"7z"; } ImGui::SameLine();
							if (ImGui::RadioButton("zip", &selectedFormat, 1)) { tempExportConfig.zipFormat = L"zip"; }

							ImGui::SeparatorText(L("GROUP_EXPORT_BLACKLIST"));
							ImGui::BeginChild("BlacklistChild", ImVec2(0, 150), true);
							for (int i = 0; i < tempExportConfig.blacklist.size(); ++i) {
								if (ImGui::Selectable(wstring_to_utf8(tempExportConfig.blacklist[i]).c_str(), selectedBlacklistItem == i)) {
									selectedBlacklistItem = i;
								}
							}
							ImGui::EndChild();

							if (ImGui::Button(L("BUTTON_REMOVE_SELECTED")) && selectedBlacklistItem != -1) {
								tempExportConfig.blacklist.erase(tempExportConfig.blacklist.begin() + selectedBlacklistItem);
								selectedBlacklistItem = -1;
							}
							ImGui::InputTextWithHint("##AddItem", L("HINT_ADD_BLACKLIST_ITEM"), blacklistAddItemBuf, sizeof(blacklistAddItemBuf));
							ImGui::SameLine();
							if (ImGui::Button(L("BUTTON_ADD")) && strlen(blacklistAddItemBuf) > 0) {
								tempExportConfig.blacklist.push_back(utf8_to_wstring(blacklistAddItemBuf));
								memset(blacklistAddItemBuf, 0, sizeof(blacklistAddItemBuf));
							}


							ImGui::SeparatorText(L("GROUP_EXPORT_DESCRIPTION"));
							ImGui::InputTextMultiline("##Desc", descBuf, sizeof(descBuf), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4), ImGuiInputTextFlags_AllowTabInput);

							ImGui::Separator();
							float exportBtnWidth = CalcPairButtonWidth(L("BUTTON_EXPORT"), L("BUTTON_CANCEL"));
							if (ImGui::Button(L("BUTTON_EXPORT"), ImVec2(exportBtnWidth, 0))) {
								const auto& dw = displayWorlds[selectedWorldIndex];

								wstring worldFullPath = JoinPath(dw.effectiveConfig.saveRoot, dw.name).wstring();

								thread export_thread(DoExportForSharing, tempExportConfig, dw.name, worldFullPath, utf8_to_wstring(outputPathBuf), utf8_to_wstring(descBuf), ref(console));
								export_thread.detach();

								ImGui::CloseCurrentPopup();
							}
							ImGui::SameLine();
							if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(exportBtnWidth, 0))) {
								ImGui::CloseCurrentPopup();
							}

							ImGui::EndPopup();
						}


					}

					// 自动备份弹窗
					ImGui::SetNextWindowViewport(viewport->ID);
					if (ImGui::BeginPopupModal(L("AUTOBACKUP_SETTINGS"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
						bool is_task_running = false;
						pair<int, int> taskKey = { -1,-1 };
						vector<DisplayWorld> localDisplayWorlds = displayWorlds; // 供显示使用，避免每帧重建
						{
							lock_guard<mutex> lock(g_appState.task_mutex);
							if (selectedWorldIndex >= 0) {
								if (selectedWorldIndex < (int)localDisplayWorlds.size()) {
									taskKey = { localDisplayWorlds[selectedWorldIndex].baseConfigIndex, localDisplayWorlds[selectedWorldIndex].baseWorldIndex };
									is_task_running = (g_appState.g_active_auto_backups.count(taskKey) > 0);
								}
							}
						}

						if (is_task_running) {
							ImGui::Text(L("AUTOBACKUP_RUNNING"), wstring_to_utf8(localDisplayWorlds[selectedWorldIndex].name).c_str());
							ImGui::Separator();
							if (ImGui::Button(L("BUTTON_STOP_AUTOBACKUP"), ImVec2(CalcButtonWidth(L("BUTTON_STOP_AUTOBACKUP")), 0))) {
								lock_guard<mutex> lock(g_appState.task_mutex);
								if (g_appState.g_active_auto_backups.count(taskKey)) {
									// 1. 设置停止标志
									g_appState.g_active_auto_backups.at(taskKey).stop_flag = true;
									// 2. 等待线程结束
									if (g_appState.g_active_auto_backups.at(taskKey).worker.joinable())
										g_appState.g_active_auto_backups.at(taskKey).worker.join();
									// 3. 从管理器中移除
									g_appState.g_active_auto_backups.erase(taskKey);
								}
								ImGui::CloseCurrentPopup();
							}
							ImGui::SameLine();
							if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(CalcButtonWidth(L("BUTTON_CANCEL")), 0))) {
								ImGui::CloseCurrentPopup();
							}
						}
						else {
							if (selectedWorldIndex < 0 || selectedWorldIndex >= (int)localDisplayWorlds.size()) {
								ImGui::TextDisabled("%s", L("PROMPT_SELECT_WORLD"));
								if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(CalcButtonWidth(L("BUTTON_CANCEL")), 0))) {
									ImGui::CloseCurrentPopup();
								}
							}
							else {
								ImGui::Text(L("AUTOBACKUP_SETUP_FOR"), wstring_to_utf8(localDisplayWorlds[selectedWorldIndex].name).c_str());
								ImGui::Separator();
								ImGui::InputInt(L("INTERVAL_MINUTES"), &last_interval);
								if (last_interval < 1) last_interval = 1;
								float autoBkpBtnWidth = CalcPairButtonWidth(L("BUTTON_START"), L("BUTTON_CANCEL"));
								if (ImGui::Button(L("BUTTON_START"), ImVec2(autoBkpBtnWidth, 0))) {
									// 注册并启动线程
									lock_guard<mutex> lock(g_appState.task_mutex);
									if (taskKey.first >= 0) {
										AutoBackupTask& task = g_appState.g_active_auto_backups[taskKey];
										task.stop_flag = false;

										task.worker = thread(AutoBackupThreadFunction, taskKey.first, taskKey.second, last_interval, &console, ref(task.stop_flag));

										ImGui::CloseCurrentPopup();
									}
								}
								ImGui::SameLine();
								if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(autoBkpBtnWidth, 0))) {
									ImGui::CloseCurrentPopup();
								}
							}
						}
						ImGui::EndPopup();
					}
				}
				else {
					ImGui::SameLine();
					ImGui::SeparatorText(L("WORLD_DETAILS_PANE_TITLE"));
					ImVec2 window_size = ImGui::GetWindowSize();
					ImVec2 text_size = ImGui::CalcTextSize(L("PROMPT_SELECT_WORLD"));
					ImGui::SetCursorPos(ImVec2((window_size.x - text_size.x) * 0.5f, (window_size.y - text_size.y) * 0.5f));
					ImGui::TextDisabled("%s", L("PROMPT_SELECT_WORLD"));
				}
			}
			ImGui::End();

			if (ImGui::Begin(L("CONSOLE_TITLE"))) {
				console.DrawEmbedded();
			}
			ImGui::End();


			if (showSettings) {
				//ImGui::SetNextWindowDockID(0, ImGuiCond_None); // 强制窗口不参与停靠
				ImGui::SetNextWindowViewport(viewport->ID);
				ShowSettingsWindowV2();  // 使用新版横向标签页设置窗口
			}
			if (showHistoryWindow) {
				if (specialSetting) {
					if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size())
						ShowHistoryWindow(displayWorlds[selectedWorldIndex].baseConfigIndex);
					else {
						auto spIt = g_appState.specialConfigs.find(g_appState.currentConfigIndex);
						if (spIt != g_appState.specialConfigs.end() && !spIt->second.tasks.empty())
							ShowHistoryWindow(spIt->second.tasks[0].configIndex);
					}
				}
				else {
					ShowHistoryWindow(g_appState.currentConfigIndex);
				}
			}
			//console.Draw(L("CONSOLE_TITLE"), &g_appState.showMainApp);
		}

		// Rendering
		ImGui::Render();
		int display_w, display_h;
		glfwGetFramebufferSize(wc, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);
		glClearColor(clear_color.x* clear_color.w, clear_color.y* clear_color.w, clear_color.z* clear_color.w, clear_color.w);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			GLFWwindow* backup_current_context = glfwGetCurrentContext();
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
			glfwMakeContextCurrent(backup_current_context);
		}

		glfwSwapBuffers(wc);
	}

	// 清理
	BroadcastEvent("event=app_shutdown");
	lock_guard<mutex> lock(g_appState.task_mutex);
	for (auto& pair : g_appState.g_active_auto_backups) {
		pair.second.stop_flag = true; // 通知线程停止
		if (pair.second.worker.joinable()) {
			pair.second.worker.join(); // 等待线程执行完毕
		}
	}
	for (auto const& [key, val] : g_worldIconTextures) {
		if (val > 0) {
			glDeleteTextures(1, &val);
		}
	}

	glfwGetWindowSize(wc, &g_windowWidth, &g_windowHeight);

	if (filesystem::exists("config.ini"))
		SaveConfigs();

	// 将捕获到的所有日志写入文件
	ofstream log_file("auto_log.txt", ios::app | ios::binary);
	if (log_file.is_open()) {
		for (const char* item : console.Items) {
			log_file << (item) << endl;
		}
		log_file << "=== End ===" << endl << endl;
		log_file.close();
	}

#ifdef _WIN32
	RemoveTrayIcon();
	UnregisterHotkeys(hwnd_hidden, MINEBACKUP_HOTKEY_ID);
	UnregisterHotkeys(hwnd_hidden, MINERESTORE_HOTKEY_ID);
	DestroyWindow(hwnd_hidden);
#else
	RemoveTrayIcon();
	UnregisterHotkeys(MINEBACKUP_HOTKEY_ID);
	UnregisterHotkeys(MINERESTORE_HOTKEY_ID);
#endif
	g_worldIconTextures.clear();
	worldIconWidths.clear();
	worldIconHeights.clear();
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(wc);
	glfwTerminate();

	g_stopExitWatcher = true;
	if (g_exitWatcherThread.joinable()) {
		g_exitWatcherThread.join();
	}

	// 清理 KnotLink
	CleanupKnotLink();

	return 0;
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

void ApplyTheme(const int& theme)
{
	switch (theme) {
	case 0: ImGuiTheme::ApplyImGuiDark(); break;
	case 1: ImGuiTheme::ApplyImGuiLight(); break;
	case 2: ImGuiTheme::ApplyImGuiClassic(); break;
	case 3: ImGuiTheme::ApplyWindows11(false); break;
	case 4: ImGuiTheme::ApplyWindows11(true); break;
	case 5: ImGuiTheme::ApplyNord(false); break;
	case 6: ImGuiTheme::ApplyNord(true); break;
	case 7: ImGuiTheme::ApplyCustom(); break;
	}
}
