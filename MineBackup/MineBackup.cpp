#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_IMPLEMENTATION
#include "Broadcast.h"
#include "imgui-all.h"
#include "i18n.h"
#include "AppState.h"
#ifdef _WIN32
#include "Platform_win.h"
#else
#include "Platform_linux.h"
#endif
#include "Console.h"
#include "ConfigManager.h"
#include "text_to_text.h"
#include "HistoryManager.h"
#include "BackupManager.h"
#include <conio.h>

using namespace std;

GLFWwindow* wc = nullptr;
static map<wstring, GLuint> g_worldIconTextures;
static map<wstring, ImVec2> g_worldIconDimensions;
static vector<int> worldIconWidths, worldIconHeights;
string CURRENT_VERSION = "1.9.9";
atomic<bool> g_UpdateCheckDone(false);
atomic<bool> g_NewVersionAvailable(false);
string g_LatestVersionStr;
string g_ReleaseNotes;
int g_windowWidth = 1280, g_windowHeight = 800;
float g_uiScale = 1.0f;

int last_interval = 15;


// 设置项变量（全局）
ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
wstring Fontss;
bool showSettings = false;
bool isSilence = false, isSafeDelete = false;
bool specialSetting = false;
bool g_CheckForUpdates = true, g_RunOnStartup = false;
bool showHistoryWindow = false;
bool g_enableKnotLink = true;

atomic<bool> specialTasksRunning = false;
atomic<bool> specialTasksComplete = false;
thread g_exitWatcherThread;
atomic<bool> g_stopExitWatcher(false);
map<pair<int, int>, wstring> g_activeWorlds; // Key: {configIdx, worldIdx}, Value: worldName
wstring g_worldToFocusInHistory = L"";
vector<wstring> restoreWhitelist;
extern enum class BackupCheckResult {
	NO_CHANGE,
	CHANGES_DETECTED,
	FORCE_FULL_BACKUP_METADATA_INVALID,
	FORCE_FULL_BACKUP_BASE_MISSING
};

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

bool IsPureASCII(const wstring& s);
void OpenLinkInBrowser(const wstring& url);
inline void ApplyTheme(int& theme);
wstring SanitizeFileName(const wstring& input);
//bool LoadTextureFromFile(const char* filename, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height);
bool LoadTextureFromFileGL(const char* filename, GLuint* out_texture, int* out_width, int* out_height);
bool checkWorldName(const wstring& world, const vector<pair<wstring, wstring>>& worldList);
bool ExtractFontToTempFile(wstring& extractedPath);
bool Extract7zToTempFile(wstring& extractedPath);
void GameSessionWatcherThread();
bool IsFileLocked(const wstring& path);
bool is_blacklisted(const filesystem::path& file_to_check, const filesystem::path& backup_source_root, const filesystem::path& original_world_root, const vector<wstring>& blacklist);
size_t CalculateFileHash(const filesystem::path& filepath);
string GetRegistryValue(const string& keyPath, const string& valueName);
void UpdateMetadataFile(const filesystem::path& metadataPath, const wstring& newBackupFile, const wstring& basedOnBackupFile, const map<wstring, size_t>& currentState);


void ShowSettingsWindow();
void ShowHistoryWindow(int& tempCurrentConfigIndex);
vector<DisplayWorld> BuildDisplayWorldsForSelection();


bool RunCommandInBackground(wstring command, Console& console, bool useLowPriority, const wstring& workingDirectory = L"");
string ProcessCommand(const string& commandStr, Console* console);
void DoExportForSharing(Config tempConfig, wstring worldName, wstring worldPath, wstring outputPath, wstring description, Console& console);
void RunSpecialMode(int configId);
void CheckForConfigConflicts();
void ConsoleLog(Console* console, const char* format, ...);



#ifdef _WIN32
// Main code
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
	// 设置当前工作目录为可执行文件所在目录，避免开机自启寻找config错误
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(NULL, exePath, MAX_PATH);
	SetCurrentDirectoryW(filesystem::path(exePath).parent_path().c_str());

	//_setmode(_fileno(stdout), _O_U8TEXT);
	//_setmode(_fileno(stdin), _O_U8TEXT);
	//CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	HWND hwnd_hidden = CreateHiddenWindow(hInstance);
	CreateTrayIcon(hwnd_hidden, hInstance);
	RegisterHotkeys(hwnd_hidden);
#else
int main(int argc, char** argv)
{
#endif

	wstring g_7zTempPath, g_FontTempPath;
	bool sevenZipExtracted = Extract7zToTempFile(g_7zTempPath);
	bool fontExtracted = ExtractFontToTempFile(g_FontTempPath);
	if (!sevenZipExtracted || !fontExtracted) {
		MessageBoxWin("Error", L("LOG_ERROR_7Z_NOT_FOUND"));
		return 0;
	}

	LoadConfigs("config.ini");
	CheckForConfigConflicts();
	LoadHistory();
	if (g_CheckForUpdates) {
		thread update_thread(CheckForUpdatesThread);
		update_thread.detach();
	}
	g_stopExitWatcher = false;
	g_exitWatcherThread = thread(GameSessionWatcherThread);
	BroadcastEvent("event=app_startup;version=" + CURRENT_VERSION);

	if (g_enableKnotLink) {
		// 初始化信号发送器 （异步进行避免卡顿）
		thread linkLoaderThread([]() {
			g_signalSender = new SignalSender("0x00000020", "0x00000020");
			// 初始化命令响应器，并将 ProcessCommand 设为回调
			try {
				g_commandResponser = new OpenSocketResponser("0x00000020", "0x00000010");
				g_commandResponser->setQuestionHandler(
					[](const string& q) {
						// 将收到的问题交给命令处理器
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

		if (!hide) {
			AllocConsole(); // Create a console window
			// Redirect standard I/O to the new console
			FILE* pCout, * pCerr, * pCin;
			freopen_s(&pCout, "CONOUT$", "w", stdout);
			freopen_s(&pCerr, "CONOUT$", "w", stderr);
			freopen_s(&pCin, "CONIN$", "r", stdin);
		}

		RunSpecialMode(g_appState.currentConfigIndex);

		if (!hide) {
			FreeConsole();
		}
		Sleep(3000);
		return 0;
	}

	
	{ // 检查 VC++2015-2022 运行时
		HKEY hKey;
		const wchar_t* registryPath = L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x86";
		long result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, registryPath, 0, KEY_READ, &hKey);
		if (result == ERROR_SUCCESS) {
			MessageBox(NULL, utf8_to_wstring(gbk_to_utf8(L("NO_RUNNINGTIME"))).c_str(), L"ERROR", MB_ICONERROR);
			return -1;
		}
		else {
			RegCloseKey(hKey);
		}
	}

	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
		return 1;

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
	// GL 3.0 + GLSL 130
	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif


	float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor()); // Valid on GLFW 3.3+ only
	wc = glfwCreateWindow(g_windowWidth, g_windowHeight, "MineBackup", nullptr, nullptr);
	if (wc == nullptr)
		return 1;
	glfwMakeContextCurrent(wc);
	glfwSwapInterval(1); // Enable vsync

	
#ifdef _WIN32
	int width, height, channels;
	// 为了跨平台，更好的方式是直接加载一个png文件 - 写cmake的时候再替换吧
	// unsigned char* pixels = stbi_load("icon.png", &width, &height, 0, 4); 
	HRSRC hRes = FindResource(hInstance, MAKEINTRESOURCE(IDI_ICON3), RT_GROUP_ICON);
	HGLOBAL hMem = LoadResource(hInstance, hRes);
	void* pMem = LockResource(hMem);
	int nId = LookupIconIdFromDirectoryEx((PBYTE)pMem, TRUE, 0, 0, LR_DEFAULTCOLOR);
	hRes = FindResource(hInstance, MAKEINTRESOURCE(nId), RT_ICON);
	hMem = LoadResource(hInstance, hRes);
	pMem = LockResource(hMem);
#endif

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


	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;

	io.FontGlobalScale = g_uiScale;

	// 启用Docking
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // 加上就失去圆角了，不知道怎么解决
	io.ConfigViewportsNoAutoMerge = true; // 不自动合并视口

	// 圆润风格
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 8.0f;
	style.FrameRounding = 5.0f;
	style.GrabRounding = 5.0f;
	style.PopupRounding = 5.0f;
	style.ScrollbarRounding = 5.0f;
	style.ChildRounding = 8.0f;

	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	// 设置字体和全局缩放
	style.ScaleAllSizes(main_scale);        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
	io.ConfigDpiScaleFonts = true;
	// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
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

	bool errorShow = false;
	bool isFirstRun = !filesystem::exists("config.ini");
	static bool showConfigWizard = isFirstRun;
	g_appState.showMainApp = !isFirstRun;
	if (isFirstRun)
		ImGui::StyleColorsLight();//默认亮色

	if (g_appState.configs.count(g_appState.currentConfigIndex))
		ApplyTheme(g_appState.configs[g_appState.currentConfigIndex].theme); // 把主题加载放在这里了
	else
		ApplyTheme(g_appState.specialConfigs[g_appState.currentConfigIndex].theme);

	if (isFirstRun) {
#ifdef _WIN32
		GetUserDefaultUILanguageWin();
		if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttc"))
			Fontss = L"C:\\Windows\\Fonts\\msyh.ttc";
		else if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttf"))
			Fontss = L"C:\\Windows\\Fonts\\msyh.ttf";
		else
			Fontss = L"C:\\Windows\\Fonts\\SegoeUI.ttf";
#else
		if (filesystem::exists("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"))
			Fontss = L"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
		else if (filesystem::exists("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"))
			Fontss = L"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc";
		else
			Fontss = L"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"; // 英文/通用
#endif
	}
	if (g_CurrentLang == "zh_CN")
		ImFont* font = io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f * main_scale, nullptr, io.Fonts->GetGlyphRangesChineseFull());
	else
		ImFont* font = io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f * main_scale, nullptr, io.Fonts->GetGlyphRangesDefault());

	// 准备合并图标字体
	ImFontConfig config2;
	config2.MergeMode = true;
	config2.PixelSnapH = true;
	config2.GlyphMinAdvanceX = 20.0f * main_scale; // 图标的宽度
	// 定义要从图标字体中加载的图标范围
	static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };

	// 加载并合并
	io.Fonts->AddFontFromFileTTF(wstring_to_utf8(g_FontTempPath).c_str(), 20.0f * main_scale, &config2, icon_ranges);

	// 构建字体图谱
	io.Fonts->Build();

	console.AddLog(L("CONSOLE_WELCOME"));

	if (sevenZipExtracted) {
		console.AddLog(L("LOG_7Z_EXTRACT_SUCCESS"));
	}
	else {
		console.AddLog(L("LOG_7Z_EXTRACT_FAIL"));
	}

	// 记录注释
	static char backupComment[CONSTANT1] = "";


	// Main loop
	while (!g_appState.done && !glfwWindowShouldClose(wc))
	{

		// 如果窗口最小化或不显示，可以等待更长时间
		if (glfwGetWindowAttrib(wc, GLFW_ICONIFIED) || !g_appState.showMainApp) {
			// 使用带超时的等待，这样我们仍然可以周期性地处理Win32消息
			glfwWaitEventsTimeout(0.2); // 等待200ms
		}
		else {
			// 正常轮询，以保持流畅的UI动画
			glfwPollEvents();
		}

		// Poll and handle messages (inputs, window resize, etc.)
		// See the WndProc() function below for our to dispatch events to the Win32 backend.
		MSG msg;
		while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				g_appState.done = true;
		}

		glfwPollEvents();
		if (glfwGetWindowAttrib(wc, GLFW_ICONIFIED) != 0)
		{
			ImGui_ImplGlfw_Sleep(10);
			continue;
		}

		// Start the Dear ImGui frame
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		if (showConfigWizard) {
			// 首次启动向导使用的静态变量
			static int page = 0;
			static bool isWizardOpen = true;
			static char saveRootPath[CONSTANT1] = "";
			static char backupPath[CONSTANT1] = "";
			static char zipPath[CONSTANT1] = "";

			if (!isWizardOpen)
				g_appState.done = true;

			ImGui::Begin(L("WIZARD_TITLE"), &isWizardOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

			if (page == 0) {
				ImGui::Text(L("WIZARD_WELCOME"));
				ImGui::Separator();
				ImGui::TextWrapped(L("WIZARD_INTRO1"));
				ImGui::TextWrapped(L("WIZARD_INTRO2"));
				ImGui::TextWrapped(L("WIZARD_INTRO3"));
				ImGui::Dummy(ImVec2(0.0f, 10.0f));
				if (ImGui::Button(L("BUTTON_START_CONFIG"), ImVec2(120, 0))) {
					page++;
				}
			}
			else if (page == 1) {
				ImGui::Text(L("WIZARD_STEP1_TITLE"));
				ImGui::TextWrapped(L("WIZARD_STEP1_DESC1"));
				ImGui::TextWrapped(L("WIZARD_STEP1_DESC2"));
				ImGui::Dummy(ImVec2(0.0f, 10.0f));

				// 找路径
				string pathTemp;
				if (ImGui::Button(L("BUTTON_AUTO_JAVA"))) {
					if (filesystem::exists((string)getenv("APPDATA") + "\\.minecraft\\saves")) {
						pathTemp = (string)getenv("APPDATA") + "\\.minecraft\\saves";
						strncpy_s(saveRootPath, pathTemp.c_str(), sizeof(saveRootPath));
					}
				}
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_AUTO_BEDROCK"))) { // 不能用 getenv，改成_dupenv_s了...
					if (filesystem::exists("C:\\Users\\" + (string)getenv("USERNAME") + "\\Appdata\\Local\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\LocalState\\games\\com.mojang\\minecraftWorlds")) {
						pathTemp = "C:\\Users\\" + (string)getenv("USERNAME") + "\\Appdata\\Local\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\LocalState\\games\\com.mojang\\minecraftWorlds";
						strncpy_s(saveRootPath, pathTemp.c_str(), sizeof(saveRootPath));
					}
				}
				if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
					wstring selected_folder = SelectFolderDialog();
					if (!selected_folder.empty()) {
						strncpy_s(saveRootPath, wstring_to_utf8(selected_folder).c_str(), sizeof(saveRootPath));
					}
				}
				ImGui::SameLine();
				ImGui::InputText(L("SAVES_ROOT_PATH"), saveRootPath, IM_ARRAYSIZE(saveRootPath));

				ImGui::Dummy(ImVec2(0.0f, 20.0f));
				if (ImGui::Button(L("BUTTON_NEXT"), ImVec2(120, 0))) {
					if (strlen(saveRootPath) > 0 && filesystem::exists(utf8_to_wstring(saveRootPath))) {
						page++;
						errorShow = false;
					}
					else {
						errorShow = true;
					}
				}
				if (errorShow)
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), L("WIZARD_PATH_EMPTY_OR_INVALID"));
			}
			else if (page == 2) {
				ImGui::Text(L("WIZARD_STEP2_TITLE"));
				ImGui::TextWrapped(L("WIZARD_STEP2_DESC"));
				ImGui::Dummy(ImVec2(0.0f, 10.0f));

				if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
					wstring selected_folder = SelectFolderDialog();
					if (!selected_folder.empty()) {
						strncpy_s(backupPath, wstring_to_utf8(selected_folder).c_str(), sizeof(backupPath));
					}
				}
				ImGui::SameLine();
				ImGui::InputText(L("WIZARD_BACKUP_PATH"), backupPath, IM_ARRAYSIZE(backupPath));

				ImGui::Dummy(ImVec2(0.0f, 20.0f));
				if (ImGui::Button(L("BUTTON_PREVIOUS"), ImVec2(120, 0))) page--;
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_NEXT"), ImVec2(120, 0))) {
					// 存在中文路径，要gbk
					if (strlen(backupPath) > 0 && filesystem::exists(utf8_to_wstring(backupPath))) {
						page++;
						errorShow = false;
					}
					else {
						errorShow = true;
					}
				}
				if (errorShow)
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), L("WIZARD_PATH_EMPTY_OR_INVALID"));
			}
			else if (page == 3) {
				ImGui::Text(L("WIZARD_STEP3_TITLE"));
				ImGui::TextWrapped(L("WIZARD_STEP3_DESC"));
				ImGui::Dummy(ImVec2(0.0f, 10.0f));
				// 检查内嵌的7z是否已释放成功
				if (sevenZipExtracted) {
					string extracted_path_utf8 = wstring_to_utf8(g_7zTempPath);
					strncpy_s(zipPath, extracted_path_utf8.c_str(), sizeof(zipPath));
					ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.4f, 1.0f), L("WIZARD_USING_EMBEDDED_7Z"));
				}
				else {
					// 如果释放失败，执行原来的自动检测逻辑
					if (filesystem::exists("7z.exe"))
					{
						strncpy_s(zipPath, "7z.exe", sizeof(zipPath));
						ImGui::Text(L("AUTODETECTED_7Z"));
					}
					else
					{
						static string zipTemp = GetRegistryValue("Software\\7-Zip", "Path") + "7z.exe";
						strncpy_s(zipPath, zipTemp.c_str(), sizeof(zipPath));
						if (strlen(zipPath) != 0)
							ImGui::Text(L("AUTODETECTED_7Z"));
					}
				}
				if (ImGui::Button(L("BUTTON_SELECT_FILE"))) {
					wstring selected_file = SelectFileDialog();
					if (!selected_file.empty()) {
						strncpy_s(zipPath, wstring_to_utf8(selected_file).c_str(), sizeof(zipPath));
					}
				}
				ImGui::SameLine();
				ImGui::InputText(L("WIZARD_7Z_PATH"), zipPath, IM_ARRAYSIZE(zipPath));

				ImGui::Dummy(ImVec2(0.0f, 20.0f));
				if (ImGui::Button(L("BUTTON_PREVIOUS"), ImVec2(120, 0))) page--;
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_FINISH_CONFIG"), ImVec2(120, 0))) {
					if (strlen(saveRootPath) > 0 && strlen(backupPath) > 0 && strlen(zipPath) > 0) {
						// 创建并填充第一个配置
						g_appState.currentConfigIndex = 1;
						Config& initialConfig = g_appState.configs[g_appState.currentConfigIndex];

						// 1. 保存向导中收集的路径
						initialConfig.saveRoot = utf8_to_wstring(saveRootPath);
						initialConfig.backupPath = utf8_to_wstring(backupPath);
						initialConfig.zipPath = utf8_to_wstring(zipPath);

						// 2. 自动扫描存档目录，填充世界列表
						if (filesystem::exists(initialConfig.saveRoot)) {
							for (auto& entry : filesystem::directory_iterator(initialConfig.saveRoot)) {
								if (entry.is_directory()) {
									// 针对基岩版的特殊处理：把 levelname.txt 里的内容当做文件描述
									
									if (filesystem::exists(entry.path() / "levelname.txt")) {
										ifstream levelNameFile(entry.path() / "levelname.txt");
										string levelName = "";
										getline(levelNameFile, levelName);
										levelNameFile.close();
										initialConfig.worlds.push_back({ entry.path().filename().wstring(), utf8_to_wstring(levelName) });
									}
									else {
										initialConfig.worlds.push_back({ entry.path().filename().wstring(), L"" }); // 名称为文件夹名，描述为空
									}
								}
							}
						}

						// 3. 设置合理的默认值
						initialConfig.zipFormat = L"7z";
						initialConfig.zipLevel = 5;
						initialConfig.keepCount = 0;
						initialConfig.backupMode = 1;
						initialConfig.hotBackup = false;
						initialConfig.backupBefore = false;
						initialConfig.skipIfUnchanged = true;
						isSilence = false;
#ifdef _WIN32
						if (g_CurrentLang == "zh_CN") {
							if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttc"))
								initialConfig.fontPath = L"C:\\Windows\\Fonts\\msyh.ttc";
							else if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttf"))
								initialConfig.fontPath = L"C:\\Windows\\Fonts\\msyh.ttf";
						}
						else
							initialConfig.fontPath = L"C:\\Windows\\Fonts\\SegoeUI.ttf";
#else
						if (filesystem::exists("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"))
							initialConfig.fontPath = L"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
						else if (filesystem::exists("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"))
							initialConfig.fontPath = L"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc";
						else
							initialConfig.fontPath = L"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
						g_appState.specialConfigs.clear();

						// 4. 保存到文件并切换到主应用界面
						SaveConfigs();
						showConfigWizard = false;
						g_appState.showMainApp = true;
					}
				}
				ImGui::Text(L("WIZARD_WARNING_TIPS"));
			}

			ImGui::End();
		}
		else if (!glfwGetWindowAttrib(wc, GLFW_ICONIFIED) && g_appState.showMainApp) {


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
			// --- 顶部菜单栏 ---
			if (ImGui::BeginMenuBar()) {

				if (ImGui::BeginMenu(L("MENU_FILE"))) {
					if (ImGui::MenuItem(L("EXIT"))) {
						g_appState.done = true;
						SaveConfigs();
					}
					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu(L("SETTINGS"))) {

					if (ImGui::Checkbox(L("RUN_ON_WINDOWS_STARTUP"), &g_RunOnStartup)) {
						wchar_t selfPath[MAX_PATH];
						GetModuleFileNameW(NULL, selfPath, MAX_PATH);
						SetAutoStart("MineBackup_AutoTask_" + to_string(g_appState.currentConfigIndex), selfPath, false, g_appState.currentConfigIndex, g_RunOnStartup);
					}
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", L("TIP_GLOBAL_STARTUP"));


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
					if (ImGui::BeginPopupModal(L("UPDATE_POPUP_TITLE"), &open_update_popup, ImGuiWindowFlags_AlwaysAutoResize)) {
						ImGui::Text(L("UPDATE_POPUP_HEADER"), g_LatestVersionStr.c_str());
						ImGui::Separator();
						ImGui::TextWrapped(L("UPDATE_POPUP_NOTES"));

						ImGui::BeginChild("ReleaseNotes", ImVec2(ImGui::GetContentRegionAvail().x, 450), true);
						ImGui::TextWrapped("%s", g_ReleaseNotes.c_str());
						ImGui::EndChild();
						ImGui::Separator();
						if (ImGui::Button(L("UPDATE_POPUP_DOWNLOAD_BUTTON"), ImVec2(180, 0))) {
							OpenLinkInBrowser(L"https://github.com/Leafuke/MineBackup/releases/download/" + utf8_to_wstring(g_LatestVersionStr) + L"/MineBackup.exe");
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						if (ImGui::Button(L("UPDATE_POPUP_DOWNLOAD_BUTTON_2"), ImVec2(180, 0))) {
							OpenLinkInBrowser(L"https://gh-proxy.com/https://github.com/Leafuke/MineBackup/releases/download/" + utf8_to_wstring(g_LatestVersionStr) + L"/MineBackup.exe");
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
				

				{
					float buttonSize = ImGui::GetFrameHeight();
					// 将按钮推到菜单栏的最右边
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttonSize * 3);

					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.4f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.1f, 0.1f, 0.5f));

					
					// Minimize Button
					if (ImGui::Button("-", ImVec2(buttonSize, buttonSize))) {
						g_appState.showMainApp = false;
						glfwHideWindow(wc);
					}
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("MINIMIZE_TO_TRAY_TIP"));

					ImGui::PopStyleColor(3);
				}


				ImGui::EndMenuBar();
			}
			if (showAboutWindow)
				ImGui::OpenPopup(L("MENU_ABOUT"));

			//ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			if (ImGui::BeginPopupModal(L("MENU_ABOUT"), &showAboutWindow, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::Text("MineBackup v%s", CURRENT_VERSION.c_str());
				ImGui::Separator();
				ImGui::TextWrapped("%s", L("ABOUT_DESCRIPTION"));
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

				ImGui::Dummy(ImVec2(0.0f, 10.0f));
				ImGui::SeparatorText(L("ABOUT_LICENSE_HEADER"));
				ImGui::Text("%s", L("ABOUT_LICENSE_TYPE"));
				ImGui::Text("%s", L("ABOUT_LICENSE_COPYRIGHT"));
				ImGui::Text("%s", L("ABOUT_LICENSE_TEXT"));

				ImGui::Dummy(ImVec2(0.0f, 10.0f));
				if (ImGui::Button(L("BUTTON_OK"), ImVec2(250, 0))) // 给按钮一个固定宽度以获得更好的观感
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
			// --- 动态调整世界图标纹理和尺寸向量的大小 ---
			vector<DisplayWorld> displayWorlds = BuildDisplayWorldsForSelection();
			int worldCount = (int)displayWorlds.size();


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
				if (ImGui::BeginPopupModal(L("CONFIRM_DELETE_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
					showDeleteConfigPopup = false;
					if (specialSetting) {
						ImGui::Text("[Sp.]");
						ImGui::SameLine();
						ImGui::Text(L("CONFIRM_DELETE_MSG"), g_appState.currentConfigIndex, g_appState.specialConfigs[g_appState.currentConfigIndex].name);
					}
					else {
						ImGui::Text(L("CONFIRM_DELETE_MSG"), g_appState.currentConfigIndex, g_appState.configs[g_appState.currentConfigIndex].name);
					}
					ImGui::Separator();
					if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
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
					if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
				// 添加新配置弹窗
				if (showAddConfigPopup)
					ImGui::OpenPopup(L("ADD_NEW_CONFIG_POPUP_TITLE"));
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

					if (ImGui::Button(L("CREATE_BUTTON"), ImVec2(120, 0))) {
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
					if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}

				ImGui::SeparatorText(L("WORLD_LIST"));

				// 新的自定义卡片
				//ImGui::BeginChild("WorldListChild", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3), true); // 预留底部按钮空间
				ImGui::BeginChild("WorldListChild", ImVec2(0, 0), true);

				// selectedWorldIndex 的语义现在改变了——它现在是 displayWorlds 的索引（而不是 cfg.worlds 的索引）


				for (int i = 0; i < worldCount; ++i) {
					const auto& dw = displayWorlds[i];
					ImGui::PushID(i);
					bool is_selected = (selectedWorldIndex == i);

					// worldFolder / backupFolder 基于 effectiveConfig
					wstring worldFolder = dw.effectiveConfig.saveRoot + L"\\" + dw.name;
					wstring backupFolder = dw.effectiveConfig.backupPath + L"\\" + dw.name;

					// --- 左侧图标区 ---
					ImDrawList* draw_list = ImGui::GetWindowDrawList();

					float iconSz = ImGui::GetTextLineHeightWithSpacing() * 2.5f * main_scale;
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

						string iconPath = utf8_to_gbk(wstring_to_utf8(worldFolder + L"\\icon.png"));
						string bedrockIconPath = utf8_to_gbk(wstring_to_utf8(worldFolder + L"\\world_icon.jpeg"));

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
							// 覆盖原 icon.png
							CopyFileW(sel.c_str(), (worldFolder + L"\\icon.png").c_str(), FALSE);
							// 释放旧纹理并重新加载
							if (current_texture) {
								glDeleteTextures(1, &current_texture);
							}
							GLuint newTextureId = 0;
							int tex_w = 0, tex_h = 0;
							LoadTextureFromFileGL(utf8_to_gbk(wstring_to_utf8(worldFolder + L"\\icon.png")).c_str(), &newTextureId, &tex_w, &tex_h);
							g_worldIconTextures[iconKey] = newTextureId;
							g_worldIconDimensions[iconKey] = ImVec2((float)tex_w, (float)tex_h);
						}
					}

					ImGui::SameLine();
					// --- 状态逻辑 (为图标做准备) ---
					lock_guard<mutex> lock(g_appState.task_mutex); // 访问 g_appState.g_active_auto_backups 需要加锁
					bool is_task_running = g_appState.g_active_auto_backups.count(make_pair(displayWorlds[i].baseConfigIndex, i)) > 0;
					// 如果最后打开时间比最后备份时间新，则认为需要备份
					//wstring worldFolder = cfg.saveRoot + L"\\" + cfg.worlds[i].first;
					bool needs_backup = GetLastOpenTime(worldFolder) > GetLastBackupTime(backupFolder);

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
						wstring worldFolder = displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot + L"\\" + displayWorlds[selectedWorldIndex].name;
						wstring backupFolder = displayWorlds[selectedWorldIndex].effectiveConfig.backupPath + L"\\" + displayWorlds[selectedWorldIndex].name;
						ImGui::Text("%s: %s", L("TABLE_LAST_OPEN"), wstring_to_utf8(GetLastOpenTime(worldFolder)).c_str());
						ImGui::Text("%s: %s", L("TABLE_LAST_BACKUP"), wstring_to_utf8(GetLastBackupTime(backupFolder)).c_str());

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
							thread backup_thread(DoBackup, displayWorlds[selectedWorldIndex].effectiveConfig, make_pair(displayWorlds[selectedWorldIndex].name, displayWorlds[selectedWorldIndex].desc), ref(console), utf8_to_wstring(backupComment));
							backup_thread.detach();
							strcpy_s(backupComment, "");
						}
						ImGui::SameLine();
						if (ImGui::Button(L("BUTTON_AUTO_BACKUP_SELECTED"), ImVec2(button_width, 0))) {
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
							wstring path = displayWorlds[selectedWorldIndex].effectiveConfig.backupPath + L"\\" + displayWorlds[selectedWorldIndex].name;
							if (filesystem::exists(path)) {
								OpenFolder(path);
							}
							else {
								OpenFolder(displayWorlds[selectedWorldIndex].effectiveConfig.backupPath);
							}
						}
						if (ImGui::Button(L("OPEN_SAVEROOT_FOLDER"), ImVec2(-1, 0))) {
							wstring path = displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot + L"\\" + displayWorlds[selectedWorldIndex].name;
							OpenFolder(path);
						}

						// 模组备份
						if (ImGui::Button(L("BUTTON_BACKUP_MODS"), ImVec2(-1, 0))) {
							if (selectedWorldIndex != -1) {
								ImGui::OpenPopup(L("CONFIRM_BACKUP_OTHERS_TITLE"));
							}
						}

						if (ImGui::BeginPopupModal(L("CONFIRM_BACKUP_OTHERS_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
							static char mods_comment[256] = "";
							ImGui::TextUnformatted(L("CONFIRM_BACKUP_OTHERS_MSG"));
							ImGui::InputText(L("HINT_BACKUP_COMMENT"), mods_comment, IM_ARRAYSIZE(mods_comment));
							ImGui::Separator();

							if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
								if (g_appState.configs.count(g_appState.currentConfigIndex)) {
									filesystem::path tempPath = displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot;
									filesystem::path modsPath = tempPath.parent_path() / "mods";
									if (!filesystem::exists(modsPath) && filesystem::exists(tempPath / "mods")) { // 服务器的模组可能放在world同级文件夹下
										modsPath = tempPath / "mods";
									}
									thread backup_thread(DoOthersBackup, g_appState.configs[g_appState.currentConfigIndex], modsPath, utf8_to_wstring(mods_comment));
									backup_thread.detach();
									strcpy_s(mods_comment, "");
								}
								ImGui::CloseCurrentPopup();
							}
							ImGui::SameLine();
							if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
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
						strcpy(buf, wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.othersPath).c_str());
						if (ImGui::InputTextWithHint("##OTHERS", L("HINT_BACKUP_WHAT"), buf, IM_ARRAYSIZE(buf))) {
							displayWorlds[selectedWorldIndex].effectiveConfig.othersPath = utf8_to_wstring(buf);
							g_appState.configs[displayWorlds[selectedWorldIndex].baseConfigIndex].othersPath = displayWorlds[selectedWorldIndex].effectiveConfig.othersPath;
						}

						if (ImGui::BeginPopupModal("Others", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
							static char others_comment[CONSTANT1] = "";
							ImGui::TextUnformatted(L("CONFIRM_BACKUP_OTHERS_MSG"));
							ImGui::InputText(L("HINT_BACKUP_COMMENT"), others_comment, IM_ARRAYSIZE(others_comment));
							ImGui::Separator();

							if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
								thread backup_thread(DoOthersBackup, displayWorlds[selectedWorldIndex].effectiveConfig, utf8_to_wstring(buf), utf8_to_wstring(others_comment));
								backup_thread.detach();
								strcpy_s(others_comment, "");
								SaveConfigs(); // 保存一下路径
								ImGui::CloseCurrentPopup();
							}
							ImGui::SameLine();
							if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
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
								wstring rclone_command = L"\"" + config.rclonePath + L"\" copy \"" + config.backupPath + L"\\" + displayWorlds[selectedWorldIndex].name + L"\" \"" + config.rcloneRemotePath + L"\" --progress";
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
							if (ImGui::Button(L("BUTTON_EXPORT"), ImVec2(120, 0))) {
								const auto& dw = displayWorlds[selectedWorldIndex];

								wstring worldFullPath = dw.effectiveConfig.saveRoot + L"\\" + dw.name;

								thread export_thread(DoExportForSharing, tempExportConfig, dw.name, worldFullPath, utf8_to_wstring(outputPathBuf), utf8_to_wstring(descBuf), ref(console));
								export_thread.detach();

								ImGui::CloseCurrentPopup();
							}
							ImGui::SameLine();
							if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
								ImGui::CloseCurrentPopup();
							}

							ImGui::EndPopup();
						}


					}

					// 自动备份弹窗
					if (ImGui::BeginPopupModal(L("AUTOBACKUP_SETTINGS"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
						bool is_task_running = false;
						pair<int, int> taskKey = { -1,-1 };
						vector<DisplayWorld> localDisplayWorlds; // 供显示使用
						{
							lock_guard<mutex> lock(g_appState.task_mutex);
							if (selectedWorldIndex >= 0) {
								// 如果使用 displayWorlds：
								localDisplayWorlds = BuildDisplayWorldsForSelection();
								if (selectedWorldIndex < (int)localDisplayWorlds.size()) {
									taskKey = { localDisplayWorlds[selectedWorldIndex].baseConfigIndex, localDisplayWorlds[selectedWorldIndex].baseWorldIndex };
									is_task_running = (g_appState.g_active_auto_backups.count(taskKey) > 0);
								}
							}
						}

						if (is_task_running) {
							ImGui::Text(L("AUTOBACKUP_RUNNING"), wstring_to_utf8(localDisplayWorlds[selectedWorldIndex].name).c_str());
							ImGui::Separator();
							if (ImGui::Button(L("BUTTON_STOP_AUTOBACKUP"), ImVec2(240, 0))) {
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
							if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
								ImGui::CloseCurrentPopup();
							}
						}
						else {
							ImGui::Text(L("AUTOBACKUP_SETUP_FOR"), wstring_to_utf8(BuildDisplayWorldsForSelection()[selectedWorldIndex].name).c_str());
							ImGui::Separator();
							ImGui::InputInt(L("INTERVAL_MINUTES"), &last_interval);
							if (last_interval < 1) last_interval = 1;
							if (ImGui::Button(L("BUTTON_START"), ImVec2(120, 0))) {
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
							if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
								ImGui::CloseCurrentPopup();
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
			

			if (showSettings) ShowSettingsWindow();
			if (showHistoryWindow) {
				if (specialSetting) {
					if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size())
						ShowHistoryWindow(displayWorlds[selectedWorldIndex].baseConfigIndex);
					else if (!g_appState.specialConfigs[g_appState.currentConfigIndex].tasks.empty())
						ShowHistoryWindow(g_appState.specialConfigs[g_appState.currentConfigIndex].tasks[0].configIndex);
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

		// Update and Render additional Platform Windows
		// (Platform functions may change the current OpenGL context, so we save/restore it to make it easier to paste this code elsewhere.
		//  For this specific demo app we could also call glfwMakeContextCurrent(window) directly)
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
	SaveConfigs();
#ifdef _WIN32
	RemoveTrayIcon();
	UnregisterHotkeys(hwnd_hidden);
	DestroyWindow(hwnd_hidden);
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

	//linkLoaderThread.join();
	if (g_commandResponser) {
		//delete g_commandResponser;
		g_commandResponser = nullptr;
	}
	if (g_signalSender) {
		//delete g_signalSender;
		g_signalSender = nullptr;
	}

	return 0;
}


bool LoadTextureFromFileGL(const char* filename, GLuint* out_texture, int* out_width, int* out_height)
{
	// 从文件加载图像数据
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
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// 上传纹理数据
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_width, image_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
	stbi_image_free(image_data); // 释放CPU内存

	*out_texture = image_texture;
	*out_width = image_width;
	*out_height = image_height;

	return true;
}

inline void ApplyTheme(int& theme)
{
	switch (theme) {
	case 0: ImGui::StyleColorsDark(); break;
	case 1: ImGui::StyleColorsLight(); break;
	case 2: ImGui::StyleColorsClassic(); break;
	}
}




void ShowSettingsWindow() {
	ImGui::Begin(L("SETTINGS"), &showSettings, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
	ImGui::SeparatorText(L("CONFIG_MANAGEMENT"));

	string current_config_label = "None";
	if (g_appState.specialConfigs.count(g_appState.currentConfigIndex)) {
		specialSetting = true;
		current_config_label = "[Sp." + to_string(g_appState.currentConfigIndex) + "] " + g_appState.specialConfigs[g_appState.currentConfigIndex].name;
	}
	else if (g_appState.configs.count(g_appState.currentConfigIndex)) {
		specialSetting = false;
		current_config_label = "[No." + to_string(g_appState.currentConfigIndex) + "] " + g_appState.configs[g_appState.currentConfigIndex].name;
	}
	else {
		return;
	}
	//string(L("CONFIG_N")) + to_string(g_appState.currentConfigIndex)
	static bool showAddConfigPopup = false, showDeleteConfigPopup = false;
	if (ImGui::BeginCombo(L("CURRENT_CONFIG"), current_config_label.c_str())) {
		// 普通配置
		for (auto const& [idx, val] : g_appState.configs) {
			const bool is_selected = (g_appState.currentConfigIndex == idx);
			string label = "[No." + to_string(idx) + "] " + val.name;

			if (ImGui::Selectable(label.c_str(), is_selected)) {
				g_appState.currentConfigIndex = idx;
				specialSetting = false;
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
	if (showDeleteConfigPopup) {
		ImGui::OpenPopup(L("CONFIRM_DELETE_TITLE"));
	}
	if (ImGui::BeginPopupModal(L("CONFIRM_DELETE_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		showDeleteConfigPopup = false;
		if (specialSetting) {
			ImGui::Text("[Sp.]");
			ImGui::SameLine();
			ImGui::Text(L("CONFIRM_DELETE_MSG"), g_appState.currentConfigIndex, g_appState.specialConfigs[g_appState.currentConfigIndex].name);
		}
		else {
			ImGui::Text(L("CONFIRM_DELETE_MSG"), g_appState.currentConfigIndex, g_appState.configs[g_appState.currentConfigIndex].name);
		}
		ImGui::Separator();
		if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
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
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	// 添加新配置弹窗
	if (showAddConfigPopup) {
		ImGui::OpenPopup(L("ADD_NEW_CONFIG_POPUP_TITLE"));
	}
	if (ImGui::BeginPopupModal(L("ADD_NEW_CONFIG_POPUP_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		showAddConfigPopup = false;
		static int config_type = 0; // 0 for Normal, 1 for Special
		static char new_config_name[128] = "New Config";

		ImGui::Text(L("CONFIG_TYPE_LABEL"));
		ImGui::RadioButton(L("CONFIG_TYPE_NORMAL"), &config_type, 0); ImGui::SameLine();
		ImGui::RadioButton(L("CONFIG_TYPE_SPECIAL"), &config_type, 1);

		ImGui::InputText(L("NEW_CONFIG_NAME_LABEL"), new_config_name, IM_ARRAYSIZE(new_config_name));
		ImGui::Separator();

		if (ImGui::Button(L("CREATE_BUTTON"), ImVec2(120, 0))) {
			if (strlen(new_config_name) > 0) {
				if (config_type == 0) {
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
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}



	ImGui::Dummy(ImVec2(0.0f, 10.0f));
	ImGui::SeparatorText(L("CURRENT_CONFIG_DETAILS"));

	if (specialSetting) {
		if (!g_appState.specialConfigs.count(g_appState.currentConfigIndex)) {
			specialSetting = false;
			g_appState.currentConfigIndex = g_appState.configs.empty() ? 1 : g_appState.configs.begin()->first;
		}
		else {
			SpecialConfig& spCfg = g_appState.specialConfigs[g_appState.currentConfigIndex];

			char buf[128];
			strncpy_s(buf, spCfg.name.c_str(), sizeof(buf));
			if (ImGui::InputText(L("CONFIG_NAME"), buf, sizeof(buf))) spCfg.name = buf;

			if (ImGui::BeginTable("sp_cfg_layout", 2, ImGuiTableFlags_BordersInnerV)) {
				ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch);

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);

				ImGui::SeparatorText(L("GROUP_STARTUP_BEHAVIOR"));
				ImGui::Checkbox(L("EXECUTE_ON_STARTUP"), &spCfg.autoExecute);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_EXECUTE_ON_STARTUP"));
				ImGui::Checkbox(L("EXIT_WHEN_FINISHED"), &spCfg.exitAfterExecution);
				if (ImGui::Checkbox(L("RUN_ON_WINDOWS_STARTUP"), &spCfg.runOnStartup)) {
					wchar_t selfPath[MAX_PATH];
					GetModuleFileNameW(NULL, selfPath, MAX_PATH);
					SetAutoStart("MineBackup_AutoTask_" + to_string(g_appState.currentConfigIndex), selfPath, true, g_appState.currentConfigIndex, spCfg.runOnStartup);
				}
				ImGui::Checkbox(L("HIDE_CONSOLE_WINDOW"), &spCfg.hideWindow);

				ImGui::TableSetColumnIndex(1);

				ImGui::SeparatorText(L("GROUP_BACKUP_OVERRIDES"));
				ImGui::Checkbox(L("IS_HOT_BACKUP"), &spCfg.hotBackup);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_HOT_BACKUP"));
				ImGui::Checkbox(L("BACKUP_ON_START"), &spCfg.backupOnGameStart);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_BACKUP_ON_START"));
				ImGui::Checkbox(L("USE_LOW_PRIORITY"), &spCfg.useLowPriority);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_LOW_PRIORITY"));

				int max_threads = thread::hardware_concurrency();
				ImGui::SliderInt(L("CPU_THREAD_COUNT"), &spCfg.cpuThreads, 0, max_threads);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_CPU_THREADS"));

				ImGui::EndTable();
			}

			if (ImGui::RadioButton(L("THEME_DARK"), &spCfg.theme, 0)) { ApplyTheme(spCfg.theme); } ImGui::SameLine();
			if (ImGui::RadioButton(L("THEME_LIGHT"), &spCfg.theme, 1)) { ApplyTheme(spCfg.theme); } ImGui::SameLine();
			if (ImGui::RadioButton(L("THEME_CLASSIC"), &spCfg.theme, 2)) { ApplyTheme(spCfg.theme); }

			if (ImGui::Button(L("BUTTON_SWITCH_TO_SP_MODE"))) {
				g_appState.specialConfigs[g_appState.currentConfigIndex].autoExecute = true;
				SaveConfigs();
				ReStartApplication();
				g_appState.done = true;
			}

			//ImGui::SeparatorText(L("BLACKLIST_HEADER"));
			//if (ImGui::Button(L("BUTTON_ADD_FILE_BLACKLIST"))) {
			//	wstring sel = SelectFileDialog(); if (!sel.empty()) spCfg.blacklist.push_back(sel);
			//}
			//ImGui::SameLine();
			//if (ImGui::Button(L("BUTTON_ADD_FOLDER_BLACKLIST"))) {
			//	wstring sel = SelectFolderDialog(); if (!sel.empty()) spCfg.blacklist.push_back(sel);
			//}
			//static int sel_bl_item = -1;
			//if (ImGui::BeginListBox("##blacklist", ImVec2(ImGui::GetContentRegionAvail().x, 2 * ImGui::GetTextLineHeightWithSpacing()))) {
			//	// 检查 blacklist 是否为空
			//	if (spCfg.blacklist.empty()) {
			//		ImGui::Text(L("No items in blacklist")); // 显示空列表提示
			//	}
			//	else {
			//		// 遍历显示黑名单项
			//		for (int n = 0; n < spCfg.blacklist.size(); n++) {
			//			string label = wstring_to_utf8(spCfg.blacklist[n]);
			//			if (ImGui::Selectable(label.c_str(), sel_bl_item == n)) {
			//				sel_bl_item = n;
			//			}
			//		}
			//	}
			//	ImGui::EndListBox();
			//}

			ImGui::SeparatorText(L("COMMANDS_TO_RUN"));
			static char cmd_buf[512] = "";
			if (ImGui::InputText("##cmd_input", cmd_buf, sizeof(cmd_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
				if (strlen(cmd_buf) > 0) {
					spCfg.commands.push_back(utf8_to_wstring(cmd_buf));
					strcpy_s(cmd_buf, "");
				}
			}
			ImGui::SameLine();
			if (ImGui::Button(L("ADD_COMMAND"))) {
				if (strlen(cmd_buf) > 0) {
					spCfg.commands.push_back(utf8_to_wstring(cmd_buf));
					strcpy_s(cmd_buf, "");
				}
			}
			static int sel_cmd_item = -1;
			ImGui::BeginListBox("##commands_list", ImVec2(-FLT_MIN, 3 * ImGui::GetTextLineHeightWithSpacing()));
			for (int n = 0; n < spCfg.commands.size(); n++) {
				if (ImGui::Selectable(wstring_to_utf8(spCfg.commands[n]).c_str(), sel_cmd_item == n)) sel_cmd_item = n;
			}
			ImGui::EndListBox();
			if (ImGui::Button(L("BUTTON_REMOVE_COMMAND")) && sel_cmd_item != -1) {
				spCfg.commands.erase(spCfg.commands.begin() + sel_cmd_item);
				sel_cmd_item = -1;
			}

			ImGui::SeparatorText(L("AUTOMATED_TASKS"));
			if (ImGui::Button(L("ADD_BACKUP_TASK"))) spCfg.tasks.push_back(AutomatedTask());
			ImGui::SameLine();
			static int sel_task_item = -1; // 追踪被删除的item
			if (ImGui::Button(L("BUTTON_REMOVE_TASK")) && sel_task_item != -1 && sel_task_item < spCfg.tasks.size()) {
				spCfg.tasks.erase(spCfg.tasks.begin() + sel_task_item);
				sel_task_item = -1;
			}


			for (int i = 0; i < spCfg.tasks.size(); ++i) {
				ImGui::PushID(2000 + i);
				ImGui::Separator();

				string task_label = "Task " + to_string(i + 1);
				if (ImGui::Selectable(task_label.c_str(), sel_task_item == i)) {
					sel_task_item = i;
				}

				AutomatedTask& task = spCfg.tasks[i];

				string current_task_config_label = g_appState.configs.count(task.configIndex) ? (string(L("CONFIG_N")) + to_string(task.configIndex)) : "None";
				if (ImGui::BeginCombo(L("CONFIG_COMBO"), current_task_config_label.c_str())) {
					for (auto const& [idx, val] : g_appState.configs) {
						if (ImGui::Selectable((string(L("CONFIG_N")) + to_string(idx)).c_str(), task.configIndex == idx)) {
							task.configIndex = idx;
							task.worldIndex = val.worlds.empty() ? -1 : 0; // 重置世界idx
						}
					}
					ImGui::EndCombo();
				}

				if (g_appState.configs.count(task.configIndex)) {
					Config& selected_cfg = g_appState.configs[task.configIndex];
					string current_world_label = "None";
					if (!selected_cfg.worlds.empty() && task.worldIndex >= 0 && task.worldIndex < selected_cfg.worlds.size()) {
						current_world_label = wstring_to_utf8(selected_cfg.worlds[task.worldIndex].first);
					}
					if (ImGui::BeginCombo(L("WORLD_COMBO"), current_world_label.c_str())) {
						for (int w_idx = 0; w_idx < selected_cfg.worlds.size(); ++w_idx) {
							if (ImGui::Selectable(wstring_to_utf8(selected_cfg.worlds[w_idx].first).c_str(), task.worldIndex == w_idx)) {
								task.worldIndex = w_idx;
							}
						}
						ImGui::EndCombo();
					}
				}

				const char* modeList[] = { L("SCHED_MODES_ONCE"), L("SCHED_MODES_INTERVAL"), L("SCHED_MODES_SCHED") };
				ImGui::Combo(L("TASK_BACKUP_TYPE"), &task.backupType, modeList, IM_ARRAYSIZE(modeList));

				if (task.backupType == 1) { // 间隔
					ImGui::InputInt(L("INTERVAL_MINUTES"), &task.intervalMinutes);
					if (task.intervalMinutes < 1) task.intervalMinutes = 1;
				}
				else if (task.backupType == 2) { // 计划
					ImGui::Text("At:"); ImGui::SameLine();
					ImGui::SetNextItemWidth(100); ImGui::InputInt(L("SCHED_HOUR"), &task.schedHour);
					ImGui::SameLine(); ImGui::Text(":"); ImGui::SameLine();
					ImGui::SetNextItemWidth(100); ImGui::InputInt(L("SCHED_MINUTE"), &task.schedMinute);
					ImGui::SameLine(); ImGui::Text("On (Month/Day):"); ImGui::SameLine();
					ImGui::SetNextItemWidth(100); ImGui::InputInt(L("SCHED_MONTH"), &task.schedMonth);
					ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
					ImGui::SetNextItemWidth(100); ImGui::InputInt(L("SCHED_DAY"), &task.schedDay);
					ImGui::SameLine(); ImGui::TextDisabled("(0=Every)");

					task.schedHour = clamp(task.schedHour, 0, 23);
					task.schedMinute = clamp(task.schedMinute, 0, 59);
					task.schedMonth = clamp(task.schedMonth, 0, 12);
					task.schedDay = clamp(task.schedDay, 0, 31);
				}
				ImGui::PopID();
			}
		}
	}
	else {
		if (!g_appState.configs.count(g_appState.currentConfigIndex)) {
			// 如果配置被删除
			if (g_appState.configs.empty()) g_appState.configs[1] = Config(); // 如果1被删，新建
			g_appState.currentConfigIndex = g_appState.configs.begin()->first;
		}
		Config& cfg = g_appState.configs[g_appState.currentConfigIndex];
		char buf[128];
		strncpy_s(buf, cfg.name.c_str(), sizeof(buf));
		if (ImGui::InputText(L("CONFIG_NAME"), buf, sizeof(buf))) cfg.name = buf;

		if (ImGui::CollapsingHeader(L("GROUP_PATHS"))) {
			char rootBufA[CONSTANT1];
			strncpy_s(rootBufA, wstring_to_utf8(cfg.saveRoot).c_str(), sizeof(rootBufA));
			if (ImGui::Button(L("BUTTON_SELECT_SAVES_DIR"))) {
				wstring sel = SelectFolderDialog();
				if (!sel.empty()) {
					cfg.saveRoot = sel;
				}
			}
			ImGui::SameLine();
			if (ImGui::InputText(L("SAVES_ROOT_PATH"), rootBufA, CONSTANT1)) {
				cfg.saveRoot = utf8_to_wstring(rootBufA);
			}

			char buf[CONSTANT1];
			strncpy_s(buf, wstring_to_utf8(cfg.backupPath).c_str(), sizeof(buf));
			if (ImGui::Button(L("BUTTON_SELECT_BACKUP_DIR"))) {
				wstring sel = SelectFolderDialog();
				if (!sel.empty()) {
					cfg.backupPath = sel;
				}
			}
			//ImVec2 item_width = ImGui::CalcTextSize(L("BUTTON_SELECT_BACKUP_DIR"));
			//item_width.x = abs(ImGui::CalcTextSize(L("BUTTON_SELECT_SAVES_DIR")).x - ImGui::CalcTextSize(L("BUTTON_SELECT_BACKUP_DIR")).x);
			/*ImGui::SameLine();
			ImGui::InvisibleButton("##width", item_width);*/
			ImGui::SameLine();
			if (ImGui::InputText(L("BACKUP_DEST_PATH_LABEL"), buf, CONSTANT1)) {
				cfg.backupPath = utf8_to_wstring(buf);
			}

			char zipBuf[CONSTANT1];
			strncpy_s(zipBuf, wstring_to_utf8(cfg.zipPath).c_str(), sizeof(zipBuf));
			if (filesystem::exists("7z.exe") && cfg.zipPath.empty()) {
				cfg.zipPath = L"7z.exe";
				ImGui::Text(L("AUTODETECTED_7Z"));
			}
			else if (cfg.zipPath.empty()) {
				string zipPathStr = GetRegistryValue("Software\\7-Zip", "Path") + "7z.exe";
				if (filesystem::exists(zipPathStr)) {
					cfg.zipPath = utf8_to_wstring(zipPathStr);
					ImGui::Text(L("AUTODETECTED_7Z"));
				}
			}
			if (ImGui::Button(L("BUTTON_SELECT_7Z"))) {
				wstring sel = SelectFileDialog();
				if (!sel.empty()) {
					cfg.zipPath = sel;
				}
			}
			ImGui::SameLine();
			if (ImGui::InputText(L("7Z_PATH_LABEL"), zipBuf, CONSTANT1)) {
				cfg.zipPath = utf8_to_wstring(zipBuf);
			}

			char snapshotPathBuf[CONSTANT1];
			strncpy_s(snapshotPathBuf, wstring_to_utf8(cfg.snapshotPath).c_str(), sizeof(snapshotPathBuf));
			if (ImGui::Button(L("BUTTON_SELECT_SNAPSHOT_DIR"))) {
				wstring sel = SelectFolderDialog();
				if (!sel.empty()) {
					cfg.snapshotPath = sel;
				}
			}
			ImGui::SameLine();
			if (ImGui::InputText(L("SNAPSHOT_PATH"), snapshotPathBuf, CONSTANT1)) {
				cfg.snapshotPath = utf8_to_wstring(snapshotPathBuf);
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_SNAPSHOT_PATH"));
		}



		if (ImGui::CollapsingHeader(L("GROUP_WORLD_MANAGEMENT"))) {
			// Auto-scan worlds
			if (ImGui::Button(L("BUTTON_SCAN_SAVES"))) {
				cfg.worlds.clear();
				if (filesystem::exists(cfg.saveRoot))
					for (auto& e : filesystem::directory_iterator(cfg.saveRoot))
						if (e.is_directory())
							cfg.worlds.push_back({ e.path().filename().wstring(), L"" });
			}

			// World name + description editor
			ImGui::Separator();
			ImGui::Text(L("WORLD_NAME_AND_DESC"));
			for (size_t i = 0; i < cfg.worlds.size(); ++i) {
				ImGui::PushID(int(i));
				char name[CONSTANT1], desc[CONSTANT1];
				strncpy_s(name, wstring_to_utf8(cfg.worlds[i].first).c_str(), sizeof(name));
				strncpy_s(desc, wstring_to_utf8(cfg.worlds[i].second).c_str(), sizeof(desc));

				if (ImGui::InputText(L("WORLD_NAME"), name, CONSTANT1))
					cfg.worlds[i].first = utf8_to_wstring(name);
				if (cfg.worlds[i].second.find(L"\"") != wstring::npos || cfg.worlds[i].second.find(L":") != wstring::npos || cfg.worlds[i].second.find(L"\\") != wstring::npos || cfg.worlds[i].second.find(L"/") != wstring::npos || cfg.worlds[i].second.find(L">") != wstring::npos || cfg.worlds[i].second.find(L"<") != wstring::npos || cfg.worlds[i].second.find(L"|") != wstring::npos || cfg.worlds[i].second.find(L"?") != wstring::npos || cfg.worlds[i].second.find(L"*") != wstring::npos) {
					memset(desc, '\0', sizeof(desc));
					cfg.worlds[i].second = L"";
				}
				if (ImGui::InputText(L("WORLD_DESC"), desc, CONSTANT1))
					cfg.worlds[i].second = utf8_to_wstring(desc);

				ImGui::PopID();
			}
		}

		// 备份行为
		if (ImGui::CollapsingHeader(L("GROUP_BACKUP_BEHAVIOR"), ImGuiTreeNodeFlags_DefaultOpen)) {
			static int format_choice = (cfg.zipFormat == L"zip") ? 1 : 0;
			ImGui::Text(L("COMPRESSION_FORMAT")); ImGui::SameLine();
			if (ImGui::RadioButton("7z", &format_choice, 0)) { cfg.zipFormat = L"7z"; } ImGui::SameLine();
			if (ImGui::RadioButton("zip", &format_choice, 1)) { cfg.zipFormat = L"zip"; }


			ImGui::Text(L("TEXT_BACKUP_MODE")); ImGui::SameLine();
			ImGui::RadioButton(L("BUTTOM_BACKUP_MODE_NORMAL"), &cfg.backupMode, 1);
			ImGui::SameLine();
			ImGui::RadioButton(L("BUTTOM_BACKUP_MODE_SMART"), &cfg.backupMode, 2);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(L("TIP_SMART_BACKUP"));
			}
			ImGui::SameLine();
			ImGui::RadioButton(L("BUTTOM_BACKUP_MODE_OVERWRITE"), &cfg.backupMode, 3);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(L("TIP_OVERWRITE_BACKUP"));
			}
			ImGui::Text(L("BACKUP_NAMING"));
			ImGui::SameLine();
			int folder_name_choice = (int)cfg.folderNameType;
			if (ImGui::RadioButton(L("NAME_BY_WORLD"), &folder_name_choice, 0)) { cfg.folderNameType = 0; } ImGui::SameLine();
			if (ImGui::RadioButton(L("NAME_BY_DESC"), &folder_name_choice, 1)) { cfg.folderNameType = 1; }

			ImGui::Checkbox(L("IS_HOT_BACKUP"), &cfg.hotBackup); ImGui::SameLine();
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(L("TIP_HOT_BACKUP"));
			}
			ImGui::Checkbox(L("BACKUP_ON_START"), &cfg.backupOnGameStart);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_BACKUP_ON_START"));
			// 低优先级
			ImGui::Checkbox(L("USE_LOW_PRIORITY"), &cfg.useLowPriority);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(L("TIP_LOW_PRIORITY"));
			}
			ImGui::SameLine();
			ImGui::Checkbox(L("SKIP_IF_UNCHANGED"), &cfg.skipIfUnchanged);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(L("TIP_SKIP_IF_UNCHANGED"));
			}

			const char* zip_methods[] = { "LZMA2", "Deflate", "BZip2", "ZStandard" };
			int method_idx = 0;
			for (int i = 0; i < IM_ARRAYSIZE(zip_methods); ++i) {
				if (_wcsicmp(cfg.zipMethod.c_str(), utf8_to_wstring(zip_methods[i]).c_str()) == 0) {
					method_idx = i;
					break;
				}
			}
			// 在压缩格式为 zip 时默认使用 Deflate
			if (cfg.zipFormat == L"zip" && method_idx == 0) {
				method_idx = 1;
				cfg.zipMethod = L"Deflate";
			}
			else if (cfg.zipFormat == L"7z" && method_idx == 1) {
				method_idx = 0;
				cfg.zipMethod = L"LZMA2";
			}

			if (ImGui::Combo(L("COMPRESSION_METHOD"), &method_idx, zip_methods, IM_ARRAYSIZE(zip_methods))) {
				cfg.zipMethod = utf8_to_wstring(zip_methods[method_idx]);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", L("TIP_COMPRESSION_METHOD"));
			}
			
			

			// CPU 线程
			int max_threads = thread::hardware_concurrency();
			ImGui::SliderInt(L("CPU_THREAD_COUNT"), &cfg.cpuThreads, 0, max_threads);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(L("TIP_CPU_THREADS"));
			}
			ImGui::SliderInt(L("COMPRESSION_LEVEL"), &cfg.zipLevel, 0, 9);
			ImGui::InputInt(L("BACKUPS_TO_KEEP"), &cfg.keepCount);
			ImGui::SameLine();
			ImGui::Checkbox(L("IS_SAFE_DELETE"), &isSafeDelete);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("IS_SAFE_DELETE_TIP"));
			ImGui::InputInt(L("MAX_SMART_BACKUPS"), &cfg.maxSmartBackupsPerFull, 1, 5);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_MAX_SMART_BACKUPS"));
			ImGui::SeparatorText(L("BLACKLIST_HEADER"));
			if (ImGui::Button(L("BUTTON_ADD_FILE_BLACKLIST"))) {
				wstring sel = SelectFileDialog();
				if (!sel.empty()) cfg.blacklist.push_back(sel); // 或 spCfg.blacklist
			}
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_ADD_FOLDER_BLACKLIST"))) {
				wstring sel = SelectFolderDialog();
				if (!sel.empty()) cfg.blacklist.push_back(sel);
			}
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_ADD_REGEX_BLACKLIST"))) {
				ImGui::OpenPopup("Add Regex Rule");
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", L("TIP_USE_REGEX"));

			static int sel_bl_item = -1;
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_REMOVE_BLACKLIST")) && sel_bl_item != -1) {
				cfg.blacklist.erase(cfg.blacklist.begin() + sel_bl_item); sel_bl_item = -1;
			}

			// 添加正则表达式规则的弹窗
			if (ImGui::BeginPopupModal("Add Regex Rule", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
				static char regex_buf[256] = "regex:";
				ImGui::InputText("Regex Pattern", regex_buf, IM_ARRAYSIZE(regex_buf));
				ImGui::Separator();
				if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
					if (strlen(regex_buf) > 6) { // 确保 "regex:" 后面有内容
						// 根据当前是普通配置还是特殊配置，添加到对应的黑名单
						if (specialSetting) {
							g_appState.specialConfigs[g_appState.currentConfigIndex].blacklist.push_back(utf8_to_wstring(regex_buf));
						}
						else {
							g_appState.configs[g_appState.currentConfigIndex].blacklist.push_back(utf8_to_wstring(regex_buf));
						}
					}
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}

			if (ImGui::BeginListBox("##blacklist", ImVec2(ImGui::GetContentRegionAvail().x, 2 * ImGui::GetTextLineHeightWithSpacing()))) {
				// 检查 blacklist 是否为空
				if (cfg.blacklist.empty()) {
					ImGui::Text(L("No items in blacklist")); // 显示空列表提示
				}
				else {
					// 遍历显示黑名单项
					for (int n = 0; n < cfg.blacklist.size(); n++) {
						string label = wstring_to_utf8(cfg.blacklist[n]);
						if (ImGui::Selectable(label.c_str(), sel_bl_item == n)) {
							sel_bl_item = n;
						}
						if (ImGui::IsItemHovered()) {
							// 如果鼠标悬停，则设置一个Tooltip显示完整内容
							ImGui::SetTooltip("%s", label.c_str());
						}
					}
				}
				ImGui::EndListBox();
			}
		}

		// 还原行为
		if (ImGui::CollapsingHeader(L("GROUP_RESTORE_BEHAVIOR"))) {
			ImGui::Checkbox(L("BACKUP_BEFORE_RESTORE"), &cfg.backupBefore);

			ImGui::SeparatorText(L("RESTORE_WHITELIST_HEADER"));
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_WHITELIST"));

			// 添加新项的输入框
			static char whitelist_add_buf[256] = "";
			ImGui::InputTextWithHint("##whitelist_add", "file_or_folder_name", whitelist_add_buf, IM_ARRAYSIZE(whitelist_add_buf));
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_ADD_WHITELIST")) && strlen(whitelist_add_buf) > 0) {
				restoreWhitelist.push_back(utf8_to_wstring(whitelist_add_buf));
				strcpy_s(whitelist_add_buf, "");
			}

			// 白名单列表
			static int sel_wl_item = -1;
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_REMOVE_WHITELIST")) && sel_wl_item != -1) {
				restoreWhitelist.erase(restoreWhitelist.begin() + sel_wl_item);
				sel_wl_item = -1;
			}

			if (ImGui::BeginListBox("##whitelist", ImVec2(ImGui::GetContentRegionAvail().x, 4 * ImGui::GetTextLineHeightWithSpacing()))) {
				if (restoreWhitelist.empty()) {
					ImGui::TextDisabled("Whitelist is empty.");
				}
				else {
					for (int n = 0; n < restoreWhitelist.size(); n++) {
						string label = wstring_to_utf8(restoreWhitelist[n]);
						if (ImGui::Selectable(label.c_str(), sel_wl_item == n)) {
							sel_wl_item = n;
						}
					}
				}
				ImGui::EndListBox();
			}
		}

		if (ImGui::CollapsingHeader(L("GROUP_WE_INTEGRATION"))) {
			ImGui::Checkbox(L("ENABLE_WE_INTEGRATION"), &cfg.enableWEIntegration);

			ImGui::BeginDisabled(!cfg.enableWEIntegration);

			static char wePathBuf[CONSTANT1];
			static int last_config_idx = -1;
			if (g_appState.currentConfigIndex != last_config_idx) {
				strncpy_s(wePathBuf, wstring_to_utf8(cfg.weSnapshotPath).c_str(), sizeof(wePathBuf));
				last_config_idx = g_appState.currentConfigIndex;
			}

			if (ImGui::InputText(L("WE_SNAPSHOT_PATH_LABEL"), wePathBuf, CONSTANT1)) {
				wstring newPath = utf8_to_wstring(wePathBuf);
				if (!IsPureASCII(newPath)) {
					//MessageBoxWin(L("ERROR_TITLE"), L("ERROR_NON_ASCII_PATH"));
					strcpy_s(wePathBuf, "");
					cfg.weSnapshotPath = L"";
				}
				else {
					cfg.weSnapshotPath = newPath;
				}
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_WE_SNAPSHOT_PATH"));

			ImGui::EndDisabled();
		}

		// 云同步设置
		if (ImGui::CollapsingHeader(L("GROUP_CLOUD_SYNC")))
		{
			ImGui::Checkbox(L("ENABLE_CLOUD_SYNC"), &cfg.cloudSyncEnabled);

			ImGui::BeginDisabled(!cfg.cloudSyncEnabled);

			char rclonePathBuf[CONSTANT1];
			strncpy_s(rclonePathBuf, wstring_to_utf8(cfg.rclonePath).c_str(), sizeof(rclonePathBuf));
			ImGui::InputText(L("RCLONE_PATH_LABEL"), rclonePathBuf, CONSTANT1);
			cfg.rclonePath = utf8_to_wstring(rclonePathBuf);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RCLONE_PATH"));

			char remotePathBuf[CONSTANT1];
			strncpy_s(remotePathBuf, wstring_to_utf8(cfg.rcloneRemotePath).c_str(), sizeof(remotePathBuf));
			ImGui::InputText(L("RCLONE_REMOTE_PATH_LABEL"), remotePathBuf, CONSTANT1);
			cfg.rcloneRemotePath = utf8_to_wstring(remotePathBuf);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RCLONE_REMOTE_PATH"));

			ImGui::EndDisabled();
		}


		if (ImGui::CollapsingHeader(L("GROUP_APPEARANCE"))) {
			// 语言选择
			static int lang_idx = 0;
			for (int i = 0; i < IM_ARRAYSIZE(lang_codes); ++i) {
				if (g_CurrentLang == lang_codes[i]) {
					lang_idx = i;
					break;
				}
			}
			if (ImGui::Combo(L("LANGUAGE"), &lang_idx, langs, IM_ARRAYSIZE(langs))) {
				g_CurrentLang = lang_codes[lang_idx];
			}
			ImGui::Separator();
			ImGui::Text(L("THEME_SETTINGS"));
			if (ImGui::RadioButton(L("THEME_DARK"), &cfg.theme, 0)) { ApplyTheme(cfg.theme); } ImGui::SameLine();
			if (ImGui::RadioButton(L("THEME_LIGHT"), &cfg.theme, 1)) { ApplyTheme(cfg.theme); } ImGui::SameLine();
			if (ImGui::RadioButton(L("THEME_CLASSIC"), &cfg.theme, 2)) { ApplyTheme(cfg.theme); }

			ImGui::SliderFloat(L("UI_SCALE"), &g_uiScale, 0.75f, 2.5f, "%.2f");
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_OK"))) {
				/*ImGuiStyle& style = ImGui::GetStyle();
				style.ScaleAllSizes(cfg.uiScale);*/
				ImGuiIO& io = ImGui::GetIO(); (void)io;
				io.FontGlobalScale = g_uiScale;
			}

			ImGui::Text(L("FONT_SETTINGS"));
			char Fonts[CONSTANT1];
			strncpy_s(Fonts, wstring_to_utf8(cfg.fontPath).c_str(), sizeof(Fonts));
			if (ImGui::Button(L("BUTTON_SELECT_FONT"))) {
				wstring sel = SelectFileDialog();
				if (!sel.empty()) {
					cfg.fontPath = sel;
					Fontss = sel;
				}
			}
			ImGui::SameLine();
			if (ImGui::InputText("##fontPathValue", Fonts, CONSTANT1)) {
				cfg.fontPath = utf8_to_wstring(Fonts);
				Fontss = cfg.fontPath;
			}
		}
	}
	
	ImGui::Dummy(ImVec2(0.0f, 10.0f));
	if (ImGui::Button(L("BUTTON_SAVE_AND_CLOSE"), ImVec2(120, 0))) {
		SaveConfigs();
		showSettings = false;
	}
	ImGui::End();
}

void RunSpecialMode(int configId) {
	SpecialConfig spCfg;
	if (g_appState.specialConfigs.count(configId)) {
		spCfg = g_appState.specialConfigs[configId];
	}
	else {
		ConsoleLog(nullptr, L("SPECIAL_CONFIG_NOT_FOUND"), configId);
		Sleep(3000);
		return;
	}
#ifdef _WIN32
	// 隐藏控制台窗口（如果配置要求）
	if (spCfg.hideWindow) {
		ShowWindow(GetConsoleWindow(), SW_HIDE);
	}
#endif

	// 设置控制台标题和头部信息
	system(("title MineBackup - Automated Task: " + utf8_to_gbk(spCfg.name)).c_str());
	ConsoleLog(&console, L("AUTOMATED_TASK_RUNNER_HEADER"));
	ConsoleLog(&console, L("EXECUTING_CONFIG_NAME"), utf8_to_gbk(spCfg.name.c_str()));
	ConsoleLog(&console, "----------------------------------------------");
	if (!spCfg.hideWindow) {
		ConsoleLog(&console, L("CONSOLE_QUIT_PROMPT"));
		ConsoleLog(&console, "----------------------------------------------");
	}

	atomic<bool> shouldExit = false;
	vector<thread> taskThreads;
	static Console dummyConsole; // 用于传递给 DoBackup

	// --- 1. 执行一次性命令 ---
	for (const auto& cmd : spCfg.commands) {
		ConsoleLog(&console, L("LOG_CMD_EXECUTING"), wstring_to_utf8(cmd).c_str());
		system(utf8_to_gbk(wstring_to_utf8(cmd)).c_str()); // 使用 system 简化实现
	}

	// --- 2. 处理并启动所有自动备份任务 ---
	for (const auto& task : spCfg.tasks) {
		if (!g_appState.configs.count(task.configIndex) ||
			task.worldIndex < 0 ||
			task.worldIndex >= g_appState.configs[task.configIndex].worlds.size())
		{
			ConsoleLog(&console, L("ERROR_INVALID_WORLD_IN_TASK"), task.configIndex, task.worldIndex);
			continue;
		}

		// 创建任务专用配置（合并基础配置和特殊设置）
		Config taskConfig = g_appState.configs[task.configIndex];
		const auto& worldData = taskConfig.worlds[task.worldIndex];
		taskConfig.hotBackup = spCfg.hotBackup;
		taskConfig.zipLevel = spCfg.zipLevel;
		taskConfig.keepCount = spCfg.keepCount;
		taskConfig.cpuThreads = spCfg.cpuThreads;
		taskConfig.useLowPriority = spCfg.useLowPriority;
		//taskConfig.blacklist = spCfg.blacklist; 沿用普通配置的黑名单

		if (task.backupType == 0) { // 类型 0: 一次性备份
			ConsoleLog(&console, L("TASK_QUEUE_ONETIME_BACKUP"), utf8_to_gbk(wstring_to_utf8(worldData.first)).c_str());
			g_appState.realConfigIndex = task.configIndex;
			DoBackup(taskConfig, worldData, dummyConsole, L"SpecialMode");
			// 成功
			ConsoleLog(&console, L("TASK_SPECIAL_BACKUP_DONE"), utf8_to_gbk(wstring_to_utf8(worldData.first)).c_str());
		}
		else { // 类型 1 (间隔) 和 2 (计划) 在后台线程运行
			taskThreads.emplace_back([task, taskConfig, worldData, &shouldExit]() {
				ConsoleLog(&console, L("THREAD_STARTED_FOR_WORLD"), utf8_to_gbk(wstring_to_utf8(worldData.first)).c_str());

				while (!shouldExit) {
					// 计算下次运行时间
					time_t next_run_t = 0;
					if (task.backupType == 1) { // 间隔备份
						this_thread::sleep_for(chrono::minutes(task.intervalMinutes));
					}
					else { // 计划备份
						while (true) {
							time_t now_t = time(nullptr);
							tm local_tm;
							localtime_s(&local_tm, &now_t);

							// 设置目标时间为今天，如果已过时则调整
							tm target_tm = local_tm;
							target_tm.tm_hour = task.schedHour;
							target_tm.tm_min = task.schedMinute;
							target_tm.tm_sec = 0;

							if (task.schedDay != 0) target_tm.tm_mday = task.schedDay;
							if (task.schedMonth != 0) target_tm.tm_mon = task.schedMonth - 1;

							next_run_t = mktime(&target_tm);

							if (next_run_t <= now_t) {
								if (task.schedDay == 0) target_tm.tm_mday++;
								else if (task.schedMonth == 0) target_tm.tm_mon++;
								else target_tm.tm_year++;
								next_run_t = mktime(&target_tm);
							}

							if (next_run_t > now_t) break;
							this_thread::sleep_for(chrono::seconds(1));
						}

						char time_buf[26];
						ctime_s(time_buf, sizeof(time_buf), &next_run_t);
						time_buf[strlen(time_buf) - 1] = '\0';
						ConsoleLog(&console, L("SCHEDULE_NEXT_BACKUP_AT"), utf8_to_gbk(wstring_to_utf8(worldData.first).c_str()), time_buf);

						// 等待直到目标时间，同时检查退出信号
						while (time(nullptr) < next_run_t && !shouldExit) {
							this_thread::sleep_for(chrono::seconds(1));
						}
					}

					if (shouldExit) break;

					ConsoleLog(&console, L("BACKUP_PERFORMING_FOR_WORLD"), utf8_to_gbk(wstring_to_utf8(worldData.first).c_str()));
					g_appState.realConfigIndex = task.configIndex;
					DoBackup(taskConfig, worldData, console, L"SpecialMode");
					ConsoleLog(&console, L("TASK_SPECIAL_BACKUP_DONE"), utf8_to_gbk(wstring_to_utf8(worldData.first)).c_str());
				}
				ConsoleLog(&console, L("THREAD_STOPPED_FOR_WORLD"), utf8_to_gbk(wstring_to_utf8(worldData.first).c_str()));
				});
		}
	}

	ConsoleLog(&console, L("INFO_TASKS_INITIATED"));

	// --- 3. 用户输入主循环（如果控制台可见）---
	while (!shouldExit) {
		if (!spCfg.hideWindow && _kbhit()) {
			char c = tolower(_getch());
			if (c == 'q') {
				g_stopExitWatcher = true;
				if (g_exitWatcherThread.joinable()) {
					g_exitWatcherThread.join();
				}
				shouldExit = true;
				ConsoleLog(&console, L("INFO_QUIT_SIGNAL_RECEIVED"));
			}
			else if (c == 'm') {
				g_stopExitWatcher = true;
				if (g_exitWatcherThread.joinable()) {
					g_exitWatcherThread.join();
				}
				shouldExit = true;
				g_appState.specialConfigs[configId].autoExecute = false;
				SaveConfigs();
				ConsoleLog(&console, L("INFO_SWITCHING_TO_GUI_MODE"));
#ifdef _WIN32
				wchar_t selfPath[MAX_PATH];
				GetModuleFileNameW(NULL, selfPath, MAX_PATH); // 获得程序路径
				ShellExecuteW(NULL, L"open", selfPath, NULL, NULL, SW_SHOWNORMAL); // 开启
#endif
			}
		}

		// 如果启用自动退出且没有后台线程，则可以退出
		if (spCfg.exitAfterExecution && taskThreads.empty()) {
			shouldExit = true;
		}

		this_thread::sleep_for(chrono::milliseconds(200));
	}

	// --- 4. 清理 ---
	for (auto& t : taskThreads) {
		if (t.joinable()) {
			t.join();
		}
	}

	// 停止所有启动的任务
	{
		lock_guard<mutex> lock(g_appState.task_mutex);
		for (auto& kv : g_appState.g_active_auto_backups) {
			kv.second.stop_flag = true;
		}
	}
	for (auto& kv : g_appState.g_active_auto_backups) {
		if (kv.second.worker.joinable()) kv.second.worker.join();
	}
	g_appState.g_active_auto_backups.clear();

	ConsoleLog(&console, L("INFO_ALL_TASKS_SHUT_DOWN"));

	// 将捕获到的所有日志写入文件
	ofstream log_file("special_mode_log.txt", ios::app);
	if (log_file.is_open()) {
		time_t now = time(0);
		char time_buf[100];
		ctime_s(time_buf, sizeof(time_buf), &now);
		log_file.imbue(locale(log_file.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
		log_file << L("SPECIAL_MODE_LOG_START") << time_buf << endl;
		
		for (const char* item : console.Items) {
			log_file << gbk_to_utf8(item) << endl;
		}
		log_file << L("SPECIAL_MODE_LOG_END") << endl << endl;
		log_file.close();
	}
	else {
		ConsoleLog(nullptr, L("SPECIAL_MODE_LOG_FILE_ERROR"));
	}
	return;
}

void ShowHistoryWindow(int& tempCurrentConfigIndex) {
	// 使用static变量来持久化UI状态
	static HistoryEntry* selected_entry = nullptr;
	static ImGuiTextFilter filter;
	static char rename_buf[MAX_PATH];
	static char comment_buf[512];
	static string original_comment; // 用于支持“取消”编辑
	static bool is_comment_editing = false;
	static HistoryEntry* entry_to_delete = nullptr;
	Config& cfg = g_appState.configs[tempCurrentConfigIndex];

	ImGui::SetNextWindowSize(ImVec2(850, 600), ImGuiCond_FirstUseEver);

	if (!ImGui::Begin(L("HISTORY_WINDOW_TITLE"), &showHistoryWindow)) {
		ImGui::End();
		return;
	}

	// 当窗口关闭或配置改变时，重置选中项
	if (!showHistoryWindow || (selected_entry && g_appState.g_history.find(tempCurrentConfigIndex) == g_appState.g_history.end())) {
		selected_entry = nullptr;
		is_comment_editing = false;
	}

	// --- 顶部工具栏 ---
	filter.Draw(L("HISTORY_SEARCH_HINT"), ImGui::GetContentRegionAvail().x * 0.5f);
	ImGui::SameLine();
	if (ImGui::Button(L("HISTORY_CLEAN_INVALID"))) {
		ImGui::OpenPopup(L("HISTORY_CONFIRM_CLEAN_TITLE"));
	}

	// 清理确认弹窗
	if (ImGui::BeginPopupModal(L("HISTORY_CONFIRM_CLEAN_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(L("HISTORY_CONFIRM_CLEAN_MSG"));
		ImGui::Separator();
		if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
			if (g_appState.configs.count(tempCurrentConfigIndex) && g_appState.g_history.count(tempCurrentConfigIndex)) {
				auto& history_vec = g_appState.g_history.at(tempCurrentConfigIndex);
				history_vec.erase(
					remove_if(history_vec.begin(), history_vec.end(),
						[&](const HistoryEntry& entry) {
							return !filesystem::exists(filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / entry.worldName / entry.backupFile);
						}),
					history_vec.end()
				);
				SaveHistory();
				//selected_entry = nullptr; // 清理后重置选择
				is_comment_editing = false;
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}

	ImGui::Separator();

	// --- 主体布局：左右分栏 ---
	float list_width = ImGui::GetContentRegionAvail().x * 0.45f;
	ImGui::BeginChild("HistoryListPane", ImVec2(list_width, 0), true);

	if (g_appState.g_history.find(tempCurrentConfigIndex) == g_appState.g_history.end() || g_appState.g_history.at(tempCurrentConfigIndex).empty()) {
		ImGui::TextWrapped(L("HISTORY_EMPTY"));
	}
	else {
		auto& history_vec = g_appState.g_history.at(tempCurrentConfigIndex);

		map<wstring, vector<HistoryEntry*>> world_history_map;
		for (auto& entry : history_vec) {
			world_history_map[entry.worldName].push_back(&entry);
		}

		for (auto& pair : world_history_map) {

			// 默认展开世界
			if (!g_worldToFocusInHistory.empty() && pair.first == g_worldToFocusInHistory) {
				ImGui::SetNextItemOpen(true);
			}
			else if (!g_worldToFocusInHistory.empty()) {
				ImGui::SetNextItemOpen(false);
			}

			if (ImGui::TreeNode(wstring_to_utf8(pair.first).c_str())) {
				sort(pair.second.begin(), pair.second.end(), [](const HistoryEntry* a, const HistoryEntry* b) {
					return a->timestamp_str > b->timestamp_str;
					});

				for (HistoryEntry* entry : pair.second) {
					string entry_label_utf8 = wstring_to_utf8(entry->backupFile);
					if (!filter.PassFilter(entry_label_utf8.c_str()) && !filter.PassFilter(wstring_to_utf8(entry->comment).c_str())) {
						continue;
					}

					filesystem::path backup_path = filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / entry->worldName / entry->backupFile;
					bool file_exists = filesystem::exists(backup_path);
					bool is_small = file_exists && filesystem::file_size(backup_path) < 10240;

					// --- 自定义列表项卡片 ---
					ImGui::PushID(entry);
					if (ImGui::Selectable("##entry_selectable", selected_entry == entry, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2.5f))) {
						selected_entry = entry;
						is_comment_editing = false; // 切换选择时退出编辑模式
					}

					ImDrawList* draw_list = ImGui::GetWindowDrawList();
					ImVec2 p_min = ImGui::GetItemRectMin();
					ImVec2 p_max = ImGui::GetItemRectMax();
					if (ImGui::IsItemHovered()) {
						draw_list->AddRectFilled(p_min, p_max, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), 4.0f);
					}
					if (selected_entry == entry) {
						draw_list->AddRect(p_min, p_max, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), 4.0f, 0, 2.0f);
					}

					// 图标
					const char* icon = file_exists ? (is_small ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_FILE) : ICON_FA_GHOST;
					ImVec4 icon_color = file_exists ? (is_small ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f)) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
					ImGui::SetCursorScreenPos(ImVec2(p_min.x + 5, p_min.y + (p_max.y - p_min.y) / 2 - ImGui::GetTextLineHeight() / 2));
					ImGui::TextColored(icon_color, "%s", icon);

					// 文本内容
					ImGui::SetCursorScreenPos(ImVec2(p_min.x + 30, p_min.y + 5));
					ImGui::TextUnformatted(entry_label_utf8.c_str());
					ImGui::SetCursorScreenPos(ImVec2(p_min.x + 30, p_min.y + 5 + ImGui::GetTextLineHeightWithSpacing()));
					ImGui::TextDisabled("%s", wstring_to_utf8(entry->timestamp_str + L" | " + entry->comment).c_str());
					ImGui::SameLine();
					ImGui::SetCursorScreenPos(ImVec2(p_max.x - 25, p_min.y + (p_max.y - p_min.y) / 2 - ImGui::GetTextLineHeight() / 2));

					if (entry->isImportant) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f)); // Gold color for important
					}
					else {
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]); // Grey for not important
					}
					ImGui::Text(ICON_FA_STAR);

					ImGui::PopStyleColor();
					ImGui::PopID();
				}
				ImGui::TreePop();
			}
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// --- 右侧详情与操作面板 ---
	ImGui::BeginChild("DetailsPane", ImVec2(0, 0), true);
	if (selected_entry) {
		ImGui::SeparatorText(L("HISTORY_DETAILS_PANE_TITLE"));

		filesystem::path backup_path = filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / selected_entry->worldName / selected_entry->backupFile;
		bool file_exists = filesystem::exists(backup_path);

		// 详细信息表格
		if (ImGui::BeginTable("DetailsTable", 2, ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableNextColumn(); ImGui::TextUnformatted(L("HISTORY_LABEL_WORLD"));
			ImGui::TableNextColumn(); ImGui::Text("%s", wstring_to_utf8(selected_entry->worldName).c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(L("HISTORY_LABEL_FILENAME"));
			ImGui::TableNextColumn(); ImGui::Text("%s", wstring_to_utf8(selected_entry->backupFile).c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(L("HISTORY_LABEL_BACKUP_TIME"));
			ImGui::TableNextColumn(); ImGui::Text("%s", wstring_to_utf8(selected_entry->timestamp_str).c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(L("HISTORY_LABEL_STATUS"));
			ImGui::TableNextColumn();
			if (file_exists) {
				bool is_small = filesystem::file_size(backup_path) < 10240;
				ImGui::TextColored(is_small ? ImVec4(1.0f, 0.9f, 0.6f, 1.0f) : ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", L(is_small ? "HISTORY_STATUS_SMALL" : "HISTORY_STATUS_OK"));
			}
			else {
				ImGui::TextDisabled("%s", L("HISTORY_STATUS_MISSING"));
			}
			if (file_exists) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::TextUnformatted(L("HISTORY_LABEL_FILE_SIZE"));
				ImGui::TableNextColumn();
				char size_buf[64];
				sprintf_s(size_buf, "%.2f MB", filesystem::file_size(backup_path) / (1024.0f * 1024.0f));
				ImGui::Text("%s", size_buf);
			}
			ImGui::EndTable();
		}

		ImGui::SeparatorText(L("HISTORY_GROUP_ACTIONS"));
		if (!file_exists) ImGui::BeginDisabled();
		if (ImGui::Button(L("HISTORY_BUTTON_RESTORE"))) {
			ImGui::OpenPopup("##CONFIRM_RESTORE");
		}
		if (ImGui::BeginPopupModal("##CONFIRM_RESTORE", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			//history_restore = false;
			ImGui::SeparatorText(L("SELECTED_BACKUP_DETAILS"));
			ImGui::Text("%s: %s", L("FILENAME_LABEL"), wstring_to_utf8(selected_entry->backupFile).c_str());
			ImGui::Text("%s: %s", L("TIMESTAMP_LABEL"), wstring_to_utf8(selected_entry->timestamp_str).c_str());
			ImGui::Text("%s: %s", L("TYPE_LABEL"), wstring_to_utf8(selected_entry->backupType).c_str());
			ImGui::Text("%s: %s", L("COMMENT_LABEL"), selected_entry->comment.empty() ? L("HISTORY_NO_COMMENT") : wstring_to_utf8(selected_entry->comment).c_str());
			
			ImGui::SeparatorText(L("CHOOSE_RESTORE_METHOD_TITLE"));
			static int restore_method = 0;
			static char customRestoreBuf[CONSTANT2] = "";

			ImGui::RadioButton(L("RESTORE_METHOD_CLEAN"), &restore_method, 0);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_METHOD_CLEAN"));

			ImGui::RadioButton(L("RESTORE_METHOD_OVERWRITE"), &restore_method, 1);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_METHOD_OVERWRITE"));

			ImGui::RadioButton(L("RESTORE_METHOD_REVERSE"), &restore_method, 2);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_METHOD_REVERSE"));

			ImGui::RadioButton(L("RESTORE_METHOD_CUSTOM"), &restore_method, 3);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_METHOD_CUSTOM"));

			// 仅在选择自定义还原时显示输入框
			if (restore_method == 3) {
				ImGui::Indent();
				ImGui::SetNextItemWidth(-1);
				ImGui::InputTextWithHint("##custom_restore_input", L("CUSTOM_RESTORE_ITEMS_HINT"), customRestoreBuf, IM_ARRAYSIZE(customRestoreBuf));
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", L("CUSTOM_RESTORE_HINT"));
				}
				ImGui::Unindent();
			}
			else {
				// 确保在切换到其他模式时清空输入，避免混淆
				if (strlen(customRestoreBuf) > 0) {
					strcpy_s(customRestoreBuf, "");
				}
			}

			ImGui::Separator();

			if (ImGui::Button(L("BUTTON_CONFIRM_RESTORE"), ImVec2(120, 0))) {
				if (cfg.backupBefore) {
					DoBackup(cfg, { selected_entry->worldName, L"" }, ref(console), L"Auto");
				}
				// 传递 customRestoreBuf, 只有在 mode 3 时它才可能有内容
				thread restore_thread(DoRestore, cfg, selected_entry->worldName, selected_entry->backupFile, ref(console), restore_method, customRestoreBuf);
				restore_thread.detach();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();

			if (ImGui::Button(L("BUTTON_SELECT_CUSTOM_FILE"), ImVec2(-1, 0))) {
				wstring selectedFile = SelectFileDialog();
				if (!selectedFile.empty()) {
					thread restore_thread(DoRestore2, cfg, selected_entry->worldName, selectedFile, ref(console), restore_method);
					restore_thread.detach();
					ImGui::CloseCurrentPopup(); // Close method choice
					//entry_for_action = nullptr; // Reset selection
				}
			}

			ImGui::Dummy(ImVec2(-1, 0));
			if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(300 + ImGui::GetStyle().ItemSpacing.x, 0))) {
				ImGui::CloseCurrentPopup();
				//selected_entry = nullptr;
			}

			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("HISTORY_BUTTON_RENAME"))) {
			strncpy_s(rename_buf, wstring_to_utf8(selected_entry->backupFile).c_str(), sizeof(rename_buf));
			ImGui::OpenPopup(L("HISTORY_RENAME_POPUP_TITLE"));
		}
		ImGui::SameLine();
		if (ImGui::Button(L("HISTORY_BUTTON_OPEN_FOLDER"))) {
			wstring cmd = L"/select,\"" + backup_path.wstring() + L"\"";
			ShellExecuteW(NULL, L"open", L"explorer.exe", cmd.c_str(), NULL, SW_SHOWNORMAL);
		}
		ImGui::SameLine();
		if (ImGui::Button(selected_entry->isImportant ? L("HISTORY_UNMARK_IMPORTANT") : L("HISTORY_MARK_IMPORTANT"))) {
			selected_entry->isImportant = !selected_entry->isImportant;
		}
		// -----------
		if (!cfg.enableWEIntegration) ImGui::BeginDisabled();
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_ADD_TO_WE"))) {
			thread we_thread(AddBackupToWESnapshots, cfg, *selected_entry, ref(console));
			we_thread.detach();
		}

		if (!cfg.enableWEIntegration) ImGui::EndDisabled();
		// -----------
		if (!file_exists) ImGui::EndDisabled();


		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
		if (ImGui::Button(L("HISTORY_BUTTON_DELETE"), ImVec2(100, 0))) {
			entry_to_delete = selected_entry;
			ImGui::OpenPopup(L("HISTORY_DELETE_POPUP_TITLE"));
		}
		ImGui::PopStyleColor(2);


		// --- 重命名弹窗 ---
		if (ImGui::BeginPopupModal(L("HISTORY_RENAME_POPUP_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextUnformatted(L("HISTORY_RENAME_POPUP_MSG"));
			ImGui::InputText("##renameedit", rename_buf, sizeof(rename_buf));
			ImGui::Separator();
			if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
				filesystem::path old_path = backup_path;
				filesystem::path new_path = old_path.parent_path() / utf8_to_wstring(rename_buf);
				if (old_path != new_path && filesystem::exists(old_path)) {
					error_code ec;
					auto last_write = filesystem::last_write_time(old_path, ec);
					if (!ec) {
						filesystem::rename(old_path, new_path, ec);
						if (!ec) {
							filesystem::last_write_time(new_path, last_write, ec); // 恢复修改时间
							selected_entry->backupFile = new_path.filename().wstring();
							SaveHistory();
						}
					}
				}
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}

		// --- 删除确认弹窗 ---
		if (ImGui::BeginPopupModal(L("HISTORY_DELETE_POPUP_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextWrapped(L("HISTORY_DELETE_POPUP_MSG"), wstring_to_utf8(entry_to_delete->backupFile).c_str());
			ImGui::Separator();
			if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
				filesystem::path path_to_delete = filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / entry_to_delete->worldName / entry_to_delete->backupFile;
				DoDeleteBackup(g_appState.configs[tempCurrentConfigIndex], *entry_to_delete, tempCurrentConfigIndex, ref(console));
				is_comment_editing = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(L("CONFIRM_SAFEDELETE"), ImVec2(120, 0))) {
				if (entry_to_delete->backupType.find(L"Smart") != wstring::npos) {
					thread safe_delete_thread(DoSafeDeleteBackup, g_appState.configs[tempCurrentConfigIndex], *entry_to_delete, tempCurrentConfigIndex, ref(console));
					safe_delete_thread.detach();
				}
				else {
					MessageBoxWin("Error", "Not a [Smart] Backup");
				}
				is_comment_editing = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}

		

		ImGui::SeparatorText(L("HISTORY_GROUP_COMMENT"));
		if (is_comment_editing) {
			ImGui::InputTextMultiline("##commentedit", comment_buf, sizeof(comment_buf), ImVec2(-1, ImGui::GetContentRegionAvail().y - 40));
			if (ImGui::Button(L("HISTORY_BUTTON_SAVE_COMMENT"))) {
				selected_entry->comment = utf8_to_wstring(comment_buf);
				SaveHistory();
				is_comment_editing = false;
			}
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_CANCEL"))) {
				is_comment_editing = false;
			}
		}
		else {
			string comment_text = selected_entry->comment.empty() ? "(No comment)" : wstring_to_utf8(selected_entry->comment);
			ImGui::TextWrapped("%s", comment_text.c_str());
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", L("HISTORY_EDIT_COMMENT_TIP"));
			}
			if (ImGui::IsItemClicked()) {
				is_comment_editing = true;
				strncpy_s(comment_buf, wstring_to_utf8(selected_entry->comment).c_str(), sizeof(comment_buf));
			}
		}

	}
	else {
		ImGui::TextWrapped(L("HISTORY_SELECT_PROMPT"));
	}
	ImGui::EndChild();

	ImGui::End();
}



void GameSessionWatcherThread() {
	console.AddLog(L("LOG_START_WATCHER_START"));

	while (!g_stopExitWatcher) {
		map<pair<int, int>, wstring> currently_locked_worlds;

		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			
			for (const auto& config_pair : g_appState.configs) {
				const Config& cfg = config_pair.second;
				if (!cfg.backupOnGameStart) continue;
				for (int world_idx = 0; world_idx < cfg.worlds.size(); ++world_idx) {
					wstring levelDatPath = cfg.saveRoot + L"\\" + cfg.worlds[world_idx].first + L"\\session.lock";
					if (!filesystem::exists(levelDatPath)) { // 没有 session.lock 文件，可能是基岩版存档，需要遍历db文件夹下的所有文件看看有没有被锁定的
						wstring temp = cfg.saveRoot + L"\\" + cfg.worlds[world_idx].first + L"\\db";
						if (!filesystem::exists(temp))
							continue;
						for (const auto& entry : filesystem::directory_iterator(temp)) {
							if (IsFileLocked(entry.path())) {
								levelDatPath = entry.path();
								break;
							}
						}
					}
					if (IsFileLocked(levelDatPath)) {
						currently_locked_worlds[{config_pair.first, world_idx}] = cfg.worlds[world_idx].first;
					}
				}
			}
			
			for (const auto& sp_config_pair : g_appState.specialConfigs) {
				const SpecialConfig& sp_cfg = sp_config_pair.second;
				if (!sp_cfg.backupOnGameStart) continue;
				for (const auto& task : sp_cfg.tasks) {
					if (g_appState.configs.count(task.configIndex) && task.worldIndex < g_appState.configs[task.configIndex].worlds.size()) {
						const Config& base_cfg = g_appState.configs[task.configIndex];
						const auto& world = base_cfg.worlds[task.worldIndex];
						wstring levelDatPath = base_cfg.saveRoot + L"\\" + world.first + L"\\session.lock";
						if (!filesystem::exists(levelDatPath)) { // 没有 session.lock 文件，可能是基岩版存档，需要遍历db文件夹下的所有文件看看有没有被锁定的
							wstring temp = base_cfg.saveRoot + L"\\" + world.first + L"\\db";
							if (!filesystem::exists(temp))
								continue;
							for (const auto& entry : filesystem::directory_iterator(temp)) {
								if (IsFileLocked(entry.path())) {
									levelDatPath = entry.path();
									break;
								}
							}
						}
						if (IsFileLocked(levelDatPath)) {
							currently_locked_worlds[{task.configIndex, task.worldIndex}] = world.first;
						}
					}
				}
			}
		}

		{
			vector<pair<int, int>> worlds_to_backup;

			// 检查新启动的世界
			for (const auto& locked_pair : currently_locked_worlds) {
				if (g_activeWorlds.find(locked_pair.first) == g_activeWorlds.end()) {
					console.AddLog(L("LOG_GAME_SESSION_STARTED"), wstring_to_utf8(locked_pair.second).c_str());
					string payload = "event=game_session_start;config=" + to_string(locked_pair.first.first) + ";world=" + wstring_to_utf8(locked_pair.second);
					BroadcastEvent(payload);
					worlds_to_backup.push_back(locked_pair.first);
				}
			}

			for (const auto& active_pair : g_activeWorlds) {
				if (currently_locked_worlds.find(active_pair.first) == currently_locked_worlds.end()) {
					console.AddLog(L("LOG_GAME_SESSION_ENDED"), wstring_to_utf8(active_pair.second).c_str());
					string payload = "event=game_session_end;config=" + to_string(active_pair.first.first) + ";world=" + wstring_to_utf8(active_pair.second);
					BroadcastEvent(payload);
				}
			}

			// 更新当前活动的世界列表
			g_activeWorlds = currently_locked_worlds;

			if (!worlds_to_backup.empty() && (g_appState.configs[g_appState.currentConfigIndex].backupOnGameStart || g_appState.specialConfigs[g_appState.currentConfigIndex].backupOnGameStart)) {
				lock_guard<mutex> config_lock(g_appState.configsMutex);
				for (const auto& backup_target : worlds_to_backup) {
					int config_idx = backup_target.first;
					int world_idx = backup_target.second;
					if (g_appState.configs.count(config_idx) && world_idx < g_appState.configs[config_idx].worlds.size()) {
						Config backupConfig = g_appState.configs[config_idx];
						backupConfig.hotBackup = true; // 必须热备份
						thread backup_thread(DoBackup, backupConfig, backupConfig.worlds[world_idx], ref(console), L"OnStart");
						backup_thread.detach();
					}
				}
			}
		}

		this_thread::sleep_for(chrono::seconds(10));
	}
	console.AddLog(L("LOG_EXIT_WATCHER_STOP"));
}



// 构建当前选择（普通 / 特殊）下用于显示的世界列表
static vector<DisplayWorld> BuildDisplayWorldsForSelection() {
	lock_guard<mutex> lock(g_appState.configsMutex);
	vector<DisplayWorld> out;
	// 普通配置视图
	if (!specialSetting) {
		if (!g_appState.configs.count(g_appState.currentConfigIndex)) return out;
		const Config& src = g_appState.configs[g_appState.currentConfigIndex];
		for (int i = 0; i < (int)src.worlds.size(); ++i) {
			if (src.worlds[i].second == L"#") continue; // 隐藏标记
			DisplayWorld dw;
			dw.name = src.worlds[i].first;
			dw.desc = src.worlds[i].second;
			dw.baseConfigIndex = g_appState.currentConfigIndex;
			dw.baseWorldIndex = i;
			dw.effectiveConfig = src; // 默认使用基础配置
			out.push_back(dw);
		}
		return out;
	}

	// 特殊配置视图：把 SpecialConfig.tasks 映射为 DisplayWorld 列表
	if (!g_appState.specialConfigs.count(g_appState.currentConfigIndex)) return out;
	const SpecialConfig& sp = g_appState.specialConfigs[g_appState.currentConfigIndex];
	for (const auto& task : sp.tasks) {
		if (!g_appState.configs.count(task.configIndex)) continue;
		const Config& baseCfg = g_appState.configs[task.configIndex];
		if (task.worldIndex < 0 || task.worldIndex >= (int)baseCfg.worlds.size()) continue;

		DisplayWorld dw;
		dw.name = baseCfg.worlds[task.worldIndex].first;
		dw.desc = baseCfg.worlds[task.worldIndex].second;
		dw.baseConfigIndex = task.configIndex;
		dw.baseWorldIndex = task.worldIndex;

		// 合并配置：以 baseCfg 为主，特殊配置覆盖常用字段
		dw.effectiveConfig = baseCfg;
		dw.effectiveConfig.zipLevel = sp.zipLevel;
		if (sp.keepCount > 0) dw.effectiveConfig.keepCount = sp.keepCount;
		if (sp.cpuThreads > 0) dw.effectiveConfig.cpuThreads = sp.cpuThreads;
		dw.effectiveConfig.useLowPriority = sp.useLowPriority;
		dw.effectiveConfig.hotBackup = sp.hotBackup;
		dw.effectiveConfig.blacklist = sp.blacklist;

		out.push_back(dw);
	}

	return out;
}





void DoExportForSharing(Config tempConfig, wstring worldName, wstring worldPath, wstring outputPath, wstring description, Console& console) {
	console.AddLog(L("LOG_EXPORT_STARTED"), wstring_to_utf8(worldName).c_str());

	// 准备临时文件和路径
	filesystem::path temp_export_dir = filesystem::temp_directory_path() / L"MineBackup_Export" / worldName;
	filesystem::path readme_path = temp_export_dir / L"readme.txt";

	try {
		// 清理并创建临时工作目录
		if (filesystem::exists(temp_export_dir)) {
			filesystem::remove_all(temp_export_dir);
		}
		filesystem::create_directories(temp_export_dir);

		// 如果有描述，创建 readme.txt
		if (!description.empty()) {
			wofstream readme_file(readme_path);
			readme_file.imbue(locale(readme_file.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
			readme_file << L"[Name]\n" << worldName << L"\n\n";
			readme_file << L"[Description]\n" << description << L"\n\n";
			readme_file << L"[Exported by MineBackup]\n";
			readme_file.close();
		}

		// 收集并过滤文件
		vector<filesystem::path> files_to_export;
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (!is_blacklisted(entry.path(), worldPath, worldPath, tempConfig.blacklist)) {
				files_to_export.push_back(entry.path());
			}
		}

		// 将 readme.txt 也加入待压缩列表
		if (!description.empty()) {
			files_to_export.push_back(readme_path);
		}

		if (files_to_export.empty()) {
			console.AddLog("[Error] No files left to export after applying blacklist.");
			filesystem::remove_all(temp_export_dir);
			return;
		}

		// 创建文件列表供 7z 使用
		wstring filelist_path = (temp_export_dir / L"filelist.txt").wstring();
		wofstream ofs(filelist_path);
		ofs.imbue(locale(ofs.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
		for (const auto& file : files_to_export) {
			// 对于世界文件，写入相对路径；对于readme，写入绝对路径
			if (file.wstring().rfind(worldPath, 0) == 0) {
				ofs << filesystem::relative(file, worldPath).wstring() << endl;
			}
			else {
				ofs << file.wstring() << endl;
			}
		}
		ofs.close();

		// 构建并执行 7z 命令
		wstring command = L"\"" + tempConfig.zipPath + L"\" a -t" + tempConfig.zipFormat + L" -m0=" + tempConfig.zipMethod + L" -mx=" + to_wstring(tempConfig.zipLevel) +
			L" \"" + outputPath + L"\"" + L" @" + filelist_path;

		// 工作目录应为原始世界路径，以确保压缩包内路径正确
		if (RunCommandInBackground(command, console, tempConfig.useLowPriority, worldPath)) {
			console.AddLog(L("LOG_EXPORT_SUCCESS"), wstring_to_utf8(outputPath).c_str());
			wstring cmd = L"/select,\"" + outputPath + L"\"";
			ShellExecuteW(NULL, L"open", L"explorer.exe", cmd.c_str(), NULL, SW_SHOWNORMAL);
		}
		else {
			console.AddLog(L("LOG_EXPORT_FAILED"));
		}

	}
	catch (const exception& e) {
		console.AddLog("[Error] An exception occurred during export: %s", e.what());
	}

	// 清理临时目录
	filesystem::remove_all(temp_export_dir);
}



void TriggerHotkeyBackup() {
	console.AddLog(L("LOG_HOTKEY_BACKUP_TRIGGERED"));

	for (const auto& config_pair : g_appState.configs) {
		int config_idx = config_pair.first;
		const Config& cfg = config_pair.second;

		for (int world_idx = 0; world_idx < cfg.worlds.size(); ++world_idx) {
			const auto& world = cfg.worlds[world_idx];
			wstring levelDatPath = cfg.saveRoot + L"\\" + world.first + L"\\session.lock";
			if (!filesystem::exists(levelDatPath)) { // 没有 session.lock 文件，可能是基岩版存档，需要遍历db文件夹下的所有文件看看有没有被锁定的
				wstring temp = cfg.saveRoot + L"\\" + world.first + L"\\db";
				if (!filesystem::exists(temp))
					continue;
				for (const auto& entry : filesystem::directory_iterator(temp)) {
					if (IsFileLocked(entry.path())) {
						levelDatPath = entry.path();
						break;
					}
				}
			}

			if (IsFileLocked(levelDatPath)) {
				console.AddLog(L("LOG_ACTIVE_WORLD_FOUND"), wstring_to_utf8(world.first).c_str(), cfg.name.c_str());

				console.AddLog(L("KNOTLINK_PRE_HOT_BACKUP"), cfg.name.c_str(), wstring_to_utf8(world.first).c_str());


				Config hotkeyConfig = cfg;
				hotkeyConfig.hotBackup = true;

				thread backup_thread(DoBackup, hotkeyConfig, world, ref(console), L"Hotkey");
				backup_thread.detach();
				return;
			}
		}
	}

	console.AddLog(L("LOG_NO_ACTIVE_WORLD_FOUND"));
}

void TriggerHotkeyRestore() {

	HotRestoreState expected_idle = HotRestoreState::IDLE;
	// 使用CAS操作确保线程安全地从IDLE转换到WAITING_FOR_MOD
	if (!g_appState.hotkeyRestoreState.compare_exchange_strong(expected_idle, HotRestoreState::WAITING_FOR_MOD)) {
		console.AddLog(L("[Hotkey] A restore operation is already in progress. Ignoring request."));
		return;
	}

	g_appState.isRespond = false;
	console.AddLog(L("LOG_HOTKEY_RESTORE_TRIGGERED"));

	for (const auto& config_pair : g_appState.configs) {
		int config_idx = config_pair.first;
		const Config& cfg = config_pair.second;

		for (int world_idx = 0; world_idx < cfg.worlds.size(); ++world_idx) {
			const auto& world = cfg.worlds[world_idx];
			wstring levelDatPath = cfg.saveRoot + L"\\" + world.first + L"\\session.lock";
			if (!filesystem::exists(levelDatPath)) { // 没有 session.lock 文件，可能是基岩版存档，需要遍历db文件夹下的所有文件看看有没有被锁定的
				wstring temp = cfg.saveRoot + L"\\" + world.first + L"\\db";
				if (!filesystem::exists(temp))
					continue;
				for (const auto& entry : filesystem::directory_iterator(temp)) {
					if (IsFileLocked(entry.path())) {
						levelDatPath = entry.path();
						break;
					}
				}
			}

			if (IsFileLocked(levelDatPath)) {


				console.AddLog(L("LOG_ACTIVE_WORLD_FOUND"), wstring_to_utf8(world.first).c_str(), cfg.name.c_str());
				// KnotLink 通知
				BroadcastEvent("event=pre_hot_restore;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(world.first));
				console.AddLog(L("KNOTLINK_PRE_RESTORE"), cfg.name.c_str(), wstring_to_utf8(world.first).c_str());



				// 4. 启动后台等待线程
				thread([=]() {
					using namespace std::chrono;
					auto startTime = steady_clock::now();
					const auto timeout = seconds(15);

					// 等待响应或超时
					while (steady_clock::now() - startTime < timeout) {
						if (g_appState.isRespond) {
							break; // 收到响应
						}
						this_thread::sleep_for(milliseconds(100));
					}

					// 检查是收到了响应还是超时了
					if (!g_appState.isRespond) {
						console.AddLog(L("[Error] Mod did not respond within 15 seconds. Restore aborted."));
						BroadcastEvent("event=restore_cancelled;reason=timeout");
						g_appState.hotkeyRestoreState = HotRestoreState::IDLE; // 重置状态
						return;
					}

					// --- 收到响应，开始还原 ---
					g_appState.isRespond = false; // 重置标志位
					g_appState.hotkeyRestoreState = HotRestoreState::RESTORING;
					console.AddLog(L("[Hotkey] Mod is ready. Starting restore process."));

					// 查找最新备份文件 (这部分逻辑保持不变)
					wstring backupDir = cfg.backupPath + L"\\" + world.first;
					filesystem::path latestBackup;
					auto latest_time = filesystem::file_time_type{};
					bool found = false;

					if (filesystem::exists(backupDir)) {
						for (const auto& entry : filesystem::directory_iterator(backupDir)) {
							if (entry.is_regular_file()) {
								if (entry.last_write_time() > latest_time) {
									latest_time = entry.last_write_time();
									latestBackup = entry.path();
									found = true;
								}
							}
						}
					}

					if (!found) {
						console.AddLog(L("LOG_NO_BACKUP_FOUND"));
						BroadcastEvent("event=restore_finished;status=failure;reason=no_backup_found");
						g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
						return;
					}

					console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(latestBackup.filename().wstring()).c_str());

					DoRestore(cfg, world.first, latestBackup.filename().wstring(), ref(console), 0, "");

					// 假设成功，广播完成事件
					BroadcastEvent("event=restore_finished;status=success;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(world.first));
					console.AddLog(L("[Hotkey] Restore completed successfully."));

					// 最终，重置状态
					g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
					g_appState.isRespond = false;


				}).detach(); // 分离线程，让它在后台运行
				return;
			}
		}
	}
	g_appState.isRespond = false;
	console.AddLog(L("LOG_NO_ACTIVE_WORLD_FOUND"));
	g_appState.hotkeyRestoreState = HotRestoreState::IDLE; // 重置状态
}