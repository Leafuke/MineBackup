#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_IMPLEMENTATION
#include "Broadcast.h"
#include "imgui-all.h"
#include "i18n.h"
#include "AppState.h"
#include "Platform_win.h"
#include "Console.h"
#include "ConfigManager.h"
#include "text_to_text.h"
#include "HistoryManager.h"
#include "BackupManager.h"
#include <locale>
#include <codecvt>
#include <fcntl.h>
#include <io.h>
#include <thread>
#include <atomic> // �����̰߳�ȫ�ı�־
#include <mutex>  // ���ڻ�����
#include <shellapi.h> // ����ͼ�����
#include <conio.h>

using namespace std;

GLFWwindow* wc = nullptr;
static map<wstring, GLuint> g_worldIconTextures;
static map<wstring, ImVec2> g_worldIconDimensions;
static vector<int> worldIconWidths, worldIconHeights;
string CURRENT_VERSION = "1.9.0";
atomic<bool> g_UpdateCheckDone(false);
atomic<bool> g_NewVersionAvailable(false);
string g_LatestVersionStr;
string g_ReleaseNotes;

int last_interval = 15;


// �����������ȫ�֣�
ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
wstring Fontss;
bool showSettings = false;
bool isSilence = false;

bool specialSetting = false;
bool g_CheckForUpdates = true, g_RunOnStartup = false;
bool showHistoryWindow = false;
bool g_enableKnotLink = true;

extern NOTIFYICONDATA nid;

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


inline void ApplyTheme(int& theme);
wstring SanitizeFileName(const wstring& input);
void CheckForUpdatesThread();
void SetAutoStart(const string& appName, const wstring& appPath, bool configType, int& configId, bool& enable);
//bool LoadTextureFromFile(const char* filename, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height);
bool LoadTextureFromFileGL(const char* filename, GLuint* out_texture, int* out_width, int* out_height);
bool checkWorldName(const wstring& world, const vector<pair<wstring, wstring>>& worldList);
bool ExtractFontToTempFile(wstring& extractedPath);
bool Extract7zToTempFile(wstring& extractedPath);
void TriggerHotkeyBackup();
void TriggerHotkeyRestore();
void GameSessionWatcherThread();

bool IsFileLocked(const wstring& path);
bool is_blacklisted(const filesystem::path& file_to_check, const filesystem::path& backup_source_root, const filesystem::path& original_world_root, const vector<wstring>& blacklist);
wstring SelectFileDialog(HWND hwndOwner = NULL);
wstring SelectFolderDialog(HWND hwndOwner = NULL);
size_t CalculateFileHash(const filesystem::path& filepath);
string GetRegistryValue(const string& keyPath, const string& valueName);
wstring GetLastOpenTime(const wstring& worldPath);
wstring GetLastBackupTime(const wstring& backupDir);

void UpdateMetadataFile(const filesystem::path& metadataPath, const wstring& newBackupFile, const wstring& basedOnBackupFile, const map<wstring, size_t>& currentState);


void ShowSettingsWindow();
void ShowHistoryWindow(int& tempCurrentConfigIndex);
vector<DisplayWorld> BuildDisplayWorldsForSelection();

LRESULT WINAPI HiddenWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void LimitBackupFiles(const wstring& folderPath, int limit, Console* console = nullptr);
bool RunCommandInBackground(wstring command, Console& console, bool useLowPriority, const wstring& workingDirectory = L"");

string ProcessCommand(const string& commandStr, Console* console);
wstring CreateWorldSnapshot(const filesystem::path& worldPath, const wstring& snapshotPath, Console& console);
void DoExportForSharing(Config tempConfig, wstring worldName, wstring worldPath, wstring outputPath, wstring description, Console& console);
void RunSpecialMode(int configId);
void CheckForConfigConflicts();
void ConsoleLog(Console* console, const char* format, ...);




// Main code
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	// ���õ�ǰ����Ŀ¼Ϊ��ִ���ļ�����Ŀ¼�����⿪������Ѱ��config����
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(NULL, exePath, MAX_PATH);
	SetCurrentDirectoryW(filesystem::path(exePath).parent_path().c_str());

	//_setmode(_fileno(stdout), _O_U8TEXT);
	//_setmode(_fileno(stdin), _O_U8TEXT);
	//CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	//ImGui_ImplWin32_EnableDpiAwareness(); ����win32���еģ�����Ǩ�Ƶ�glfw����Ҫ����ʵ��

	HWND hwnd_hidden = CreateHiddenWindow(hInstance);
	CreateTrayIcon(hwnd_hidden, hInstance);

	wstring g_7zTempPath, g_FontTempPath;
	bool sevenZipExtracted = Extract7zToTempFile(g_7zTempPath);
	bool fontExtracted = ExtractFontToTempFile(g_FontTempPath);
	if (!ExtractFontToTempFile(g_FontTempPath)) {
		printf("\a");
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
		// ��ʼ���źŷ����� ���첽���б��⿨�٣�
		thread linkLoaderThread([]() {
			g_signalSender = new SignalSender("0x00000020", "0x00000020");
			// ��ʼ��������Ӧ�������� ProcessCommand ��Ϊ�ص�
			try {
				g_commandResponser = new OpenSocketResponser("0x00000020", "0x00000010");
				g_commandResponser->setQuestionHandler(
					[](const string& q) {
						// ���յ������⽻���������
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

	
	{ // ��� VC++2015-2022 ����ʱ
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
	wc = glfwCreateWindow((int)(1280 * main_scale), (int)(800 * main_scale), "MineBackup", nullptr, nullptr);
	if (wc == nullptr)
		return 1;
	glfwMakeContextCurrent(wc);
	glfwSwapInterval(1); // Enable vsync
	

	int width, height, channels;
	// Ϊ�˿�ƽ̨�����õķ�ʽ��ֱ�Ӽ���һ��png�ļ� - дcmake��ʱ�����滻��
	// unsigned char* pixels = stbi_load("path/to/icon.png", &width, &height, 0, 4); 
	HRSRC hRes = FindResource(hInstance, MAKEINTRESOURCE(IDI_ICON3), RT_GROUP_ICON);
	HGLOBAL hMem = LoadResource(hInstance, hRes);
	void* pMem = LockResource(hMem);
	int nId = LookupIconIdFromDirectoryEx((PBYTE)pMem, TRUE, 0, 0, LR_DEFAULTCOLOR);
	hRes = FindResource(hInstance, MAKEINTRESOURCE(nId), RT_ICON);
	hMem = LoadResource(hInstance, hRes);
	pMem = LockResource(hMem);

	// ���ڴ��е�ͼ�����ݼ���
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

	// ����Docking
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // ���Ͼ�ʧȥԲ���ˣ���֪����ô���
	io.ConfigViewportsNoAutoMerge = true; // ���Զ��ϲ��ӿ�

	// Բ����
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 8.0f;
	style.FrameRounding = 5.0f;
	style.GrabRounding = 5.0f;
	style.PopupRounding = 5.0f;
	style.ScrollbarRounding = 5.0f;
	style.ChildRounding = 8.0f;

	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	// ���������ȫ������
	style.ScaleAllSizes(main_scale);        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
	style.FontScaleDpi = main_scale;        // Set initial font scale. (using io.ConfigDpiScaleFonts=true makes this unnecessary. We leave both here for documentation purpose)
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
	//io.FontGlobalScale = dpi_scale;

	bool errorShow = false;
	bool isFirstRun = !filesystem::exists("config.ini");
	static bool showConfigWizard = isFirstRun;
	g_appState.showMainApp = !isFirstRun;
	if (isFirstRun)
		ImGui::StyleColorsLight();//Ĭ����ɫ

	if (g_appState.configs.count(g_appState.currentConfigIndex))
		ApplyTheme(g_appState.configs[g_appState.currentConfigIndex].theme); // ��������ط���������
	else
		ApplyTheme(g_appState.specialConfigs[g_appState.currentConfigIndex].theme);

	if (isFirstRun) {
		GetUserDefaultUILanguageWin();
		if (g_CurrentLang == "zh-CN") {
			if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttc"))
				Fontss = L"C:\\Windows\\Fonts\\msyh.ttc";
			else if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttf"))
				Fontss = L"C:\\Windows\\Fonts\\msyh.ttf";
		}
		else {
			Fontss = L"C:\\Windows\\Fonts\\SegoeUI.ttf";
		}
	}
	if (g_CurrentLang == "zh-CN")
		ImFont* font = io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
	else
		ImFont* font = io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f, nullptr, io.Fonts->GetGlyphRangesDefault());

	// ׼���ϲ�ͼ������
	ImFontConfig config2;
	config2.MergeMode = true;
	config2.PixelSnapH = true;
	config2.GlyphMinAdvanceX = 20.0f; // ͼ��Ŀ��
	// ����Ҫ��ͼ�������м��ص�ͼ�귶Χ
	static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };

	// ���ز��ϲ�
	io.Fonts->AddFontFromFileTTF(wstring_to_utf8(g_FontTempPath).c_str(), 20.0f, &config2, icon_ranges);

	// ��������ͼ��
	io.Fonts->Build();

	console.AddLog(L("CONSOLE_WELCOME"));

	if (sevenZipExtracted) {
		console.AddLog(L("LOG_7Z_EXTRACT_SUCCESS"));
	}
	else {
		console.AddLog(L("LOG_7Z_EXTRACT_FAIL"));
	}

	// ��¼ע��
	static char backupComment[CONSTANT1] = "";


	// Main loop
	while (!g_appState.done && !glfwWindowShouldClose(wc))
	{

		// ���������С������ʾ�����Եȴ�����ʱ��
		if (glfwGetWindowAttrib(wc, GLFW_ICONIFIED) || !g_appState.showMainApp) {
			// ʹ�ô���ʱ�ĵȴ�������������Ȼ���������Եش���Win32��Ϣ
			glfwWaitEventsTimeout(0.1); // �ȴ�100ms
		}
		else {
			// ������ѯ���Ա���������UI����
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
			// �״�������ʹ�õľ�̬����
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

				// ��·��
				string pathTemp;
				if (ImGui::Button(L("BUTTON_AUTO_JAVA"))) {
					if (filesystem::exists((string)getenv("APPDATA") + "\\.minecraft\\saves")) {
						pathTemp = (string)getenv("APPDATA") + "\\.minecraft\\saves";
						strncpy_s(saveRootPath, pathTemp.c_str(), sizeof(saveRootPath));
					}
				}
				ImGui::SameLine();
				if (ImGui::Button(L("BUTTON_AUTO_BEDROCK"))) { // ������ getenv���ĳ�_dupenv_s��...
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
					// ��������·����Ҫgbk
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
				// �����Ƕ��7z�Ƿ����ͷųɹ�
				if (sevenZipExtracted) {
					string extracted_path_utf8 = wstring_to_utf8(g_7zTempPath);
					strncpy_s(zipPath, extracted_path_utf8.c_str(), sizeof(zipPath));
					ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.4f, 1.0f), L("WIZARD_USING_EMBEDDED_7Z"));
				}
				else {
					// ����ͷ�ʧ�ܣ�ִ��ԭ�����Զ�����߼�
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
						// ����������һ������
						g_appState.currentConfigIndex = 1;
						Config& initialConfig = g_appState.configs[g_appState.currentConfigIndex];

						// 1. ���������ռ���·��
						initialConfig.saveRoot = utf8_to_wstring(saveRootPath);
						initialConfig.backupPath = utf8_to_wstring(backupPath);
						initialConfig.zipPath = utf8_to_wstring(zipPath);

						// 2. �Զ�ɨ��浵Ŀ¼����������б�
						if (filesystem::exists(initialConfig.saveRoot)) {
							for (auto& entry : filesystem::directory_iterator(initialConfig.saveRoot)) {
								if (entry.is_directory()) {
									// ��Ի��Ұ�����⴦���� levelname.txt ������ݵ����ļ�����
									
									if (filesystem::exists(entry.path() / "levelname.txt")) {
										ifstream levelNameFile(entry.path() / "levelname.txt");
										string levelName = "";
										getline(levelNameFile, levelName);
										levelNameFile.close();
										initialConfig.worlds.push_back({ entry.path().filename().wstring(), utf8_to_wstring(levelName) });
									}
									else {
										initialConfig.worlds.push_back({ entry.path().filename().wstring(), L"" }); // ����Ϊ�ļ�����������Ϊ��
									}
								}
							}
						}

						// 3. ���ú����Ĭ��ֵ
						initialConfig.zipFormat = L"7z";
						initialConfig.zipLevel = 5;
						initialConfig.keepCount = 0;
						initialConfig.backupMode = 1;
						initialConfig.hotBackup = false;
						initialConfig.backupBefore = false;
						initialConfig.skipIfUnchanged = true;
						isSilence = false;
						if (g_CurrentLang == "zh-CN") {
							if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttc"))
								initialConfig.fontPath = L"C:\\Windows\\Fonts\\msyh.ttc";
							else if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttf"))
								initialConfig.fontPath = L"C:\\Windows\\Fonts\\msyh.ttf";
						}
						else
							initialConfig.fontPath = L"C:\\Windows\\Fonts\\SegoeUI.ttf";

						g_appState.specialConfigs.clear();

						// 4. ���浽�ļ����л�����Ӧ�ý���
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
			// --- �����˵��� ---
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
						ShellExecuteA(NULL, "open", "https://github.com/Leafuke/MineBackup", NULL, NULL, SW_SHOWNORMAL);
					}
					if (ImGui::MenuItem(L("MENU_ISSUE"))) {
						ShellExecuteA(NULL, "open", "https://github.com/Leafuke/MineBackup/issues", NULL, NULL, SW_SHOWNORMAL);
					}
					if (ImGui::MenuItem(L("HELP_DOCUMENT"))) {
						ShellExecuteA(NULL, "open", "https://docs.qq.com/doc/DUUp4UVZOYmZWcm5M", NULL, NULL, SW_SHOWNORMAL);
					}
					if (ImGui::MenuItem(L("MENU_ABOUT"))) {
						showAboutWindow = true;
						ImGui::OpenPopup(L("MENU_ABOUT"));
					}
					ImGui::EndMenu();
				}

				// �ڲ˵����Ҳ���ʾ���°�ť
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
							ShellExecuteA(NULL, "open", ("https://github.com/Leafuke/MineBackup/releases/download/" + g_LatestVersionStr + "/MineBackup.exe").c_str(), NULL, NULL, SW_SHOWNORMAL);
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						if (ImGui::Button(L("UPDATE_POPUP_DOWNLOAD_BUTTON_2"), ImVec2(180, 0))) {
							ShellExecuteA(NULL, "open", ("https://gh-proxy.com/https://github.com/Leafuke/MineBackup/releases/download/" + g_LatestVersionStr + "/MineBackup.exe").c_str(), NULL, NULL, SW_SHOWNORMAL);
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						if (ImGui::Button(L("UPDATE_POPUP_DOWNLOAD_BUTTON_3"), ImVec2(180, 0))) {
							ShellExecuteA(NULL, "open", "https://www.123865.com/s/Zsyijv-UTuGd?pwd=mine#", NULL, NULL, SW_SHOWNORMAL);
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 0))) {
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						if (ImGui::Button(L("CHECK_FOR_UPDATES"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
							ShellExecuteA(NULL, "open", "https://github.com/Leafuke/MineBackup/releases", NULL, NULL, SW_SHOWNORMAL);
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::EndPopup();
					}
				}
				

				{
					float buttonSize = ImGui::GetFrameHeight();
					// ����ť�Ƶ��˵��������ұ�
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
					ShellExecuteA(NULL, "open", "https://github.com/Leafuke/MineBackup", NULL, NULL, SW_SHOWNORMAL);
				}
				ImGui::SameLine();
				if (ImGui::Button(L("ABOUT_VISIT_BILIBILI")))
				{
					ShellExecuteA(NULL, "open", "https://space.bilibili.com/545429962", NULL, NULL, SW_SHOWNORMAL);
				}
				if (ImGui::Button(L("ABOUT_VISIT_KNOTLINK")))
				{
					ShellExecuteA(NULL, "open", "https://github.com/hxh230802/KnotLink", NULL, NULL, SW_SHOWNORMAL);
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("ABOUT_VISIT_KNOTLINK_TIP"));

				ImGui::Dummy(ImVec2(0.0f, 10.0f));
				ImGui::SeparatorText(L("ABOUT_LICENSE_HEADER"));
				ImGui::Text("%s", L("ABOUT_LICENSE_TYPE"));
				ImGui::Text("%s", L("ABOUT_LICENSE_COPYRIGHT"));
				ImGui::Text("%s", L("ABOUT_LICENSE_TEXT"));

				ImGui::Dummy(ImVec2(0.0f, 10.0f));
				if (ImGui::Button(L("BUTTON_OK"), ImVec2(250, 0))) // ����ťһ���̶�����Ի�ø��õĹ۸�
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

			static int selectedWorldIndex = -1;       // �����û����б���ѡ�������
			static char backupComment[CONSTANT1] = "";// ����ע������������
			// ��ȡ��ǰ����
			if (!g_appState.configs.count(g_appState.currentConfigIndex)) { // �Ҳ�����˵��Ӧ�ö�Ӧ������������
				specialSetting = true;
			}

			float totalW = ImGui::GetContentRegionAvail().x;
			float leftW = totalW * 0.32f;
			float midW = totalW * 0.25f;
			float rightW = totalW * 0.42f;
			// --- ��̬��������ͼ������ͳߴ������Ĵ�С ---
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
					// ��ͨ����
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
					// ��������
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
						if ((!specialSetting && g_appState.configs.size() > 1) || (specialSetting && !g_appState.specialConfigs.empty())) { // ���ٱ���һ��
							showDeleteConfigPopup = true;
						}
					}


					ImGui::EndCombo();
				}

				// ɾ�����õ���
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
				// ��������õ���
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
								// ԭ���� g_appState.configs.rbegin()->first + 1��������̫�ã�����ͳһ��nextConfigId
								int new_index = CreateNewNormalConfig(new_config_name);
								// �̳е�ǰ���ã�����У���������·��Ϊ��
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

				// �µ��Զ��忨Ƭ
				//ImGui::BeginChild("WorldListChild", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3), true); // Ԥ���ײ���ť�ռ�
				ImGui::BeginChild("WorldListChild", ImVec2(0, 0), true);

				// selectedWorldIndex ���������ڸı��ˡ����������� displayWorlds �������������� cfg.worlds ��������


				for (int i = 0; i < worldCount; ++i) {
					const auto& dw = displayWorlds[i];
					ImGui::PushID(i);
					bool is_selected = (selectedWorldIndex == i);

					// worldFolder / backupFolder ���� effectiveConfig
					wstring worldFolder = dw.effectiveConfig.saveRoot + L"\\" + dw.name;
					wstring backupFolder = dw.effectiveConfig.backupPath + L"\\" + dw.name;

					// --- ���ͼ���� ---
					ImDrawList* draw_list = ImGui::GetWindowDrawList();

					float iconSz = ImGui::GetTextLineHeightWithSpacing() * 2.5f;
					ImVec2 icon_pos = ImGui::GetCursorScreenPos();
					ImVec2 icon_end_pos = ImVec2(icon_pos.x + iconSz, icon_pos.y + iconSz);

					// ����ռλ���ͱ߿�
					draw_list->AddRectFilled(icon_pos, icon_end_pos, IM_COL32(50, 50, 50, 200), 4.0f);
					draw_list->AddRect(icon_pos, icon_end_pos, IM_COL32(200, 200, 200, 200), 4.0f);


					string iconKey_utf8 = wstring_to_utf8(worldFolder);
					wstring iconKey = worldFolder;

					// �ټ����߼�
					if (g_worldIconTextures.find(iconKey) == g_worldIconTextures.end()) {
						// ���Ϊ���ڼ��ػ�ʧ�ܣ������ظ�����
						g_worldIconTextures[iconKey] = 0; // 0 ��ʾ��Ч����

						string iconPath = wstring_to_utf8(worldFolder + L"\\icon.png");
						string bedrockIconPath = wstring_to_utf8(worldFolder + L"\\world_icon.jpeg");

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

					// ��Ⱦ�߼�
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


					// ������ƹ�ͼ������
					ImGui::Dummy(ImVec2(iconSz, iconSz));

					ImGui::SetCursorScreenPos(icon_pos);
					ImGui::InvisibleButton("##icon_button", ImVec2(iconSz, iconSz));
					// �������ͼ��
					if (ImGui::IsItemClicked()) {
						wstring sel = SelectFileDialog();
						if (!sel.empty()) {
							// ����ԭ icon.png
							CopyFileW(sel.c_str(), (worldFolder + L"\\icon.png").c_str(), FALSE);
							// �ͷž��������¼���
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
					// --- ״̬�߼� (Ϊͼ����׼��) ---
					lock_guard<mutex> lock(g_appState.task_mutex); // ���� g_appState.g_active_auto_backups ��Ҫ����
					bool is_task_running = g_appState.g_active_auto_backups.count(make_pair(displayWorlds[i].baseConfigIndex, i)) > 0;
					// �������ʱ�����󱸷�ʱ���£�����Ϊ��Ҫ����
					//wstring worldFolder = cfg.saveRoot + L"\\" + cfg.worlds[i].first;
					bool needs_backup = GetLastOpenTime(worldFolder) > GetLastBackupTime(backupFolder);

					// ����������Ϊһ����ѡ��
					// ImGuiSelectableFlags_AllowItemOverlap ���������ڿ�ѡ��������������ؼ�
					if (ImGui::Selectable("##world_selectable", is_selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2.5f))) {
						selectedWorldIndex = i;
					}

					ImVec2 p_min = ImGui::GetItemRectMin();
					ImVec2 p_max = ImGui::GetItemRectMax();

					// --- ��Ƭ�����͸��� ---
					if (ImGui::IsItemHovered()) {
						draw_list->AddRectFilled(p_min, p_max, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
					}
					else if (is_selected) {
						draw_list->AddRectFilled(p_min, p_max, ImGui::GetColorU32(ImGuiCol_FrameBgActive, 0.5f), 4.0f);
					}

					if (is_selected) {
						draw_list->AddRect(p_min, p_max, ImGui::GetColorU32(ImGuiCol_ButtonActive), 4.0f, 0, 2.0f);
					}

					// �����ڿ�ѡ�����ͬλ�ÿ�ʼ�������ǵ��Զ�������
					ImGui::SameLine();
					ImGui::BeginGroup(); // ���������������һ��

					// --- ��һ�У������������� (�Զ�����) ---
					string name_utf8 = wstring_to_utf8(dw.name);
					string desc_utf8 = wstring_to_utf8(dw.desc);
					ImGui::TextWrapped("%s", name_utf8.c_str());

					//// --- �ڶ��У�ʱ���״̬ ---
					//wstring openTime = GetLastOpenTime(worldFolder);
					//wstring backupTime = GetLastBackupTime(backupFolder);

					//// ����Ҫ��Ϣ��ɫ��ң����߲�θ�
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
					if (desc_utf8.empty()) {
						ImGui::TextWrapped(L("CARD_WORLD_NO_DESC"));
					}
					else {
						ImGui::TextWrapped("%s", desc_utf8.c_str());
					}
					ImGui::PopStyleColor();

					ImGui::EndGroup();

					// --- �Ҳ��״̬ͼ�� ---
					float icon_pane_width = 40.0f;
					ImGui::SameLine(ImGui::GetContentRegionAvail().x - icon_pane_width);
					ImGui::BeginGroup();
					ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 0.25f)); // ��ֱ����һ��
					if (is_task_running) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 1.0f, 1.0f)); // ��ɫ
						ImGui::Text(ICON_FA_ROTATE); // ��תͼ�꣬��ʾ��������
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TOOLTIP_AUTOBACKUP_RUNNING"));
						ImGui::PopStyleColor();
					}
					else if (needs_backup) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f)); // ��ɫ
						ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION); // ����ͼ��
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TOOLTIP_NEEDS_BACKUP"));
						ImGui::PopStyleColor();
					}
					else {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.6f, 1.0f)); // ��ɫ
						ImGui::Text(ICON_FA_CIRCLE_CHECK); // �Թ�ͼ��
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TOOLTIP_UP_TO_DATE"));
						ImGui::PopStyleColor();
					}
					ImGui::EndGroup();


					ImGui::PopID();
					ImGui::Separator();
				}

				ImGui::EndChild(); // ���� WorldListChild

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

						// -- ��ϸ��Ϣ --
						wstring worldFolder = displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot + L"\\" + displayWorlds[selectedWorldIndex].name;
						wstring backupFolder = displayWorlds[selectedWorldIndex].effectiveConfig.backupPath + L"\\" + displayWorlds[selectedWorldIndex].name;
						ImGui::Text("%s: %s", L("TABLE_LAST_OPEN"), wstring_to_utf8(GetLastOpenTime(worldFolder)).c_str());
						ImGui::Text("%s: %s", L("TABLE_LAST_BACKUP"), wstring_to_utf8(GetLastBackupTime(backupFolder)).c_str());

						ImGui::Separator();

						// -- ע������� --if (ImGui::InputText(L("WORLD_DESC"), desc, CONSTANT2))
						//cfg.worlds[i].second = utf8_to_wstring(desc);
						//ImGui::InputTextMultiline(L("COMMENT_HINT"), backupComment, IM_ARRAYSIZE(backupComment), ImVec2(-1, ImGui::GetTextLineHeight() * 3));
						char buffer[CONSTANT1] = "";
						// ���Ӽ�飬ȷ�� selectedWorldIndex ��Ȼ��Ч
						if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size()) {
							const auto& dw = displayWorlds[selectedWorldIndex];
							wstring desc = dw.desc;
							strncpy_s(buffer, wstring_to_utf8(desc).c_str(), sizeof(buffer));
							ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
							ImGui::InputTextWithHint("##backup_desc", L("HINT_BACKUP_DESC"), buffer, IM_ARRAYSIZE(buffer), ImGuiInputTextFlags_EnterReturnsTrue);

							// ��д��ǰ���ٴν��������ļ��
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
							// ���������Ч����ʾһ�����õ�ռλ�����
							strcpy_s(buffer, "N/A");
							ImGui::BeginDisabled();
							ImGui::InputTextWithHint("##backup_desc", L("HINT_BACKUP_DESC"), buffer, IM_ARRAYSIZE(buffer));
							ImGui::EndDisabled();
						}

						ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
						ImGui::InputTextWithHint("##backup_comment", L("HINT_BACKUP_COMMENT"), backupComment, IM_ARRAYSIZE(backupComment), ImGuiInputTextFlags_EnterReturnsTrue);

						// -- ��Ҫ������ť --
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
							g_worldToFocusInHistory = displayWorlds[selectedWorldIndex].name; // ����Ҫ�۽�������
							showHistoryWindow = true; // ����ʷ����
						}
						if (ImGui::Button(L("BUTTON_HIDE_WORLD"), ImVec2(-1, 0))) {
							// ������С��Χ�ı��ؼ�鲢����Ҫ������ DisplayWorld��displayWorlds �Ǳ��ر�����
							if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size()) {
								DisplayWorld dw_copy = displayWorlds[selectedWorldIndex]; // ��һ��ֵ������֮��������������ȥ�� g_appState.configs

								bool did_change = false;

								// ���޸�ȫ�� g_appState.configs ǰ��������ֹ�����̲߳�����/д���±���
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
								} // ���� g_appState.configsMutex 
							}
						}

						if (ImGui::Button(L("BUTTON_PIN_WORLD"), ImVec2(-1, 0))) {
							// ��������Ƿ���Ч�Ҳ��ǵ�һ��
							if (selectedWorldIndex > 0 && selectedWorldIndex < displayWorlds.size()) {
								DisplayWorld& dw = displayWorlds[selectedWorldIndex];
								int configIdx = dw.baseConfigIndex;
								int worldIdx = dw.baseWorldIndex;

								// ȷ�����ǲ���������ͨ�����е������б�
								if (!specialSetting && g_appState.configs.count(configIdx)) {
									Config& cfg = g_appState.configs[configIdx];
									if (worldIdx < cfg.worlds.size()) {
										// �洢Ҫ�ƶ�������
										pair<wstring, wstring> worldToMove = cfg.worlds[worldIdx];

										// ��ԭλ��ɾ��
										cfg.worlds.erase(cfg.worlds.begin() + worldIdx);

										// ���뵽�б���
										cfg.worlds.insert(cfg.worlds.begin(), worldToMove);

										// ����ѡ����Ϊ�µĶ�����
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

						// ģ�鱸��
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
									if (!filesystem::exists(modsPath) && filesystem::exists(tempPath / "mods")) { // ��������ģ����ܷ���worldͬ���ļ�����
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

						// ��������
						float availWidth = ImGui::GetContentRegionAvail().x;
						float btnWidth = ImGui::CalcTextSize(L("BUTTON_BACKUP_OTHERS")).x + ImGui::GetStyle().FramePadding.x * 2;
						if (ImGui::Button(L("BUTTON_BACKUP_OTHERS"), ImVec2(btnWidth, 0))) {
							if (selectedWorldIndex != -1) {
								ImGui::OpenPopup("Others");
							}
						}
						ImGui::SameLine();
						ImGui::SetNextItemWidth((availWidth - btnWidth) * 0.97);
						// ����������Ҫ���ݵ��������ݵ�·�������� D:\Games\g_appState.configs
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
								if (g_appState.configs.count(g_appState.currentConfigIndex)) {
									thread backup_thread(DoOthersBackup, g_appState.configs[g_appState.currentConfigIndex], buf, utf8_to_wstring(others_comment));
									backup_thread.detach();
									strcpy_s(others_comment, "");
								}
								SaveConfigs(); // ����һ��·��
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
							// ��ͬ���߼�
							const Config& config = g_appState.configs[displayWorlds[selectedWorldIndex].baseConfigIndex];
							if (!config.rclonePath.empty() && !config.rcloneRemotePath.empty() && filesystem::exists(config.rclonePath)) {
								console.AddLog(L("CLOUD_SYNC_START"));
								wstring rclone_command = L"\"" + config.rclonePath + L"\" copy \"" + config.backupPath + L"\\" + displayWorlds[selectedWorldIndex].name + L"\" \"" + config.rcloneRemotePath + L"\" --progress";
								// ����һ���߳���ִ����ͬ��������������������
								thread([rclone_command, config]() {
									RunCommandInBackground(rclone_command, console, config.useLowPriority);
									console.AddLog(L("CLOUD_SYNC_FINISH"));
									}).detach();
							}
							else {
								console.AddLog(L("CLOUD_SYNC_INVALID"));
							}
						}

						// ��������
						if (ImGui::Button(L("BUTTON_EXPORT_FOR_SHARING"), ImVec2(-1, 0))) {
							if (selectedWorldIndex != -1) {
								ImGui::OpenPopup(L("EXPORT_WINDOW_TITLE"));
							}
						}
						if (ImGui::BeginPopupModal(L("EXPORT_WINDOW_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
							// ʹ�� static ����������һ�������ã�����ֻ�ڵ����״δ�ʱ����ʼ��
							static Config tempExportConfig;
							static char outputPathBuf[MAX_PATH];
							static char descBuf[CONSTANT2];
							static char blacklistAddItemBuf[CONSTANT1];
							static int selectedBlacklistItem = -1;
							static int selectedFormat = 0;

							// �����״δ�ʱ�����г�ʼ��
							if (ImGui::IsWindowAppearing()) {
								const auto& dw = displayWorlds[selectedWorldIndex];
								tempExportConfig = dw.effectiveConfig; // ���Ƶ�ǰ������Ϊ����

								// ��������Ĭ�����·��ΪMineBackup��ǰλ��
								wchar_t currentPath[MAX_PATH];
								GetCurrentDirectoryW(MAX_PATH, currentPath);
								wstring cleanWorldName = SanitizeFileName(dw.name);
								wstring finalPath = wstring(currentPath) + L"\\" + cleanWorldName + L"_shared." + tempExportConfig.zipFormat;
								strncpy_s(outputPathBuf, wstring_to_utf8(finalPath).c_str(), sizeof(outputPathBuf));

								// Ԥ��Ĭ�Ϻ�����
								tempExportConfig.blacklist.clear();
								tempExportConfig.blacklist.push_back(L"playerdata");
								tempExportConfig.blacklist.push_back(L"stats");
								tempExportConfig.blacklist.push_back(L"advancements");
								tempExportConfig.blacklist.push_back(L"session.lock");
								tempExportConfig.blacklist.push_back(L"level.dat_old");


								// ����ϴε�����
								memset(descBuf, 0, sizeof(descBuf));
								memset(blacklistAddItemBuf, 0, sizeof(blacklistAddItemBuf));
								selectedBlacklistItem = -1;
							}

							// ���ȡ����ѡ "�������ݰ�"����̬���/�Ƴ� datapacks
							/*bool datapacksInBlacklist = find(tempBlacklist.begin(), tempBlacklist.end(), L"datapacks") != tempBlacklist.end();
							if (includeDatapacks && datapacksInBlacklist) {
								tempBlacklist.erase(remove(tempBlacklist.begin(), tempBlacklist.end(), L"datapacks"), tempBlacklist.end());
							}
							else if (!includeDatapacks && !datapacksInBlacklist) {
								tempBlacklist.push_back(L"datapacks");
							}*/

							// --- UI ��Ⱦ ---
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

					// �Զ����ݵ���
					if (ImGui::BeginPopupModal(L("AUTOBACKUP_SETTINGS"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
						bool is_task_running = false;
						pair<int, int> taskKey = { -1,-1 };
						vector<DisplayWorld> localDisplayWorlds; // ����ʾʹ��
						{
							lock_guard<mutex> lock(g_appState.task_mutex);
							if (selectedWorldIndex >= 0) {
								// ���ʹ�� displayWorlds��
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
									// 1. ����ֹͣ��־
									g_appState.g_active_auto_backups.at(taskKey).stop_flag = true;
									// 2. �ȴ��߳̽���
									if (g_appState.g_active_auto_backups.at(taskKey).worker.joinable())
										g_appState.g_active_auto_backups.at(taskKey).worker.join();
									// 3. �ӹ��������Ƴ�
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
								// ע�Ტ�����߳�
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

	// ����
	BroadcastEvent("event=app_shutdown");
	lock_guard<mutex> lock(g_appState.task_mutex);
	for (auto& pair : g_appState.g_active_auto_backups) {
		pair.second.stop_flag = true; // ֪ͨ�߳�ֹͣ
		if (pair.second.worker.joinable()) {
			pair.second.worker.join(); // �ȴ��߳�ִ�����
		}
	}
	for (auto const& [key, val] : g_worldIconTextures) {
		if (val > 0) {
			glDeleteTextures(1, &val);
		}
	}

	SaveConfigs();

	RemoveTrayIcon();
	UnregisterHotkeys(hwnd_hidden);
	DestroyWindow(hwnd_hidden);

	g_worldIconTextures.clear();
	worldIconWidths.clear();
	worldIconHeights.clear();
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(wc);
	glfwTerminate();

	/*CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);*/
	
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
	// ���ļ�����ͼ������
	int image_width = 0;
	int image_height = 0;
	unsigned char* image_data = stbi_load(filename, &image_width, &image_height, NULL, 4);
	if (image_data == NULL)
		return false;

	// ����һ�� OpenGL ����
	GLuint image_texture;
	glGenTextures(1, &image_texture);
	glBindTexture(GL_TEXTURE_2D, image_texture);

	// �����������
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // �����ԵαӰ
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// �ϴ���������
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_width, image_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
	stbi_image_free(image_data); // �ͷ�CPU�ڴ�

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
		// ��ͨ����
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
		// ��������
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
			if ((!specialSetting && g_appState.configs.size() > 1) || (specialSetting && !g_appState.specialConfigs.empty())) { // ���ٱ���һ��
				showDeleteConfigPopup = true;
			}
		}
		ImGui::EndCombo();
	}

	// ɾ�����õ���
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
	// ��������õ���
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
					// �̳е�ǰ���ã�����У���������·��Ϊ��
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
			//	// ��� blacklist �Ƿ�Ϊ��
			//	if (spCfg.blacklist.empty()) {
			//		ImGui::Text(L("No items in blacklist")); // ��ʾ���б���ʾ
			//	}
			//	else {
			//		// ������ʾ��������
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
			static int sel_task_item = -1; // ׷�ٱ�ɾ����item
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
							task.worldIndex = val.worlds.empty() ? -1 : 0; // ��������idx
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

				if (task.backupType == 1) { // ���
					ImGui::InputInt(L("INTERVAL_MINUTES"), &task.intervalMinutes);
					if (task.intervalMinutes < 1) task.intervalMinutes = 1;
				}
				else if (task.backupType == 2) { // �ƻ�
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
			// ������ñ�ɾ��
			if (g_appState.configs.empty()) g_appState.configs[1] = Config(); // ���1��ɾ���½�
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

		// ������Ϊ
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
			ImGui::SameLine();
			ImGui::Checkbox(L("SHOW_PROGRESS"), &isSilence);
			// �����ȼ�
			ImGui::Checkbox(L("USE_LOW_PRIORITY"), &cfg.useLowPriority);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(L("TIP_LOW_PRIORITY"));
			}
			ImGui::SameLine();
			ImGui::Checkbox(L("SKIP_IF_UNCHANGED"), &cfg.skipIfUnchanged);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(L("TIP_SKIP_IF_UNCHANGED"));
			}
			// CPU �߳�
			int max_threads = thread::hardware_concurrency();
			ImGui::SliderInt(L("CPU_THREAD_COUNT"), &cfg.cpuThreads, 0, max_threads);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(L("TIP_CPU_THREADS"));
			}
			ImGui::SliderInt(L("COMPRESSION_LEVEL"), &cfg.zipLevel, 0, 9);
			ImGui::InputInt(L("BACKUPS_TO_KEEP"), &cfg.keepCount);
			ImGui::InputInt(L("MAX_SMART_BACKUPS"), &cfg.maxSmartBackupsPerFull, 1, 5);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_MAX_SMART_BACKUPS"));
			ImGui::SeparatorText(L("BLACKLIST_HEADER"));
			if (ImGui::Button(L("BUTTON_ADD_FILE_BLACKLIST"))) {
				wstring sel = SelectFileDialog();
				if (!sel.empty()) cfg.blacklist.push_back(sel); // �� spCfg.blacklist
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

			// ���������ʽ����ĵ���
			if (ImGui::BeginPopupModal("Add Regex Rule", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
				static char regex_buf[256] = "regex:";
				ImGui::InputText("Regex Pattern", regex_buf, IM_ARRAYSIZE(regex_buf));
				ImGui::Separator();
				if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
					if (strlen(regex_buf) > 6) { // ȷ�� "regex:" ����������
						// ���ݵ�ǰ����ͨ���û����������ã���ӵ���Ӧ�ĺ�����
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
				// ��� blacklist �Ƿ�Ϊ��
				if (cfg.blacklist.empty()) {
					ImGui::Text(L("No items in blacklist")); // ��ʾ���б���ʾ
				}
				else {
					// ������ʾ��������
					for (int n = 0; n < cfg.blacklist.size(); n++) {
						string label = wstring_to_utf8(cfg.blacklist[n]);
						if (ImGui::Selectable(label.c_str(), sel_bl_item == n)) {
							sel_bl_item = n;
						}
						if (ImGui::IsItemHovered()) {
							// ��������ͣ��������һ��Tooltip��ʾ��������
							ImGui::SetTooltip("%s", label.c_str());
						}
					}
				}
				ImGui::EndListBox();
			}
		}

		// ��ԭ��Ϊ
		if (ImGui::CollapsingHeader(L("GROUP_RESTORE_BEHAVIOR"))) {
			ImGui::Checkbox(L("BACKUP_BEFORE_RESTORE"), &cfg.backupBefore);

			ImGui::SeparatorText(L("RESTORE_WHITELIST_HEADER"));
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_WHITELIST"));

			// �������������
			static char whitelist_add_buf[256] = "";
			ImGui::InputTextWithHint("##whitelist_add", "file_or_folder_name", whitelist_add_buf, IM_ARRAYSIZE(whitelist_add_buf));
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_ADD_WHITELIST")) && strlen(whitelist_add_buf) > 0) {
				restoreWhitelist.push_back(utf8_to_wstring(whitelist_add_buf));
				strcpy_s(whitelist_add_buf, "");
			}

			// �������б�
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

		// ��ͬ������
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
			// ����ѡ��
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



//�ں�̨��Ĭִ��һ�������г�����7z.exe�������ȴ�����ɡ�
//����ʵ�ֱ��ݺͻ�ԭ���ܵĺ��ģ�������GUI���ٺͺڴ��ڵ�����
// ����:
//   - command: Ҫִ�е����������У����ַ�����
//   - console: ���̨��������ã����������־��Ϣ��
bool RunCommandInBackground(wstring command, Console& console, bool useLowPriority, const wstring& workingDirectory) {
	// CreateProcessW��Ҫһ����д��C-style�ַ������������ǽ�wstring���Ƶ�vector<wchar_t>
	vector<wchar_t> cmd_line(command.begin(), command.end());
	cmd_line.push_back(L'\0'); // ����ַ���������

	STARTUPINFOW si = {};
	PROCESS_INFORMATION pi = {};
	si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE; // �����ӽ��̵Ĵ���

	DWORD creationFlags = CREATE_NO_WINDOW;
	if (useLowPriority) {
		creationFlags |= BELOW_NORMAL_PRIORITY_CLASS;
	}

	// ��ʼ��������
	const wchar_t* pWorkingDir = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
	console.AddLog(L("LOG_EXEC_CMD"), wstring_to_utf8(command).c_str());

	if (!CreateProcessW(NULL, cmd_line.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, pWorkingDir, &si, &pi)) {
		console.AddLog(L("LOG_ERROR_CREATE_PROCESS"), GetLastError());
		return false;
	}

	// �ȴ��ӽ���ִ�����
	WaitForSingleObject(pi.hProcess, INFINITE);

	// ����ӽ��̵��˳�����
	DWORD exit_code;
	if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
		if (exit_code == 0) {
			console.AddLog(L("LOG_SUCCESS_CMD"));
		}
		else {
			console.AddLog(L("LOG_ERROR_CMD_FAILED"), exit_code);
			if (exit_code == 1) // ����
				console.AddLog(L("LOG_ERROR_CMD_FAILED_HOTBACKUP_SUGGESTION"));
			if (exit_code == 2) // ��������
				console.AddLog(L("LOG_7Z_ERROR_SUGGESTION"));
		}
	}
	else {
		console.AddLog(L("LOG_ERROR_GET_EXIT_CODE"));
	}

	// ������
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return true;
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

	// ���ؿ���̨���ڣ��������Ҫ��
	if (spCfg.hideWindow) {
		ShowWindow(GetConsoleWindow(), SW_HIDE);
	}

	// ���ÿ���̨�����ͷ����Ϣ
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
	static Console dummyConsole; // ���ڴ��ݸ� DoBackup

	// --- 1. ִ��һ�������� ---
	for (const auto& cmd : spCfg.commands) {
		ConsoleLog(&console, L("LOG_CMD_EXECUTING"), wstring_to_utf8(cmd).c_str());
		system(utf8_to_gbk(wstring_to_utf8(cmd)).c_str()); // ʹ�� system ��ʵ��
	}

	// --- 2. �������������Զ��������� ---
	for (const auto& task : spCfg.tasks) {
		if (!g_appState.configs.count(task.configIndex) ||
			task.worldIndex < 0 ||
			task.worldIndex >= g_appState.configs[task.configIndex].worlds.size())
		{
			ConsoleLog(&console, L("ERROR_INVALID_WORLD_IN_TASK"), task.configIndex, task.worldIndex);
			continue;
		}

		// ��������ר�����ã��ϲ��������ú��������ã�
		Config taskConfig = g_appState.configs[task.configIndex];
		const auto& worldData = taskConfig.worlds[task.worldIndex];
		taskConfig.hotBackup = spCfg.hotBackup;
		taskConfig.zipLevel = spCfg.zipLevel;
		taskConfig.keepCount = spCfg.keepCount;
		taskConfig.cpuThreads = spCfg.cpuThreads;
		taskConfig.useLowPriority = spCfg.useLowPriority;
		//taskConfig.blacklist = spCfg.blacklist; ������ͨ���õĺ�����

		if (task.backupType == 0) { // ���� 0: һ���Ա���
			ConsoleLog(&console, L("TASK_QUEUE_ONETIME_BACKUP"), utf8_to_gbk(wstring_to_utf8(worldData.first)).c_str());
			g_appState.realConfigIndex = task.configIndex;
			DoBackup(taskConfig, worldData, dummyConsole, L"SpecialMode");
			// �ɹ�
			ConsoleLog(&console, L("TASK_SPECIAL_BACKUP_DONE"), utf8_to_gbk(wstring_to_utf8(worldData.first)).c_str());
		}
		else { // ���� 1 (���) �� 2 (�ƻ�) �ں�̨�߳�����
			taskThreads.emplace_back([task, taskConfig, worldData, &shouldExit]() {
				ConsoleLog(&console, L("THREAD_STARTED_FOR_WORLD"), utf8_to_gbk(wstring_to_utf8(worldData.first)).c_str());

				while (!shouldExit) {
					// �����´�����ʱ��
					time_t next_run_t = 0;
					if (task.backupType == 1) { // �������
						this_thread::sleep_for(chrono::minutes(task.intervalMinutes));
					}
					else { // �ƻ�����
						while (true) {
							time_t now_t = time(nullptr);
							tm local_tm;
							localtime_s(&local_tm, &now_t);

							// ����Ŀ��ʱ��Ϊ���죬����ѹ�ʱ�����
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

						// �ȴ�ֱ��Ŀ��ʱ�䣬ͬʱ����˳��ź�
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

	// --- 3. �û�������ѭ�����������̨�ɼ���---
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
				wchar_t selfPath[MAX_PATH];
				GetModuleFileNameW(NULL, selfPath, MAX_PATH); // ��ó���·��
				ShellExecuteW(NULL, L"open", selfPath, NULL, NULL, SW_SHOWNORMAL); // ����
			}
		}

		// ��������Զ��˳���û�к�̨�̣߳�������˳�
		if (spCfg.exitAfterExecution && taskThreads.empty()) {
			shouldExit = true;
		}

		this_thread::sleep_for(chrono::milliseconds(200));
	}

	// --- 4. ���� ---
	for (auto& t : taskThreads) {
		if (t.joinable()) {
			t.join();
		}
	}

	// ֹͣ��������������
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

	// �����񵽵�������־д���ļ�
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
	// ʹ��static�������־û�UI״̬
	static HistoryEntry* selected_entry = nullptr;
	static ImGuiTextFilter filter;
	static char rename_buf[MAX_PATH];
	static char comment_buf[512];
	static string original_comment; // ����֧�֡�ȡ�����༭
	static bool is_comment_editing = false;
	static HistoryEntry* entry_to_delete = nullptr;
	Config& cfg = g_appState.configs[tempCurrentConfigIndex];

	ImGui::SetNextWindowSize(ImVec2(850, 600), ImGuiCond_FirstUseEver);

	if (!ImGui::Begin(L("HISTORY_WINDOW_TITLE"), &showHistoryWindow)) {
		ImGui::End();
		return;
	}

	// �����ڹرջ����øı�ʱ������ѡ����
	if (!showHistoryWindow || (selected_entry && g_appState.g_history.find(tempCurrentConfigIndex) == g_appState.g_history.end())) {
		selected_entry = nullptr;
		is_comment_editing = false;
	}

	// --- ���������� ---
	filter.Draw(L("HISTORY_SEARCH_HINT"), ImGui::GetContentRegionAvail().x * 0.5f);
	ImGui::SameLine();
	if (ImGui::Button(L("HISTORY_CLEAN_INVALID"))) {
		ImGui::OpenPopup(L("HISTORY_CONFIRM_CLEAN_TITLE"));
	}

	// ����ȷ�ϵ���
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
				//selected_entry = nullptr; // ���������ѡ��
				is_comment_editing = false;
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}

	ImGui::Separator();

	// --- ���岼�֣����ҷ��� ---
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

			// Ĭ��չ������
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

					// --- �Զ����б��Ƭ ---
					ImGui::PushID(entry);
					if (ImGui::Selectable("##entry_selectable", selected_entry == entry, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2.5f))) {
						selected_entry = entry;
						is_comment_editing = false; // �л�ѡ��ʱ�˳��༭ģʽ
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

					// ͼ��
					const char* icon = file_exists ? (is_small ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_FILE) : ICON_FA_GHOST;
					ImVec4 icon_color = file_exists ? (is_small ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f)) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
					ImGui::SetCursorScreenPos(ImVec2(p_min.x + 5, p_min.y + (p_max.y - p_min.y) / 2 - ImGui::GetTextLineHeight() / 2));
					ImGui::TextColored(icon_color, "%s", icon);

					// �ı�����
					ImGui::SetCursorScreenPos(ImVec2(p_min.x + 30, p_min.y + 5));
					ImGui::TextUnformatted(entry_label_utf8.c_str());
					ImGui::SetCursorScreenPos(ImVec2(p_min.x + 30, p_min.y + 5 + ImGui::GetTextLineHeightWithSpacing()));
					ImGui::TextDisabled("%s", wstring_to_utf8(entry->timestamp_str + L" | " + entry->comment).c_str());

					ImGui::PopID();
				}
				ImGui::TreePop();
			}
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// --- �Ҳ������������� ---
	ImGui::BeginChild("DetailsPane", ImVec2(0, 0), true);
	if (selected_entry) {
		ImGui::SeparatorText(L("HISTORY_DETAILS_PANE_TITLE"));

		filesystem::path backup_path = filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / selected_entry->worldName / selected_entry->backupFile;
		bool file_exists = filesystem::exists(backup_path);

		// ��ϸ��Ϣ���
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

			// ����ѡ���Զ��廹ԭʱ��ʾ�����
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
				// ȷ�����л�������ģʽʱ������룬�������
				if (strlen(customRestoreBuf) > 0) {
					strcpy_s(customRestoreBuf, "");
				}
			}

			ImGui::Separator();

			if (ImGui::Button(L("BUTTON_CONFIRM_RESTORE"), ImVec2(120, 0))) {
				if (cfg.backupBefore) {
					DoBackup(cfg, { selected_entry->worldName, L"" }, ref(console), L"Auto");
				}
				// ���� customRestoreBuf, ֻ���� mode 3 ʱ���ſ���������
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
		if (!file_exists) ImGui::EndDisabled();

		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
		if (ImGui::Button(L("HISTORY_BUTTON_DELETE"), ImVec2(100, 0))) {
			entry_to_delete = selected_entry;
			ImGui::OpenPopup(L("HISTORY_DELETE_POPUP_TITLE"));
		}
		ImGui::PopStyleColor(2);


		// --- ���������� ---
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
							filesystem::last_write_time(new_path, last_write, ec); // �ָ��޸�ʱ��
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

		// --- ɾ��ȷ�ϵ��� ---
		if (ImGui::BeginPopupModal(L("HISTORY_DELETE_POPUP_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextWrapped(L("HISTORY_DELETE_POPUP_MSG"), wstring_to_utf8(entry_to_delete->backupFile).c_str());
			ImGui::Separator();
			if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
				filesystem::path path_to_delete = filesystem::path(g_appState.configs[tempCurrentConfigIndex].backupPath) / entry_to_delete->worldName / entry_to_delete->backupFile;
				if (filesystem::exists(path_to_delete)) {
					filesystem::remove(path_to_delete);
				}
				RemoveHistoryEntry(tempCurrentConfigIndex, entry_to_delete->backupFile);
				//selected_entry = nullptr; �����
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
	console.AddLog(L("LOG_EXIT_WATCHER_START"));

	while (!g_stopExitWatcher) {
		map<pair<int, int>, wstring> currently_locked_worlds;

		{
			lock_guard<mutex> lock(g_appState.configsMutex);
			
			for (const auto& config_pair : g_appState.configs) {
				const Config& cfg = config_pair.second;
				if (!cfg.backupOnGameStart) continue;
				for (int world_idx = 0; world_idx < cfg.worlds.size(); ++world_idx) {
					wstring levelDatPath = cfg.saveRoot + L"\\" + cfg.worlds[world_idx].first + L"\\session.lock";
					if (!filesystem::exists(levelDatPath)) { // û�� session.lock �ļ��������ǻ��Ұ�浵����Ҫ����db�ļ����µ������ļ�������û�б�������
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
						if (!filesystem::exists(levelDatPath)) { // û�� session.lock �ļ��������ǻ��Ұ�浵����Ҫ����db�ļ����µ������ļ�������û�б�������
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
			// ����ѹرյ�����
			vector<pair<int, int>> worlds_to_backup;

			// ���������������
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

			// ���µ�ǰ��������б�
			g_activeWorlds = currently_locked_worlds;

			// ���ڸչرյ����磬��������
			if (!worlds_to_backup.empty()) {
				lock_guard<mutex> config_lock(g_appState.configsMutex);
				for (const auto& backup_target : worlds_to_backup) {
					int config_idx = backup_target.first;
					int world_idx = backup_target.second;
					if (g_appState.configs.count(config_idx) && world_idx < g_appState.configs[config_idx].worlds.size()) {
						Config backupConfig = g_appState.configs[config_idx];
						backupConfig.hotBackup = true; // �����ȱ���
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

void DoDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, Console& console) {
	console.AddLog(L("LOG_PRE_TO_DELETE"), wstring_to_utf8(entryToDelete.backupFile).c_str());

	filesystem::path backupDir = config.backupPath + L"\\" + entryToDelete.worldName;
	vector<filesystem::path> filesToDelete;
	filesToDelete.push_back(backupDir / entryToDelete.backupFile);

	// ִ��ɾ������
	for (const auto& path : filesToDelete) {
		try {
			if (filesystem::exists(path)) {
				filesystem::remove(path);
				console.AddLog("  - %s OK", wstring_to_utf8(path.filename().wstring()).c_str());
				// ����ʷ��¼���Ƴ���Ӧ��Ŀ
				RemoveHistoryEntry(g_appState.currentConfigIndex, path.filename().wstring());
			}
			else {
				console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(entryToDelete.backupFile).c_str());
				RemoveHistoryEntry(g_appState.currentConfigIndex, path.filename().wstring());
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog(L("LOG_ERROR_DELETE_BACKUP"), wstring_to_utf8(path.filename().wstring()).c_str(), e.what());
		}
	}
	SaveHistory(); // ������ʷ��¼�ĸ���
}

// ������ǰѡ����ͨ / ���⣩��������ʾ�������б�
static vector<DisplayWorld> BuildDisplayWorldsForSelection() {
	lock_guard<mutex> lock(g_appState.configsMutex);
	vector<DisplayWorld> out;
	// ��ͨ������ͼ
	if (!specialSetting) {
		if (!g_appState.configs.count(g_appState.currentConfigIndex)) return out;
		const Config& src = g_appState.configs[g_appState.currentConfigIndex];
		for (int i = 0; i < (int)src.worlds.size(); ++i) {
			if (src.worlds[i].second == L"#") continue; // ���ر��
			DisplayWorld dw;
			dw.name = src.worlds[i].first;
			dw.desc = src.worlds[i].second;
			dw.baseConfigIndex = g_appState.currentConfigIndex;
			dw.baseWorldIndex = i;
			dw.effectiveConfig = src; // Ĭ��ʹ�û�������
			out.push_back(dw);
		}
		return out;
	}

	// ����������ͼ���� SpecialConfig.tasks ӳ��Ϊ DisplayWorld �б�
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

		// �ϲ����ã��� baseCfg Ϊ�����������ø��ǳ����ֶ�
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

	// ׼����ʱ�ļ���·��
	filesystem::path temp_export_dir = filesystem::temp_directory_path() / L"MineBackup_Export" / worldName;
	filesystem::path readme_path = temp_export_dir / L"readme.txt";

	try {
		// ����������ʱ����Ŀ¼
		if (filesystem::exists(temp_export_dir)) {
			filesystem::remove_all(temp_export_dir);
		}
		filesystem::create_directories(temp_export_dir);

		// ��������������� readme.txt
		if (!description.empty()) {
			wofstream readme_file(readme_path);
			readme_file.imbue(locale(readme_file.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
			readme_file << L"[Name]\n" << worldName << L"\n\n";
			readme_file << L"[Description]\n" << description << L"\n\n";
			readme_file << L"[Exported by MineBackup]\n";
			readme_file.close();
		}

		// �ռ��������ļ�
		vector<filesystem::path> files_to_export;
		for (const auto& entry : filesystem::recursive_directory_iterator(worldPath)) {
			if (!is_blacklisted(entry.path(), worldPath, worldPath, tempConfig.blacklist)) {
				files_to_export.push_back(entry.path());
			}
		}

		// �� readme.txt Ҳ�����ѹ���б�
		if (!description.empty()) {
			files_to_export.push_back(readme_path);
		}

		if (files_to_export.empty()) {
			console.AddLog("[Error] No files left to export after applying blacklist.");
			filesystem::remove_all(temp_export_dir);
			return;
		}

		// �����ļ��б� 7z ʹ��
		wstring filelist_path = (temp_export_dir / L"filelist.txt").wstring();
		wofstream ofs(filelist_path);
		ofs.imbue(locale(ofs.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
		for (const auto& file : files_to_export) {
			// ���������ļ���д�����·��������readme��д�����·��
			if (file.wstring().rfind(worldPath, 0) == 0) {
				ofs << filesystem::relative(file, worldPath).wstring() << endl;
			}
			else {
				ofs << file.wstring() << endl;
			}
		}
		ofs.close();

		// ������ִ�� 7z ����
		wstring command = L"\"" + tempConfig.zipPath + L"\" a -t" + tempConfig.zipFormat + L" -mx=" + to_wstring(tempConfig.zipLevel) +
			L" \"" + outputPath + L"\"" + L" @" + filelist_path;

		// ����Ŀ¼ӦΪԭʼ����·������ȷ��ѹ������·����ȷ
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

	// ������ʱĿ¼
	filesystem::remove_all(temp_export_dir);
}

void DoSafeDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, int configIndex, Console& console) {
	console.AddLog(L("LOG_SAFE_DELETE_START"), wstring_to_utf8(entryToDelete.backupFile).c_str());

	if (entryToDelete.isImportant) {
		console.AddLog(L("LOG_SAFE_DELETE_ABORT_IMPORTANT"), wstring_to_utf8(entryToDelete.backupFile).c_str());
		return;
	}

	filesystem::path backupDir = config.backupPath + L"\\" + entryToDelete.worldName;
	filesystem::path pathToDelete = backupDir / entryToDelete.backupFile;
	const HistoryEntry* nextEntryRaw = nullptr;

	// Create a sorted list of history entries for this world to reliably find the next one
	vector<const HistoryEntry*> worldHistory;
	for (const auto& entry : g_appState.g_history[configIndex]) {
		if (entry.worldName == entryToDelete.worldName) {
			worldHistory.push_back(&entry);
		}
	}
	sort(worldHistory.begin(), worldHistory.end(), [](const auto* a, const auto* b) {
		return a->timestamp_str < b->timestamp_str;
		});

	for (size_t i = 0; i < worldHistory.size(); ++i) {
		if (worldHistory[i]->backupFile == entryToDelete.backupFile) {
			if (i + 1 < worldHistory.size()) {
				nextEntryRaw = worldHistory[i + 1];
			}
			break;
		}
	}

	if (!nextEntryRaw || nextEntryRaw->backupType == L"Full") {
		console.AddLog(L("LOG_SAFE_DELETE_END_OF_CHAIN"));
		DoDeleteBackup(config, entryToDelete, console);
		return;
	}

	if (nextEntryRaw->isImportant) {
		console.AddLog(L("LOG_SAFE_DELETE_ABORT_IMPORTANT_TARGET"), wstring_to_utf8(nextEntryRaw->backupFile).c_str());
		return;
	}

	const HistoryEntry nextEntry = *nextEntryRaw;
	filesystem::path pathToMergeInto = backupDir / nextEntry.backupFile;
	console.AddLog(L("LOG_SAFE_DELETE_MERGE_INFO"), wstring_to_utf8(entryToDelete.backupFile).c_str(), wstring_to_utf8(nextEntry.backupFile).c_str());

	filesystem::path tempExtractDir = filesystem::temp_directory_path() / L"MineBackup_Merge";

	try {
		filesystem::remove_all(tempExtractDir);
		filesystem::create_directories(tempExtractDir);

		console.AddLog(L("LOG_SAFE_DELETE_STEP_1"));
		wstring cmdExtract = L"\"" + config.zipPath + L"\" x \"" + pathToDelete.wstring() + L"\" -o\"" + tempExtractDir.wstring() + L"\" -y";
		if (!RunCommandInBackground(cmdExtract, console, config.useLowPriority)) {
			throw runtime_error("Failed to extract source archive.");
		}

		console.AddLog(L("LOG_SAFE_DELETE_STEP_2"));
		auto original_mod_time = filesystem::last_write_time(pathToMergeInto);

		wstring cmdMerge = L"\"" + config.zipPath + L"\" a \"" + pathToMergeInto.wstring() + L"\" .\\*";
		if (!RunCommandInBackground(cmdMerge, console, config.useLowPriority, tempExtractDir.wstring())) {
			filesystem::last_write_time(pathToMergeInto, original_mod_time);
			throw runtime_error("Failed to merge files into the target archive.");
		}
		filesystem::last_write_time(pathToMergeInto, original_mod_time);

		filesystem::path finalArchivePath = pathToMergeInto;
		wstring finalBackupType = nextEntry.backupType;
		wstring finalBackupFile = nextEntry.backupFile;

		if (entryToDelete.backupType == L"Full") {
			console.AddLog(L("LOG_SAFE_DELETE_STEP_3"));
			finalBackupType = L"Full";
			wstring newFilename = nextEntry.backupFile;
			size_t pos = newFilename.find(L"[Smart]");
			if (pos != wstring::npos) {
				newFilename.replace(pos, 7, L"[Full]");
				finalBackupFile = newFilename;
				filesystem::path newPath = backupDir / newFilename;
				filesystem::rename(pathToMergeInto, newPath);
				finalArchivePath = newPath;
				console.AddLog(L("LOG_SAFE_DELETE_RENAMED"), wstring_to_utf8(newFilename).c_str());
			}
		}
		else {
			console.AddLog(L("LOG_SAFE_DELETE_STEP_3_SKIP"));
		}

		console.AddLog(L("LOG_SAFE_DELETE_STEP_4"));
		filesystem::remove(pathToDelete);
		RemoveHistoryEntry(configIndex, entryToDelete.backupFile);

		for (auto& entry : g_appState.g_history[configIndex]) {
			if (entry.worldName == nextEntry.worldName && entry.backupFile == nextEntry.backupFile) {
				entry.backupFile = finalBackupFile;
				entry.backupType = finalBackupType;
				break;
			}
		}

		SaveHistory();
		filesystem::remove_all(tempExtractDir);
		console.AddLog(L("LOG_SAFE_DELETE_SUCCESS"));

	}
	catch (const exception& e) {
		console.AddLog(L("LOG_SAFE_DELETE_FATAL_ERROR"), e.what());
		filesystem::remove_all(tempExtractDir);
	}
}

void TriggerHotkeyBackup() {
	console.AddLog(L("LOG_HOTKEY_BACKUP_TRIGGERED"));

	for (const auto& config_pair : g_appState.configs) {
		int config_idx = config_pair.first;
		const Config& cfg = config_pair.second;

		for (int world_idx = 0; world_idx < cfg.worlds.size(); ++world_idx) {
			const auto& world = cfg.worlds[world_idx];
			wstring levelDatPath = cfg.saveRoot + L"\\" + world.first + L"\\session.lock";
			if (!filesystem::exists(levelDatPath)) { // û�� session.lock �ļ��������ǻ��Ұ�浵����Ҫ����db�ļ����µ������ļ�������û�б�������
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
	g_appState.isRespond = false;
	console.AddLog(L("LOG_HOTKEY_RESTORE_TRIGGERED"));

	for (const auto& config_pair : g_appState.configs) {
		int config_idx = config_pair.first;
		const Config& cfg = config_pair.second;

		for (int world_idx = 0; world_idx < cfg.worlds.size(); ++world_idx) {
			const auto& world = cfg.worlds[world_idx];
			wstring levelDatPath = cfg.saveRoot + L"\\" + world.first + L"\\session.lock";
			if (!filesystem::exists(levelDatPath)) { // û�� session.lock �ļ��������ǻ��Ұ�浵����Ҫ����db�ļ����µ������ļ�������û�б�������
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
				// KnotLink ֪ͨ
				BroadcastEvent("event=pre_hot_restore;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(world.first));
				console.AddLog(L("KNOTLINK_PRE_RESTORE"), cfg.name.c_str(), wstring_to_utf8(world.first).c_str());

				// �ȴ�ģ�鱣��
				this_thread::sleep_for(chrono::seconds(5));
				if (!g_appState.isRespond) {
					return;
				}

				// �������±����ļ�
				wstring backupDir = cfg.backupPath + L"\\" + world.first;
				filesystem::path latestBackup;
				auto latest_time = filesystem::file_time_type{};
				for (const auto& entry : filesystem::directory_iterator(backupDir)) {
					if (entry.is_regular_file()) {
						auto fname = entry.path().filename().wstring();
						if ((fname.find(L"[Full]") != wstring::npos || fname.find(L"[Smart]") != wstring::npos)
							&& entry.last_write_time() > latest_time) {
							latest_time = entry.last_write_time();
							latestBackup = entry.path();
						}
					}
				}

				if (latestBackup.empty()) {
					console.AddLog(L("LOG_NO_BACKUP_FOUND"));
					return;
				}

				console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(latestBackup.filename().wstring()).c_str());

				// ��ԭ��Ĭ������ԭ restoreMethod=0��
				thread restore_thread(DoRestore, cfg, world.first, latestBackup.filename().wstring(), ref(console), 0, "");
				restore_thread.detach();

				// KnotLink ֪ͨ��ԭ���
				BroadcastEvent("event=hot_restore_completed;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(world.first));
				console.AddLog(L("KNOTLINK_HOT_RESTORE_COMPLETED"), cfg.name.c_str(), wstring_to_utf8(world.first).c_str());
				g_appState.isRespond = false;
				return;
			}
		}
	}
	g_appState.isRespond = false;
	console.AddLog(L("LOG_NO_ACTIVE_WORLD_FOUND"));
}