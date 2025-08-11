#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_IMPLEMENTATION
#include "KnotLink/SignalSender.hpp"
#include "KnotLink/OpenSocketResponser.hpp"
#include "imgui-all.h"
#include "i18n.h"
#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <locale>
#include <codecvt>
#include <fcntl.h>
#include <io.h>
#include <thread>
#include <atomic> // 用于线程安全的标志
#include <mutex>  // 用于互斥锁
#include <conio.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#define CONSTANT1 256
#define CONSTANT2 512
#define MINEBACKUP_HOTKEY_ID 1
using namespace std;
// Data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static vector<ID3D11ShaderResourceView*> worldIconTextures;
static vector<int> worldIconWidths, worldIconHeights;
static atomic<bool> g_UpdateCheckDone(false);
static atomic<bool> g_NewVersionAvailable(false);
static string g_LatestVersionStr;
static string g_ReleaseNotes;
const string CURRENT_VERSION = "1.7.2";


// 结构体们
struct Config {
	int backupMode;
	wstring saveRoot;
	vector<pair<wstring, wstring>> worlds; // {name, desc}
	wstring backupPath;
	wstring zipPath;
	wstring zipFormat;
	wstring zipFonts;
	int zipLevel;
	int keepCount;
	bool hotBackup;
	bool backupBefore;
	bool manualRestore;
	int theme = 1;
	int folderNameType = 0;
	wstring themeColor;
	wstring backgroundImagePath;
	string name;
	bool backgroundImageEnabled = false;
	float backgroundImageAlpha = 0.5f;
	int cpuThreads = 0; // 0 for auto/default
	bool useLowPriority = false;
	bool skipIfUnchanged = true;
	int maxSmartBackupsPerFull = 5;
	bool backupOnGameExit = false;
	vector<wstring> blacklist;
};
struct AutomatedTask {
	int configIndex = -1;
	int worldIndex = -1;
	int backupType = 0; // 0: 单次, 1: 间隔, 2: 计划
	int intervalMinutes = 15;
	int schedMonth = 0, schedDay = 0, schedHour = 0, schedMinute = 0; // 0 意味着“每一”
};
struct SpecialConfig {
	bool autoExecute = false;
	vector<wstring> commands;
	vector<AutomatedTask> tasks; // REPLACED: a more capable task structure
	bool exitAfterExecution = false;
	string name;
	int zipLevel = 5;
	int keepCount = 0;
	int cpuThreads = 0;
	bool useLowPriority = true;
	bool hotBackup = false;
	vector<wstring> blacklist;
	bool runOnStartup = false;
	bool hideWindow = false;
	bool backupOnGameExit = false;
};
struct HistoryEntry {
	wstring timestamp_str;
	wstring worldName;
	wstring backupFile;
	wstring backupType;
	wstring comment;
};
struct AutoBackupTask {
	thread worker;
	atomic<bool> stop_flag{ false }; // 原子布尔值，用于安全地通知线程停止
};

// KnotLink 实例指针
SignalSender* g_signalSender = nullptr;
OpenSocketResponser* g_commandResponser = nullptr;

static mutex g_configsMutex;			// 用于保护全局配置的互斥锁
static mutex consoleMutex;				// 控制台模式的锁
static mutex g_task_mutex;		// 专门用于保护 g_active_auto_backups
static mutex specialConfigMutex;
static mutex g_activeWorldsMutex;
// 设置项变量（全局）
ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
wstring Fontss;
vector<wstring> savePaths = { L"" };
wchar_t backupPath[CONSTANT1] = L"", zipPath[CONSTANT1] = L"";
int compressLevel = 5;
bool showSettings = false;
bool isSilence = false;
bool specialSetting = false;
bool g_CheckForUpdates = true;
static bool showHistoryWindow = false;
static bool specialConfigMode = false; // 用来开启简单UI
static bool g_enableKnotLink = true;
int currentConfigIndex = 1;
map<int, Config> configs;
map<int, vector<HistoryEntry>> g_history;
map<int, SpecialConfig> specialConfigs;
static atomic<bool> specialTasksRunning = false;
static atomic<bool> specialTasksComplete = false;
static map<int, AutoBackupTask> g_active_auto_backups; // key: worldIndex, value: task
static thread g_exitWatcherThread;
static atomic<bool> g_stopExitWatcher(false);
static map<pair<int, int>, wstring> g_activeWorlds; // Key: {configIdx, worldIdx}, Value: worldName

// 声明辅助函数
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

string wstring_to_utf8(const wstring& wstr);
wstring utf8_to_wstring(const string& str);
string gbk_to_utf8(const string& gbk);
string utf8_to_gbk(const string& utf8);

void SaveHistory();
void LoadHistory();
void AddHistoryEntry(int configIndex, const wstring& worldName, const wstring& backupFile, const wstring& backupType, const wstring& comment);

void BroadcastEvent(const string& eventPayload); // KnotLink 广播
const char* L(const char* key);
inline void ApplyTheme(int& theme);
string find_json_value(const string& json, const string& key);
wstring SanitizeFileName(const wstring& input);
void CheckForUpdatesThread();
void SetAutoStart(const string& appName, const wstring& appPath, int configId, bool enable);
bool LoadTextureFromFile(const char* filename, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height);
bool checkWorldName(const wstring& world, const vector<pair<wstring, wstring>>& worldList);
bool ExtractFontToTempFile(wstring& extractedPath);
bool Extract7zToTempFile(wstring& extractedPath);
void TriggerHotkeyBackup();
void ExitWatcherThreadFunction();

bool IsFileLocked(const wstring& path);
wstring SelectFileDialog(HWND hwndOwner = NULL);
wstring SelectFolderDialog(HWND hwndOwner = NULL);
vector<filesystem::path> GetChangedFiles(const filesystem::path& worldPath, const filesystem::path& metadataPath);
size_t CalculateFileHash(const filesystem::path& filepath);
string GetRegistryValue(const string& keyPath, const string& valueName);
wstring GetLastOpenTime(const wstring& worldPath);
wstring GetLastBackupTime(const wstring& backupDir);

void SaveStateFile(const filesystem::path& metadataPath);
static void LoadConfigs(const string& filename = "config.ini");
static void SaveConfigs(const wstring& filename = L"config.ini");

void ShowSettingsWindow();
void ShowHistoryWindow();

struct Console
{
	char                  InputBuf[CONSTANT1];
	ImVector<char*>       Items;
	ImVector<const char*> Commands;
	ImVector<char*>       History;
	int                   HistoryPos;    // -1: new line, 0..History.Size-1 browsing history.
	ImGuiTextFilter       Filter;
	bool                  AutoScroll;
	bool                  ScrollToBottom;
	mutex            logMutex; //新增：用于保护日志内容的互斥锁 

	Console()
	{
		ClearLog();
		memset(InputBuf, 0, sizeof(InputBuf));
		HistoryPos = -1;

		// "CLASSIFY" is here to provide the test case where "C"+[tab] completes to "CL" and display multiple matches.
		Commands.push_back("HELP");
		Commands.push_back("HISTORY");
		Commands.push_back("CLEAR");
		Commands.push_back("BACKUP");
		Commands.push_back("BACKUP_MODS");
		Commands.push_back("RESTORE");
		Commands.push_back("SET_CONFIG");
		Commands.push_back("GET_CONFIG");
		Commands.push_back("LIST_WORLDS");
		Commands.push_back("LIST_CONFIGS");
		AutoScroll = true;                  //自动滚动好呀
		ScrollToBottom = false;             //不用滚动条，但可以鼠标滚
	}
	~Console()
	{
		ClearLog();
		for (int i = 0; i < History.Size; i++)
			ImGui::MemFree(History[i]);
	}

	// Portable helpers
	static int   Stricmp(const char* s1, const char* s2) { int d; while ((d = toupper(*s2) - toupper(*s1)) == 0 && *s1) { s1++; s2++; } return d; }
	static int   Strnicmp(const char* s1, const char* s2, int n) { int d = 0; while (n > 0 && (d = toupper(*s2) - toupper(*s1)) == 0 && *s1) { s1++; s2++; n--; } return d; }
	static char* Strdup(const char* s) { IM_ASSERT(s); size_t len = strlen(s) + 1; void* buf = ImGui::MemAlloc(len); IM_ASSERT(buf); return (char*)memcpy(buf, (const void*)s, len); }
	static void  Strtrim(char* s) { char* str_end = s + strlen(s); while (str_end > s && str_end[-1] == ' ') str_end--; *str_end = 0; }

	void    ClearLog()
	{
		lock_guard<mutex> lock(logMutex);//加锁
		for (int i = 0; i < Items.Size; i++)
			ImGui::MemFree(Items[i]);
		Items.clear();
	}

	//显示消息
	void    AddLog(const char* fmt, ...) IM_FMTARGS(2)
	{
		if (isSilence) return;
		lock_guard<mutex> lock(logMutex);
		// FIXME-OPT
		char buf[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, IM_ARRAYSIZE(buf), fmt, args);
		buf[IM_ARRAYSIZE(buf) - 1] = 0;
		va_end(args);
		Items.push_back(Strdup(buf));
	}

	void    Draw(const char* title, bool* p_open)
	{
		ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin(title, p_open))
		{
			ImGui::End();
			return;
		}

		// As a specific feature guaranteed by the library, after calling Begin() the last Item represent the title bar.
		// So e.g. IsItemHovered() will return true when hovering the title bar.
		// Here we create a context menu only available from the title bar.(暂时无用
		/*if (ImGui::BeginPopupContextItem())
		{
			if (ImGui::MenuItem(u8"关闭"))
				*p_open = false;
			ImGui::EndPopup();
		}*/

		ImGui::TextWrapped(L("CONSOLE_HELP_PROMPT1"));
		ImGui::TextWrapped(L("CONSOLE_HELP_PROMPT2"));

		if (ImGui::SmallButton(L("BUTTON_CLEAR"))) { ClearLog(); }
		ImGui::SameLine();
		bool copy_to_clipboard = ImGui::SmallButton(L("BUTTON_COPY"));
		ImGui::Separator();

		if (ImGui::BeginPopup(L("BUTTON_OPTIONS")))
		{
			ImGui::Checkbox(L("CONSOLE_AUTO_SCROLL"), &AutoScroll);
			ImGui::EndPopup();
		}

		ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_Tooltip);
		if (ImGui::Button(L("BUTTON_OPTIONS")))
			ImGui::OpenPopup(L("BUTTON_OPTIONS"));
		ImGui::SameLine();
		Filter.Draw(L("CONSOLE_FILTER_HINT"), 180);
		ImGui::Separator();

		// Reserve enough left-over height for 1 separator + 1 input text
		const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
		if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_HorizontalScrollbar))
		{
			/*if (ImGui::BeginPopupContextWindow())
			{
				if (ImGui::Selectable("清空")) ClearLog();
				ImGui::EndPopup();
			}*/

			// Display every line as a separate entry so we can change their color or add custom widgets.
			// If you only want raw text you can use ImGui::TextUnformatted(log.begin(), log.end());
			// NB- if you have thousands of entries this approach may be too inefficient and may require user-side clipping
			// to only process visible items. The clipper will automatically measure the height of your first item and then
			// "seek" to display only items in the visible area.
			// To use the clipper we can replace your standard loop:
			//      for (int i = 0; i < Items.Size; i++)
			//   With:
			//      ImGuiListClipper clipper;
			//      clipper.Begin(Items.Size);
			//      while (clipper.Step())
			//         for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
			// - That your items are evenly spaced (same height)
			// - That you have cheap random access to your elements (you can access them given their index,
			//   without processing all the ones before)
			// You cannot this code as-is if a filter is active because it breaks the 'cheap random-access' property.
			// We would need random-access on the post-filtered list.
			// A typical application wanting coarse clipping and filtering may want to pre-compute an array of indices
			// or offsets of items that passed the filtering test, recomputing this array when user changes the filter,
			// and appending newly elements as they are inserted. This is left as a task to the user until we can manage
			// to improve this example code!
			// If your items are of variable height:
			// - Split them into same height items would be simpler and facilitate random-seeking into your list.
			// - Consider using manual call to IsRectVisible() and skipping extraneous decoration from your items.
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighten spacing
			if (copy_to_clipboard)
				ImGui::LogToClipboard();
			for (const char* item : Items)
			{
				if (!Filter.PassFilter(item))
					continue;

				// Normally you would store more information in your item than just a string.
				// (e.g. make Items[] an array of structure, store color/type etc.)
				ImVec4 color;
				bool has_color = false;
				if (strstr(item, "[Error]")) { color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); has_color = true; }
				else if (strncmp(item, "# ", 2) == 0 || strncmp(item, "[Info] ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
				else if (strncmp(item, u8"[提示] ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
				if (has_color)
					ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::TextUnformatted(item);
				if (has_color)
					ImGui::PopStyleColor();
			}
			if (copy_to_clipboard)
				ImGui::LogFinish();

			// Keep up at the bottom of the scroll region if we were already at the bottom at the beginning of the frame.
			// Using a scrollbar or mouse-wheel will take away from the bottom edge.
			if (ScrollToBottom || (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
				ImGui::SetScrollHereY(1.0f);
			ScrollToBottom = false;

			ImGui::PopStyleVar();
		}
		ImGui::EndChild();
		ImGui::Separator();

		// Command-line
		bool reclaim_focus = false;
		ImGuiInputTextFlags input_text_flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
		if (ImGui::InputText(L("CONSOLE_INPUT_LABEL"), InputBuf, IM_ARRAYSIZE(InputBuf), input_text_flags, &TextEditCallbackStub, (void*)this))
		{
			char* s = InputBuf;
			Strtrim(s);
			if (s[0])
				ExecCommand(s);
			strcpy_s(s, strlen(s) + 1, "");//被要求从strcpy改成strcpy_s，这样中间要加个长度参数才不报错……
			reclaim_focus = true;
		}

		// Auto-focus on window apparition
		ImGui::SetItemDefaultFocus();
		if (reclaim_focus)
			ImGui::SetKeyboardFocusHere(-1); // Auto focus previous widget

		ImGui::End();
	}


	void DrawEmbedded()
	{
		// NOTE: this code is the inner body of the original Console::Draw,
		//       adapted to run *inside* an existing ImGui window/child.
		//       It intentionally DOES NOT call ImGui::SetNextWindowSize/ImGui::Begin/ImGui::End.

		ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);

		
		ImGui::TextWrapped(L("CONSOLE_HELP_PROMPT1"));
		ImGui::TextWrapped(L("CONSOLE_HELP_PROMPT2"));

		if (ImGui::SmallButton(L("BUTTON_CLEAR"))) { ClearLog(); }
		ImGui::SameLine();
		bool copy_to_clipboard = ImGui::SmallButton(L("BUTTON_COPY"));
		ImGui::Separator();

		if (ImGui::BeginPopup(L("BUTTON_OPTIONS")))
		{
			ImGui::Checkbox(L("CONSOLE_AUTO_SCROLL"), &AutoScroll);
			ImGui::EndPopup();
		}

		ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_Tooltip);
		if (ImGui::Button(L("BUTTON_OPTIONS")))
			ImGui::OpenPopup(L("BUTTON_OPTIONS"));
		ImGui::SameLine();
		Filter.Draw(L("CONSOLE_FILTER_HINT"), 180);
		ImGui::Separator();

		// Reserve enough left-over height for 1 separator + 1 input text
		const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
		if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_HorizontalScrollbar))
		{
			
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighten spacing
			if (copy_to_clipboard)
				ImGui::LogToClipboard();
			for (const char* item : Items)
			{
				if (!Filter.PassFilter(item))
					continue;

				// Normally you would store more information in your item than just a string.
				// (e.g. make Items[] an array of structure, store color/type etc.)
				ImVec4 color;
				bool has_color = false;
				if (strstr(item, "[Error]")) { color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); has_color = true; }
				else if (strncmp(item, "# ", 2) == 0 || strncmp(item, "[Info] ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
				else if (strncmp(item, u8"[提示] ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
				if (has_color)
					ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::TextUnformatted(item);
				if (has_color)
					ImGui::PopStyleColor();
			}
			if (copy_to_clipboard)
				ImGui::LogFinish();

			// Keep up at the bottom of the scroll region if we were already at the bottom at the beginning of the frame.
			// Using a scrollbar or mouse-wheel will take away from the bottom edge.
			if (ScrollToBottom || (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
				ImGui::SetScrollHereY(1.0f);
			ScrollToBottom = false;

			ImGui::PopStyleVar();
		}
		ImGui::EndChild();
		ImGui::Separator();

		// Command-line
		bool reclaim_focus = false;
		ImGuiInputTextFlags input_text_flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
		if (ImGui::InputText(L("CONSOLE_INPUT_LABEL"), InputBuf, IM_ARRAYSIZE(InputBuf), input_text_flags, &TextEditCallbackStub, (void*)this))
		{
			char* s = InputBuf;
			Strtrim(s);
			if (s[0])
				ExecCommand(s);
			strcpy_s(s, strlen(s) + 1, "");//被要求从strcpy改成strcpy_s，这样中间要加个长度参数才不报错……
			reclaim_focus = true;
		}

		// Auto-focus on window apparition
		ImGui::SetItemDefaultFocus();
		if (reclaim_focus)
			ImGui::SetKeyboardFocusHere(-1); // Auto focus previous widget
	}

	void  ExecCommand(const char* command_line);

	// In C++11 you'd be better off using lambdas for this sort of forwarding callbacks
	static int TextEditCallbackStub(ImGuiInputTextCallbackData* data)
	{
		Console* console = (Console*)data->UserData;
		return console->TextEditCallback(data);
	}

	int     TextEditCallback(ImGuiInputTextCallbackData* data)
	{
		switch (data->EventFlag)
		{
		case ImGuiInputTextFlags_CallbackCompletion:
		{
			const char* word_end = data->Buf + data->CursorPos;
			const char* word_start = word_end;
			while (word_start > data->Buf)
			{
				const char c = word_start[-1];
				if (c == ' ' || c == '\t' || c == ',' || c == ';')
					break;
				word_start--;
			}

			// Build a list of candidates
			ImVector<const char*> candidates;
			for (int i = 0; i < Commands.Size; i++)
				if (Strnicmp(Commands[i], word_start, (int)(word_end - word_start)) == 0)
					candidates.push_back(Commands[i]);

			if (candidates.Size == 0)
			{
				// No match
				AddLog(L("CONSOLE_CMD_MATCH_NONE"), (int)(word_end - word_start), word_start);
			}
			else if (candidates.Size == 1)
			{
				// Single match. Delete the beginning of the word and replace it entirely so we've got nice casing.
				data->DeleteChars((int)(word_start - data->Buf), (int)(word_end - word_start));
				data->InsertChars(data->CursorPos, candidates[0]);
				data->InsertChars(data->CursorPos, " ");
			}
			else
			{
				// 按Tab自动筛选
				int match_len = (int)(word_end - word_start);
				for (;;)
				{
					int c = 0;
					bool all_candidates_matches = true;
					for (int i = 0; i < candidates.Size && all_candidates_matches; i++)
						if (i == 0)
							c = toupper(candidates[i][match_len]);
						else if (c == 0 || c != toupper(candidates[i][match_len]))
							all_candidates_matches = false;
					if (!all_candidates_matches)
						break;
					match_len++;
				}

				if (match_len > 0)
				{
					data->DeleteChars((int)(word_start - data->Buf), (int)(word_end - word_start));
					data->InsertChars(data->CursorPos, candidates[0], candidates[0] + match_len);
				}

				// List matches
				AddLog(L("CONSOLE_CMD_MATCH_MULTIPLE"));
				for (int i = 0; i < candidates.Size; i++)
					AddLog("- %s\n", candidates[i]);
			}

			break;
		}
		case ImGuiInputTextFlags_CallbackHistory:
		{
			// Example of HISTORY
			const int prev_history_pos = HistoryPos;
			if (data->EventKey == ImGuiKey_UpArrow)
			{
				if (HistoryPos == -1)
					HistoryPos = History.Size - 1;
				else if (HistoryPos > 0)
					HistoryPos--;
			}
			else if (data->EventKey == ImGuiKey_DownArrow)
			{
				if (HistoryPos != -1)
					if (++HistoryPos >= History.Size)
						HistoryPos = -1;
			}

			// A better implementation would preserve the data on the current input line along with cursor position.
			if (prev_history_pos != HistoryPos)
			{
				const char* history_str = (HistoryPos >= 0) ? History[HistoryPos] : "";
				data->DeleteChars(0, data->BufTextLen);
				data->InsertChars(0, history_str);
			}
		}
		}
		return 0;
	}
}console;
void LimitBackupFiles(const wstring& folderPath, int limit, Console* console = nullptr);
bool RunCommandInBackground(wstring command, Console& console, bool useLowPriority, const wstring& workingDirectory = L"");

string ProcessCommand(const string& commandStr, Console* console);
wstring CreateWorldSnapshot(const filesystem::path& worldPath, Console& console);
void DoBackup(const Config config, const pair<wstring, wstring> world, Console& console, const wstring& comment = L"");
void DoRestore2(const Config config, const wstring& worldName, const filesystem::path& fullBackupPath, Console& console, int restoreMethod);
void DoRestore(const Config config, const wstring& worldName, const wstring& backupFile, Console& console, int restoreMethod); 
void DoModsBackup(const Config config, const wstring& comment);
void AutoBackupThreadFunction(int worldIdx, int configIdx, int intervalMinutes, Console* console);
void RunSpecialMode(int configId);
void CheckForConfigConflicts();
void ConsoleLog(const char* format, ...);

void  Console::ExecCommand(const char* command_line)
{
	AddLog("# %s\n", command_line);

	HistoryPos = -1;
	for (int i = History.Size - 1; i >= 0; i--)
		if (Stricmp(History[i], command_line) == 0)
		{
			ImGui::MemFree(History[i]);
			History.erase(History.begin() + i);
			break;
		}
	History.push_back(Strdup(command_line));

	// Process command
	if (Stricmp(command_line, "CLEAR") == 0)
	{
		ClearLog();
	}
	else if (Stricmp(command_line, "HELP") == 0)
	{
		AddLog("Commands:");
		for (int i = 0; i < Commands.Size; i++)
			AddLog("- %s", Commands[i]);
	}
	else if (Stricmp(command_line, "HISTORY") == 0)
	{
		int first = History.Size - 10;
		for (int i = first > 0 ? first : 0; i < History.Size; i++)
			AddLog("%3d: %s\n", i, History[i]);
	}
	else
	{
		string result = ProcessCommand(command_line, &console);
		AddLog("-> %s", result.c_str());
	}

	// On command input, we scroll to bottom even if AutoScroll==false
	ScrollToBottom = true;
}


// Main code
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	//_setmode(_fileno(stdout), _O_U8TEXT);
	//_setmode(_fileno(stdin), _O_U8TEXT);
	//CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	ImGui_ImplWin32_EnableDpiAwareness();

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
	g_exitWatcherThread = thread(ExitWatcherThreadFunction);
	BroadcastEvent("event=app_startup;version=" + CURRENT_VERSION);


	if (g_enableKnotLink) {
		// 初始化信号发送器
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
		
	}

	if (specialConfigMode)
	{
		bool hide = false;
		if (specialConfigs.count(currentConfigIndex)) {
			hide = specialConfigs[currentConfigIndex].hideWindow;
		}

		if (!hide) {
			AllocConsole(); // Create a console window
			// Redirect standard I/O to the new console
			FILE* pCout, * pCerr, * pCin;
			freopen_s(&pCout, "CONOUT$", "w", stdout);
			freopen_s(&pCerr, "CONOUT$", "w", stderr);
			freopen_s(&pCin, "CONIN$", "r", stdin);
		}

		RunSpecialMode(currentConfigIndex);

		if (!hide) {
			FreeConsole();
		}
		Sleep(3000);
		return 0;
	}

	



	// Create application window
	//ImGui_ImplWin32_EnableDpiAwareness();
	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"MineBackup", nullptr };
	::RegisterClassExW(&wc);
	// 创建隐藏窗口
	//HWND hwnd = CreateWindowEx(0, L"STATIC", L"HotkeyWnd", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);

	HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"MineBackup - v1.7.2", WS_OVERLAPPEDWINDOW, 100, 100, 10000, 1000, nullptr, nullptr, wc.hInstance, nullptr);
	//HWND hwnd2 = ::CreateWindowW(wc.lpszClassName, L"MineBackup", WS_OVERLAPPEDWINDOW, 100, 100, 1000, 1000, nullptr, nullptr, wc.hInstance, nullptr);
	// 注册热键，Alt + Ctrl + S
	::RegisterHotKey(hwnd, MINEBACKUP_HOTKEY_ID, MOD_ALT | MOD_CONTROL, 'S');
	
	// Initialize Direct3D
	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		::UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}

	// Show the window
	::ShowWindow(hwnd, SW_HIDE);
	//::ShowWindow(hwnd, SW_HIDE);
	::UpdateWindow(hwnd);
	//::ShowWindow(hwnd2, SW_HIDE);
	//::UpdateWindow(hwnd2);

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;

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

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

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
	//加载任务栏图标
	HICON hIcon = (HICON)LoadImage(
		GetModuleHandle(NULL),
		MAKEINTRESOURCE(IDI_ICON1),
		IMAGE_ICON,
		0, 0,
		LR_DEFAULTSIZE
	);

	if (hIcon) {
		// 设置大图标（任务栏/Alt+Tab）
		SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
		// 设置小图标（窗口标题栏）
		SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
	}

	// 背景图片加载用
	ID3D11ShaderResourceView* g_pBgTexture = nullptr;
	int g_bgWidth = 0, g_bgHeight = 0;
	wstring g_loadedBgPath = L"";

	// 设置字体和全局缩放
	float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
	io.FontGlobalScale = dpi_scale;

	bool errorShow = false;
	bool isFirstRun = !filesystem::exists("config.ini");
	static bool showConfigWizard = isFirstRun;
	static bool showMainApp = !isFirstRun;
	ImGui::StyleColorsLight();//默认亮色
	//LoadConfigs("config.ini"); 
	ApplyTheme(configs[currentConfigIndex].theme); // 把主题加载放在这里了


	/*if (specialConfigs.count(currentConfigIndex) && specialConfigs[currentConfigIndex].autoExecute) {
		specialConfigMode = true;
		ExecuteSpecialConfig(currentConfigIndex, console);
	}*/


	if (isFirstRun) {
		LANGID lang_id = GetUserDefaultUILanguage();

		if (lang_id == 2052 || lang_id == 1028) {
			g_CurrentLang = "zh-CN";
			if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttc"))
				Fontss = L"C:\\Windows\\Fonts\\msyh.ttc";
			else if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttf"))
				Fontss = L"C:\\Windows\\Fonts\\msyh.ttf";
		}
		else {
			g_CurrentLang = "en-US"; //英文
			Fontss = L"C:\\Windows\\Fonts\\SegoeUI.ttf";
		}
	}
	if (g_CurrentLang == "zh-CN")
		ImFont* font = io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
	else
		ImFont* font = io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f, nullptr, io.Fonts->GetGlyphRangesDefault());

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

	if (sevenZipExtracted) {
		console.AddLog(L("LOG_7Z_EXTRACT_SUCCESS"));
		// 如果释放成功，更新所有已加载配置的 zipPath
		for (auto& [idx, cfg] : configs) {
			cfg.zipPath = g_7zTempPath;
		}
	}
	else {
		console.AddLog(L("LOG_7Z_EXTRACT_FAIL"));
	}

	// 记录注释
	static char backupComment[CONSTANT1] = "";

	// Main loop
	bool done = false;
	while (!done)
	{
		// Poll and handle messages (inputs, window resize, etc.)
		// See the WndProc() function below for our to dispatch events to the Win32 backend.
		MSG msg;
		while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				done = true;
		}

		// Handle window being minimized or screen locked
		if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
		{
			Sleep(10);
			continue;
		}
		g_SwapChainOccluded = false;

		// Handle window resize (we don't resize directly in the WM_SIZE handler)
		if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
		{
			CleanupRenderTarget();
			g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
			g_ResizeWidth = g_ResizeHeight = 0;
			CreateRenderTarget();
		}

		// Start the Dear ImGui frame
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// 加载背景纹理
		if (configs.count(currentConfigIndex)) {
			Config& cfg = configs[currentConfigIndex];
			if (cfg.backgroundImageEnabled && !cfg.backgroundImagePath.empty() && cfg.backgroundImagePath != g_loadedBgPath) {
				if (g_pBgTexture) {
					g_pBgTexture->Release();
					g_pBgTexture = nullptr;
				}
				string path_utf8 = utf8_to_gbk(wstring_to_utf8(cfg.backgroundImagePath));
				LoadTextureFromFile(path_utf8.c_str(), &g_pBgTexture, &g_bgWidth, &g_bgHeight);
				g_loadedBgPath = cfg.backgroundImagePath;
			}
			else if (!cfg.backgroundImageEnabled && g_pBgTexture) {
				g_pBgTexture->Release();
				g_pBgTexture = nullptr;
				g_loadedBgPath = L"";
			}
		}

		// 渲染背景纹理
		if (configs.count(currentConfigIndex) && configs[currentConfigIndex].backgroundImageEnabled && g_pBgTexture) {
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
			ImGui::Begin("##background", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
			ImGui::GetWindowDrawList()->AddImage(
				(ImTextureID)g_pBgTexture,
				ImVec2(0, 0),
				ImGui::GetIO().DisplaySize,
				ImVec2(0, 0), ImVec2(1, 1),
				IM_COL32(255, 255, 255, (int)(configs[currentConfigIndex].backgroundImageAlpha * 255))
			);
			ImGui::End();
			ImGui::PopStyleVar();
		}

		if (showConfigWizard) {
			// 首次启动向导使用的静态变量
			static int page = 0;
			static char saveRootPath[CONSTANT1] = "";
			static char backupPath[CONSTANT1] = "";
			static char zipPath[CONSTANT1] = "";

			ImGui::Begin(L("WIZARD_TITLE"), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

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
						currentConfigIndex = 1;
						Config& initialConfig = configs[currentConfigIndex];

						// 1. 保存向导中收集的路径
						initialConfig.saveRoot = utf8_to_wstring(saveRootPath);
						initialConfig.backupPath = utf8_to_wstring(backupPath);
						initialConfig.zipPath = utf8_to_wstring(zipPath);

						// 2. 自动扫描存档目录，填充世界列表
						if (filesystem::exists(initialConfig.saveRoot)) {
							for (auto& entry : filesystem::directory_iterator(initialConfig.saveRoot)) {
								if (entry.is_directory()) {
									initialConfig.worlds.push_back({ entry.path().filename().wstring(), L"" }); // 名称为文件夹名，描述为空
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
						initialConfig.manualRestore = true;
						initialConfig.skipIfUnchanged = true;
						isSilence = false;
						if (g_CurrentLang == "zh-CN") {
							if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttc"))
								initialConfig.zipFonts = L"C:\\Windows\\Fonts\\msyh.ttc";
							else if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttf"))
								initialConfig.zipFonts = L"C:\\Windows\\Fonts\\msyh.ttf";
						}
						else
							initialConfig.zipFonts = L"C:\\Windows\\Fonts\\SegoeUI.ttf";

						// 4. 保存到文件并切换到主应用界面
						SaveConfigs();
						showConfigWizard = false;
						showMainApp = true;
					}
				}
			}

			ImGui::End();
		}
		else if (showMainApp) {
			static int selectedWorldIndex = -1;       // 跟踪用户在列表中选择的世界
			static bool openRestorePopup = false;     // 控制还原弹窗的打开
			static char backupComment[CONSTANT1] = "";// 备份注释输入框的内容
			// 获取当前配置
			if (configs.find(currentConfigIndex) == configs.end()) {
				if (!configs.empty()) currentConfigIndex = configs.begin()->first;
				else configs[1] = Config(); // 新建
			}
			Config& cfg = configs[currentConfigIndex];


			// --- 动态调整世界图标纹理和尺寸向量的大小 ---
			int worldCount = (int)cfg.worlds.size();
			if (worldIconTextures.size() != worldCount) {
				// 在调整大小前，释放旧的、不再需要的纹理资源
				for (auto& tex : worldIconTextures) {
					if (tex) {
						tex->Release();
						tex = nullptr;
					}
				}
				worldIconTextures.assign(worldCount, nullptr); // 使用 assign 清空并设置为指定大小的空指针
				worldIconWidths.resize(worldCount, 0);
				worldIconHeights.resize(worldCount, 0);
			}

			ImGui::SetNextWindowSize(ImVec2(1300, 720), ImGuiCond_FirstUseEver);
			ImGui::Begin(L("MAIN_WINDOW_TITLE"), &showMainApp, ImGuiWindowFlags_MenuBar);

			float totalW = ImGui::GetContentRegionAvail().x;
			float leftW = totalW * 0.32f;
			float midW = totalW * 0.25f;
			float rightW = totalW * 0.42f;

			// --- 顶部菜单栏 ---
			if (ImGui::BeginMenuBar()) {
				if (ImGui::BeginMenu(L("MENU_FILE"))) {
					if (ImGui::MenuItem(L("SETTINGS"), "CTRL+S")) { showSettings = true; }
					if (ImGui::MenuItem(L("EXIT"))) { done = true; }
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu(L("MENU_TOOLS"))) {
					if (ImGui::MenuItem(L("HISTORY_BUTTON"))) { showHistoryWindow = true; }
					ImGui::Separator();
					if (ImGui::MenuItem(L("BUTTON_BACKUP_MODS"))) { ImGui::OpenPopup(L("CONFIRM_BACKUP_MODS_TITLE")); }
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu(L("MENU_HELP"))) {
					if (ImGui::MenuItem(L("MENU_GITHUB"))) {
						ShellExecuteA(NULL, "open", "https://github.com/Leafuke/MineBackup", NULL, NULL, SW_SHOWNORMAL);
					}
					if (ImGui::MenuItem(L("MENU_ABOUT"))) {
						
					}
					ImGui::EndMenu();
				}

				// 在菜单栏右侧显示更新按钮
				if (g_NewVersionAvailable) {
					ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(L("UPDATE_AVAILABLE_BUTTON")).x - ImGui::GetStyle().FramePadding.x * 2 - 40);
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
						ImGui::BeginChild("ReleaseNotes", ImVec2(ImGui::GetContentRegionAvail().x, 150), true);
						ImGui::TextWrapped("%s", g_ReleaseNotes.c_str());
						ImGui::EndChild();
						ImGui::Separator();
						if (ImGui::Button(L("UPDATE_POPUP_DOWNLOAD_BUTTON"), ImVec2(120, 0))) {
							ShellExecuteA(NULL, "open", ("https://github.com/Leafuke/MineBackup/releases/download/" + g_LatestVersionStr + "/MineBackup.exe").c_str(), NULL, NULL, SW_SHOWNORMAL);
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
							open_update_popup = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::EndPopup();
					}
				}

				
				ImGui::EndMenuBar();
			}




			// --- 左侧面板：世界列表和操作 ---
			//ImGui::BeginChild("LeftPane", ImVec2(ImGui::GetContentRegionAvail().x * 0.875f, 0), true);
			
			float left_pane_width = ImGui::GetContentRegionAvail().x * 0.55f;
			if (selectedWorldIndex == -1) {
				left_pane_width = ImGui::GetContentRegionAvail().x; // 如果未选择任何项，左侧面板占满
			}
			
			ImGui::BeginChild("LeftPane", ImVec2(leftW, 0), true);

			ImGui::SeparatorText(L("WORLD_LIST"));
			
			// 新的自定义卡片
			//ImGui::BeginChild("WorldListChild", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3), true); // 预留底部按钮空间
			ImGui::BeginChild("WorldListChild", ImVec2(0, 0), true);

			for (int i = 0; i < cfg.worlds.size(); ++i) {
				ImGui::PushID(i);
				bool is_selected = (selectedWorldIndex == i);
				wstring worldFolder = cfg.saveRoot + L"\\" + cfg.worlds[i].first;

				// --- 左侧图标区 ---
				ImDrawList* draw_list = ImGui::GetWindowDrawList();

				float iconSz = ImGui::GetTextLineHeightWithSpacing() * 2.5f;
				ImVec2 icon_pos = ImGui::GetCursorScreenPos();
				ImVec2 icon_end_pos = ImVec2(icon_pos.x + iconSz, icon_pos.y + iconSz);

				// 绘制占位符和边框
				draw_list->AddRectFilled(icon_pos, icon_end_pos, IM_COL32(50, 50, 50, 200), 4.0f);
				draw_list->AddRect(icon_pos, icon_end_pos, IM_COL32(200, 200, 200, 200), 4.0f);

				// 迟加载
				if (!worldIconTextures[i]) {
					string iconPath = utf8_to_gbk(wstring_to_utf8(worldFolder + L"\\icon.png"));
					if (filesystem::exists(iconPath)) {
						LoadTextureFromFile(iconPath.c_str(), &worldIconTextures[i], &worldIconWidths[i], &worldIconHeights[i]);
					}
				}

				if (worldIconTextures[i]) {
					ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)worldIconTextures[i], icon_pos, icon_end_pos, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4.0f);
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
						if (worldIconTextures[i]) {
							worldIconTextures[i]->Release();
							worldIconTextures[i] = nullptr;
						}
						LoadTextureFromFile(utf8_to_gbk(wstring_to_utf8(worldFolder + L"\\icon.png")).c_str(), &worldIconTextures[i], &worldIconWidths[i], &worldIconHeights[i]);
					}
				}
				ImGui::SameLine();
				// --- 状态逻辑 (为图标做准备) ---
				lock_guard<mutex> lock(g_task_mutex); // 访问 g_active_auto_backups 需要加锁
				bool is_task_running = g_active_auto_backups.count(i);

				// 如果最后打开时间比最后备份时间新，则认为需要备份
				//wstring worldFolder = cfg.saveRoot + L"\\" + cfg.worlds[i].first;
				wstring backupFolder = cfg.backupPath + L"\\" + cfg.worlds[i].first;
				bool needs_backup = GetLastOpenTime(worldFolder) > GetLastBackupTime(backupFolder);

				// 整个区域作为一个可选项
				// ImGuiSelectableFlags_AllowItemOverlap 允许我们在可选项上面绘制其他控件
				if (ImGui::Selectable("##world_selectable", is_selected, ImGuiSelectableFlags_AllowItemOverlap, ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2.5f))) {
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
				string name_utf8 = wstring_to_utf8(cfg.worlds[i].first);
				string desc_utf8 = wstring_to_utf8(cfg.worlds[i].second);
				ImGui::TextWrapped("%s", name_utf8.c_str());

				//// --- 第二行：时间和状态 ---
				//wstring openTime = GetLastOpenTime(worldFolder);
				//wstring backupTime = GetLastBackupTime(backupFolder);

				//// 将次要信息颜色变灰，更具层次感
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
				if(desc_utf8.empty()) {
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

			ImGui::EndChild();
			
			if (selectedWorldIndex != -1) {
				ImGui::SameLine();
				ImGui::BeginChild("MidPane", ImVec2(midW, 0), true);
				{
					ImGui::SeparatorText(L("WORLD_DETAILS_PANE_TITLE"));
					ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
					ImGui::Text("%s", wstring_to_utf8(cfg.worlds[selectedWorldIndex].first).c_str());
					ImGui::PopTextWrapPos();
					ImGui::Separator();

					// -- 详细信息 --
					wstring worldFolder = cfg.saveRoot + L"\\" + cfg.worlds[selectedWorldIndex].first;
					wstring backupFolder = cfg.backupPath + L"\\" + cfg.worlds[selectedWorldIndex].first;
					ImGui::Text("%s: %s", L("TABLE_LAST_OPEN"), wstring_to_utf8(GetLastOpenTime(worldFolder)).c_str());
					ImGui::Text("%s: %s", L("TABLE_LAST_BACKUP"), wstring_to_utf8(GetLastBackupTime(backupFolder)).c_str());

					ImGui::Separator();

					// -- 注释输入框 --
					//ImGui::InputTextMultiline(L("COMMENT_HINT"), backupComment, IM_ARRAYSIZE(backupComment), ImVec2(-1, ImGui::GetTextLineHeight() * 3));
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
					ImGui::InputTextWithHint("##backup_comment", L("HINT_BACKUP_COMMENT"), backupComment, IM_ARRAYSIZE(backupComment), ImGuiInputTextFlags_EnterReturnsTrue);

					// -- 主要操作按钮 --
					float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 3.1f;
					if (ImGui::Button(L("BUTTON_BACKUP_SELECTED"), ImVec2(button_width, 30))) {
						thread backup_thread(DoBackup, cfg, cfg.worlds[selectedWorldIndex], ref(console), utf8_to_wstring(backupComment));
						backup_thread.detach();
						strcpy_s(backupComment, "");
					}
					ImGui::SameLine();
					if (ImGui::Button(L("BUTTON_RESTORE_SELECTED"), ImVec2(button_width, 30))) {
						if (cfg.manualRestore) { // 手动选择还原
							openRestorePopup = true;
							ImGui::OpenPopup(L("RESTORE_POPUP_TITLE"));
						}
						else { // 自动还原最新备份
							vector<filesystem::directory_entry> files;

							wstring backupDir = cfg.backupPath + L"\\" + cfg.worlds[selectedWorldIndex].first;
							// 收集所有常规文件
							try {
								for (const auto& entry : filesystem::directory_iterator(backupDir)) {
									if (entry.is_regular_file())
										files.push_back(entry);
								}
							}
							catch (const filesystem::filesystem_error& e) {
								console.AddLog(L("LOG_ERROR_SCAN_BACKUP_DIR"), e.what());
								continue;
							}
							// 按最后写入时间排序（最新的在前）
							sort(files.begin(), files.end(), [](const filesystem::directory_entry& a, const filesystem::directory_entry& b) {
								return filesystem::last_write_time(a) > filesystem::last_write_time(b);
								});
							if (cfg.backupBefore) {
								thread backup_thread(DoBackup, cfg, cfg.worlds[selectedWorldIndex], ref(console), L"Auto");
								backup_thread.detach(); // 分离线程，让它在后台独立运行
							}
							DoRestore(cfg, cfg.worlds[selectedWorldIndex].first, files.front().path().filename().wstring(), ref(console), 0);
						}
					}
					ImGui::SameLine();
					if (ImGui::Button(L("BUTTON_AUTO_BACKUP_SELECTED"), ImVec2(button_width, 30))) {
						ImGui::OpenPopup(L("AUTOBACKUP_SETTINGS"));
					}
					if (ImGui::Button(L("OPEN_BACKUP_FOLDER"), ImVec2(-1, 30))) {
						wstring path = cfg.backupPath + L"\\" + cfg.worlds[selectedWorldIndex].first;
						if (filesystem::exists(path)) {
							ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
						}
						else {
							ShellExecuteW(NULL, L"open", cfg.backupPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
						}
					}
					if (ImGui::Button(L("OPEN_SAVEROOT_FOLDER"), ImVec2(-1, 30))) {
						wstring path = cfg.saveRoot + L"\\" + cfg.worlds[selectedWorldIndex].first;
						ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
					}


					// 模组备份
					if (ImGui::Button(L("BUTTON_BACKUP_MODS"), ImVec2(-1, 30))) {
						if (selectedWorldIndex != -1) {
							ImGui::OpenPopup(L("CONFIRM_BACKUP_MODS_TITLE"));
						}
					}

					if (ImGui::BeginPopupModal(L("CONFIRM_BACKUP_MODS_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
						static char mods_comment[256] = "";
						ImGui::TextUnformatted(L("CONFIRM_BACKUP_MODS_MSG"));
						ImGui::InputText(L("HINT_BACKUP_COMMENT"), mods_comment, IM_ARRAYSIZE(mods_comment));
						ImGui::Separator();

						if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
							if (configs.count(currentConfigIndex)) {
								thread backup_thread(DoModsBackup, configs[currentConfigIndex], utf8_to_wstring(mods_comment));
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

					// 一些功能按钮
					if(ImGui::Button(L("EXIT_INFO"), ImVec2(-1, 30))) {
						done = true;
					}


				}

				// 自动备份弹窗
				if (ImGui::BeginPopupModal(L("AUTOBACKUP_SETTINGS"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
					bool is_task_running = false;
					{
						// 检查任务是否正在运行时，也需要加锁
						lock_guard<mutex> lock(g_task_mutex);
						is_task_running = g_active_auto_backups.count(selectedWorldIndex);
					}

					if (is_task_running) {
						ImGui::Text(L("AUTOBACKUP_RUNNING"), wstring_to_utf8(cfg.worlds[selectedWorldIndex].first).c_str());
						ImGui::Separator();
						if (ImGui::Button(L("BUTTON_STOP_AUTOBACKUP"), ImVec2(-1, 0))) {
							lock_guard<mutex> lock(g_task_mutex);
							// 1. 设置停止标志
							g_active_auto_backups.at(selectedWorldIndex).stop_flag = true;
							// 2. 等待线程结束
							if (g_active_auto_backups.at(selectedWorldIndex).worker.joinable()) {
								g_active_auto_backups.at(selectedWorldIndex).worker.join();
							}
							// 3. 从管理器中移除
							g_active_auto_backups.erase(selectedWorldIndex);
							ImGui::CloseCurrentPopup();
						}
					}
					else {
						ImGui::Text(L("AUTOBACKUP_SETUP_FOR"), wstring_to_utf8(cfg.worlds[selectedWorldIndex].first).c_str());
						ImGui::Separator();
						static int interval = 15; // 默认15分钟
						ImGui::InputInt(L("INTERVAL_MINUTES"), &interval);
						if (interval < 1) interval = 1; // 最小间隔1分钟

						if (ImGui::Button(L("BUTTON_START"), ImVec2(120, 0))) {
							lock_guard<mutex> lock(g_task_mutex);
							auto& task = g_active_auto_backups[selectedWorldIndex];
							task.stop_flag = false;
							// 启动后台线程，注意 console 是通过指针传递的
							task.worker = thread(AutoBackupThreadFunction, selectedWorldIndex, currentConfigIndex, interval, &console);
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
							ImGui::CloseCurrentPopup();
						}
					}
					ImGui::EndPopup();
				}

				// --- 还原文件选择弹窗 ---
				if (ImGui::BeginPopupModal(L("RESTORE_POPUP_TITLE"), &openRestorePopup, ImGuiWindowFlags_AlwaysAutoResize)) {
					ImGui::Text(L("RESTORE_PROMPT"), wstring_to_utf8(cfg.worlds[selectedWorldIndex].first).c_str());
					wstring backupDir = cfg.backupPath + L"\\" + cfg.worlds[selectedWorldIndex].first;
					static int selectedBackupIndex = -1;
					vector<wstring> backupFiles;

					// 遍历备份目录，找到所有文件
					if (filesystem::exists(backupDir)) {
						for (const auto& entry : filesystem::directory_iterator(backupDir)) {
							if (entry.is_regular_file()) {
								backupFiles.push_back(entry.path().filename().wstring());
							}
						}
					}

					// 显示备份文件列表
					if (ImGui::BeginListBox("##backupfiles", ImVec2(-FLT_MIN, 10 * ImGui::GetTextLineHeightWithSpacing()))) {
						for (int i = 0; i < backupFiles.size(); ++i) {
							const bool is_selected = (selectedBackupIndex == i);
							if (ImGui::Selectable(wstring_to_utf8(backupFiles[i]).c_str(), is_selected)) {
								selectedBackupIndex = i;
							}
						}
						ImGui::EndListBox();
					}

					ImGui::Separator();

					static int restore_method = 0; // 0 for Clean, 1 for Overwrite
					ImGui::SeparatorText(L("RESTORE_METHOD"));
					ImGui::RadioButton(L("RESTORE_METHOD_CLEAN"), &restore_method, 0);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_METHOD_CLEAN"));
					ImGui::SameLine();
					ImGui::RadioButton(L("RESTORE_METHOD_OVERWRITE"), &restore_method, 1);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_RESTORE_METHOD_OVERWRITE"));
					ImGui::Separator();

					if (ImGui::Button(L("BUTTON_SELECT_CUSTOM_FILE"))) {
						wstring selectedFile = SelectFileDialog();
						if (!selectedFile.empty()) {
							filesystem::path filePath(selectedFile);
							wstring extension = filePath.extension().wstring();
							// 合理的
							if (extension == L".zip" || extension == L".7z") {
								if (cfg.backupBefore) {
									thread backup_thread(DoBackup, cfg, cfg.worlds[selectedWorldIndex], ref(console), L"Auto");
									backup_thread.detach();
								}
								// 使用重载的 DoRestore 函数
								thread restore_thread(DoRestore2, cfg, cfg.worlds[selectedWorldIndex].first, filePath, ref(console), restore_method);
								restore_thread.detach();
								openRestorePopup = false;
								ImGui::CloseCurrentPopup();
							}
							else {
								MessageBoxW(hwnd, L"Error", utf8_to_wstring(L("ERROR_INVALID_ARCHIVE_TITLE")).c_str(), MB_OK | MB_ICONERROR);
							}
						}
					}
					ImGui::SameLine();

					// 确认还原按钮
					bool no_backup_selected = (selectedBackupIndex == -1);
					if (no_backup_selected) ImGui::BeginDisabled();

					if (ImGui::Button(L("BUTTON_CONFIRM_RESTORE"), ImVec2(120, 0))) {
						// 创建后台线程执行还原
						if (cfg.backupBefore) {
							thread backup_thread(DoBackup, cfg, cfg.worlds[selectedWorldIndex], ref(console), L"Auto");
							backup_thread.detach(); // 分离线程，让它在后台独立运行
						}
						thread restore_thread(DoRestore, cfg, cfg.worlds[selectedWorldIndex].first, backupFiles[selectedBackupIndex], ref(console), restore_method);
						restore_thread.detach();
						openRestorePopup = false; // 关闭弹窗
						ImGui::CloseCurrentPopup();
					}
					if (no_backup_selected) ImGui::EndDisabled();

					ImGui::SetItemDefaultFocus();
					ImGui::SameLine();
					if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
						openRestorePopup = false;
						ImGui::CloseCurrentPopup();
					}

					ImGui::EndPopup();
				}
				ImGui::EndChild();
			}
			else {
				ImGui::SameLine();
				ImGui::BeginChild("MidPane", ImVec2(midW, 0), true);
				ImGui::SeparatorText(L("WORLD_DETAILS_PANE_TITLE"));
				ImVec2 window_size = ImGui::GetWindowSize();
				ImVec2 text_size = ImGui::CalcTextSize(L("PROMPT_SELECT_WORLD"));
				ImGui::SetCursorPos(ImVec2((window_size.x - text_size.x) * 0.5f, (window_size.y - text_size.y) * 0.5f));
				ImGui::TextDisabled("%s", L("PROMPT_SELECT_WORLD"));
				ImGui::EndChild();
			}
			
			ImGui::SameLine();
			ImGui::BeginChild("RightColumn", ImVec2(rightW, 0), true);
			console.DrawEmbedded();
			ImGui::EndChild();
			//ImGui::End();
			
			//ImGui::BeginChild(L("RIGHT_PANE"));
			//if (ImGui::Button(L("SETTINGS"))) showSettings = true;
			//if (ImGui::Button(L("EXIT"))) done = true;
			//if (ImGui::Button(L("HISTORY_BUTTON"))) showHistoryWindow = true;
			//if (ImGui::Button(L("OPEN_BACKUP_FOLDER"))) {
			//	if (!cfg.backupPath.empty()) {
			//		HINSTANCE result = ShellExecuteW(NULL, L"open", cfg.backupPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
			//		/*if ((INT_PTR)result <= 32) {
			//			console.AddLog("[Error] %ls", cfg.backupPath.c_str());
			//		}*/
			//	}
			//}
			//if (ImGui::Button(L("OPEN_SAVEROOT_FOLDER"))) {
			//	if (!cfg.saveRoot.empty()) {
			//		HINSTANCE result = ShellExecuteW(NULL, L"open", cfg.saveRoot.c_str(), NULL, NULL, SW_SHOWNORMAL);
			//	}
			//}
			//static bool open_update_popup = false;
			//if (g_NewVersionAvailable) {
			//	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.902f, 0.6f, 1.0f));
			//	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.9f, 0.5f, 1.0f));
			//	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
			//	if (ImGui::Button(L("UPDATE_AVAILABLE_BUTTON"))) {
			//		open_update_popup = true;
			//		ImGui::OpenPopup(L("UPDATE_POPUP_TITLE"));
			//	}
			//	ImGui::PopStyleColor(3);
			//}
			//else {
			//	if (ImGui::Button(L("CHECK_FOR_UPDATES"))) {
			//		ShellExecuteA(NULL, "open", "https://github.com/Leafuke/MineBackup/releases", NULL, NULL, SW_SHOWNORMAL);
			//		thread update_thread(CheckForUpdatesThread);
			//		update_thread.detach();
			//	}
			//}

			//if (ImGui::BeginPopupModal(L("UPDATE_POPUP_TITLE"), &open_update_popup, ImGuiWindowFlags_AlwaysAutoResize)) {
			//	ImGui::Text(L("UPDATE_POPUP_HEADER"), g_LatestVersionStr.c_str());
			//	ImGui::Separator();
			//	ImGui::TextWrapped(L("UPDATE_POPUP_NOTES"));
			//	ImGui::BeginChild("ReleaseNotes", ImVec2(ImGui::GetContentRegionAvail().x, 150), true);
			//	ImGui::TextWrapped("%s", g_ReleaseNotes.c_str());
			//	ImGui::EndChild();
			//	ImGui::Separator();
			//	if (ImGui::Button(L("UPDATE_POPUP_DOWNLOAD_BUTTON"), ImVec2(120, 0))) {
			//		ShellExecuteA(NULL, "open", ("https://github.com/Leafuke/MineBackup/releases/download/" + g_LatestVersionStr + "/MineBackup.exe").c_str(), NULL, NULL, SW_SHOWNORMAL);
			//		open_update_popup = false;
			//		ImGui::CloseCurrentPopup();
			//	}
			//	ImGui::SameLine();
			//	if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
			//		open_update_popup = false;
			//		ImGui::CloseCurrentPopup();
			//	}
			//	ImGui::EndPopup();
			//}

			if (showSettings) ShowSettingsWindow();
			if (showHistoryWindow) ShowHistoryWindow();
			//console.Draw(L("CONSOLE_TITLE"), &showMainApp);
			//ImGui::EndChild();
			ImGui::End();
		}

		// Rendering渲染清理
		ImGui::Render();
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
		const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
		g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		// Present
		HRESULT hr = g_pSwapChain->Present(1, 0);   // Present with vsync
		//HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
		g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
	}

	// 清理
	BroadcastEvent("event=app_shutdown");
	lock_guard<mutex> lock(g_task_mutex);
	for (auto& pair : g_active_auto_backups) {
		pair.second.stop_flag = true; // 通知线程停止
		if (pair.second.worker.joinable()) {
			pair.second.worker.join(); // 等待线程执行完毕
		}
	}
	for (auto& tex : worldIconTextures) {
		if (tex) {
			tex->Release();
			tex = nullptr;
		}
	}
	worldIconTextures.clear();
	worldIconWidths.clear();
	worldIconHeights.clear();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);
	
	// 清理纹理
	if (g_pBgTexture) {
		g_pBgTexture->Release();
	}
	g_stopExitWatcher = true;
	if (g_exitWatcherThread.joinable()) {
		g_exitWatcherThread.join();
	}
	// 撤销热键
	::UnregisterHotKey(hwnd, MINEBACKUP_HOTKEY_ID);

	return 0;
}

// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
	// Setup swap chain
	DXGI_SWAP_CHAIN_DESC sd;
	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 2;
	sd.BufferDesc.Width = 0;
	sd.BufferDesc.Height = 0;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT createDeviceFlags = 0;
	//createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
	D3D_FEATURE_LEVEL featureLevel;
	const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
	HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
	if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
		res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
	if (res != S_OK)
		return false;

	CreateRenderTarget();
	return true;
}

void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
	if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
	if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
	ID3D11Texture2D* pBackBuffer;
	g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
	pBackBuffer->Release();
}

void CleanupRenderTarget()
{
	if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_HOTKEY:
		if (wParam == MINEBACKUP_HOTKEY_ID) {
			TriggerHotkeyBackup();
		}
		break;
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED)
			return 0;
		g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
		g_ResizeHeight = (UINT)HIWORD(lParam);
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
			return 0;
		break;
	case WM_DESTROY:
		::PostQuitMessage(0);
		return 0;
	}
	return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- Helper function to load an image into a D3D11 texture ---
bool LoadTextureFromFile(const char* filename, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height) {
	// Load from disk into a raw RGBA buffer
	int image_width = 0;
	int image_height = 0;
	unsigned char* image_data = stbi_load(filename, &image_width, &image_height, NULL, 4);
	if (image_data == NULL)
		return false;

	// Create texture
	D3D11_TEXTURE2D_DESC desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.Width = image_width;
	desc.Height = image_height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;

	ID3D11Texture2D* pTexture = NULL;
	D3D11_SUBRESOURCE_DATA subResource;
	subResource.pSysMem = image_data;
	subResource.SysMemPitch = desc.Width * 4;
	subResource.SysMemSlicePitch = 0;
	g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);

	// Create texture view
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	ZeroMemory(&srvDesc, sizeof(srvDesc));
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = desc.MipLevels;
	srvDesc.Texture2D.MostDetailedMip = 0;
	g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
	pTexture->Release();

	*out_width = image_width;
	*out_height = image_height;
	stbi_image_free(image_data);

	return true;
}

void CheckForUpdatesThread() {
	DWORD dwSize = 0;
	DWORD dwDownloaded = 0;
	LPSTR pszOutBuffer;
	string responseBody;
	BOOL bResults = FALSE;
	HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

	hSession = WinHttpOpen(L"MineBackup Update Checker/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) goto cleanup;

	hConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) goto cleanup;

	hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/repos/Leafuke/MineBackup/releases/latest", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) goto cleanup;

	WinHttpSendRequest(hRequest, L"User-Agent: MineBackup-Update-Checker\r\n", -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

	bResults = WinHttpReceiveResponse(hRequest, NULL);
	if (!bResults) goto cleanup;

	do {
		dwSize = 0;
		if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
		if (dwSize == 0) break;
		pszOutBuffer = new char[dwSize + 1];
		ZeroMemory(pszOutBuffer, dwSize + 1);
		if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded))
			responseBody.append(pszOutBuffer, dwDownloaded);
		delete[] pszOutBuffer;
	} while (dwSize > 0);

	try {
		string latestVersion = find_json_value(responseBody, "tag_name");
		// 移除版本号前的 'v'
		if (!latestVersion.empty() && (latestVersion[0] == 'v' || latestVersion[0] == 'V')) {
			latestVersion = latestVersion.substr(1);
		}

		// 简单版本比较 (例如 "1.7.0" > "1.6.7")
		if (!latestVersion.empty() && latestVersion > CURRENT_VERSION) {
			g_LatestVersionStr = "v" + latestVersion;
			g_NewVersionAvailable = true;
			g_ReleaseNotes = find_json_value(responseBody, "body");
			for (int i = 0; i < g_ReleaseNotes.size() - 1; ++i)
			{
				if (g_ReleaseNotes[i] == '#')
					g_ReleaseNotes[i] = ' ';
				else if (g_ReleaseNotes[i] == '\\' && g_ReleaseNotes[i + 1] == 'n')
					g_ReleaseNotes[i] = '\n', g_ReleaseNotes[i + 1] = ' ';
				else if (g_ReleaseNotes[i] == '\\')
					g_ReleaseNotes[i] = ' ', g_ReleaseNotes[i + 1] = ' ';
			}
			// 查找 .exe 下载链接  -- 直接手动拼接就行
			//string assets_key = "\"assets\": [";
			//size_t assets_start = responseBody.find(assets_key);
			//if (assets_start != string::npos) {
			//	size_t search_pos = assets_start + assets_key.length();
			//	while (search_pos < responseBody.length()) {
			//		size_t asset_obj_start = responseBody.find("{", search_pos);
			//		if (asset_obj_start == string::npos) break;
			//		size_t asset_obj_end = responseBody.find("}", asset_obj_start);
			//		if (asset_obj_end == string::npos) break;

			//		string asset_json = responseBody.substr(asset_obj_start, asset_obj_end - asset_obj_start);
			//		string asset_name = find_json_value(asset_json, "name");

			//		if (asset_name.size() > 4 && asset_name.substr(asset_name.size() - 4) == ".exe") {
			//			g_AssetDownloadURL = find_json_value(asset_json, "browser_download_url");
			//			break; // 找到即退出
			//		}
			//		search_pos = asset_obj_end;
			//	}
			//}
		}
	}
	catch (...) {
		// 解析失败，静默处理
	}

cleanup:
	if (hRequest) WinHttpCloseHandle(hRequest);
	if (hConnect) WinHttpCloseHandle(hConnect);
	if (hSession) WinHttpCloseHandle(hSession);
	g_UpdateCheckDone = true;
}

// 广播
void BroadcastEvent(const string& eventPayload) {
	if (g_signalSender) {
		g_signalSender->emitt(eventPayload);
	}
}

inline void ApplyTheme(int& theme)
{
	switch (theme) {
	case 0: ImGui::StyleColorsDark(); break;
	case 1: ImGui::StyleColorsLight(); break;
	case 2: ImGui::StyleColorsClassic(); break;
	}
}

const char* L(const char* key) {
	auto it = g_LangTable[g_CurrentLang].find(key);
	if (it != g_LangTable[g_CurrentLang].end())
		return it->second.c_str();
	return key;
}

static void LoadConfigs(const string& filename) {
	configs.clear();
	specialConfigs.clear();
	ifstream in(filename, ios::binary);
	if (!in.is_open()) return;
	string line1;
	wstring line, section;
	// cur作为一个指针，指向 configs 这个全局 map<int, Config> 中的元素 Config
	Config* cur = nullptr;
	SpecialConfig* spCur = nullptr;

	while (getline(in, line1)) {
		line = utf8_to_wstring(line1);
		if (line.empty() || line.front() == L'#') continue;
		if (line.front() == L'[' && line.back() == L']') {
			section = line.substr(1, line.size() - 2);
			spCur = nullptr;
			cur = nullptr;
			if (section.find(L"Config", 0) == 0) {
				int idx = stoi(section.substr(6));
				configs[idx] = Config();
				cur = &configs[idx];
			}
			else if (section.find(L"SpCfg", 0) == 0) {
				int idx = stoi(section.substr(5));
				specialConfigs[idx] = SpecialConfig();
				spCur = &specialConfigs[idx];
			}
		}
		else {
			auto pos = line.find(L'=');
			if (pos == wstring::npos) continue;
			wstring key = line.substr(0, pos);
			wstring val = line.substr(pos + 1);

			if (cur) { // Inside a [ConfigN] section
				if (key == L"ConfigName") cur->name = wstring_to_utf8(val);
				else if (key == L"SavePath") {
					cur->saveRoot = val;
				}
				else if (key == L"WorldData") {
					while (getline(in, line1) && line1 != "*") {
						line = utf8_to_wstring(line1);
						wstring name = line;
						if (!getline(in, line1) || line1 == "*") break;
						line = utf8_to_wstring(line1);
						wstring desc = line;
						cur->worlds.push_back({ name, desc });
					}
					if (filesystem::exists(cur->saveRoot)) {
						for (auto& entry : filesystem::directory_iterator(cur->saveRoot)) {
							if (entry.is_directory() && checkWorldName(entry.path().filename().wstring(), cur->worlds))
								cur->worlds.push_back({ entry.path().filename().wstring(), L"" });
						}
					}
				}
				else if (key == L"BackupPath") cur->backupPath = val;
				else if (key == L"ZipProgram") cur->zipPath = val;
				else if (key == L"ZipFormat") cur->zipFormat = val;
				else if (key == L"ZipLevel") cur->zipLevel = stoi(val);
				else if (key == L"KeepCount") cur->keepCount = stoi(val);
				else if (key == L"SmartBackup") cur->backupMode = stoi(val);
				else if (key == L"RestoreBeforeBackup") cur->backupBefore = (val != L"0");
				else if (key == L"HotBackup") cur->hotBackup = (val != L"0");
				else if (key == L"ManualRestore") cur->manualRestore = (val != L"0");
				else if (key == L"SilenceMode") isSilence = (val != L"0");
				else if (key == L"BackupNaming") cur->folderNameType = stoi(val);
				else if (key == L"SilenceMode") isSilence = (val != L"0");
				else if (key == L"CpuThreads") cur->cpuThreads = stoi(val);
				else if (key == L"UseLowPriority") cur->useLowPriority = (val != L"0");
				else if (key == L"SkipIfUnchanged") cur->skipIfUnchanged = (val != L"0");
				else if (key == L"MaxSmartBackups") cur->maxSmartBackupsPerFull = stoi(val);
				else if (key == L"BackupOnExit") cur->backupOnGameExit = (val != L"0");
				else if (key == L"BlacklistItem") cur->blacklist.push_back(val);
				else if (key == L"Theme") {
					cur->theme = stoi(val);
					//ApplyTheme(cur->theme); 这个要转移至有gui之后，否则会直接导致崩溃
				}
				else if (key == L"Font") {
					cur->zipFonts = val;
					Fontss = val;
				}
				else if (key == L"ThemeColor") {
					cur->themeColor = val;
					float r, g, b, a;
					if (swscanf_s(val.c_str(), L"%f %f %f %f", &r, &g, &b, &a) == 4) {
						clear_color = ImVec4(r, g, b, a);
					}
				}
				else if (key == L"BackgroundImage") {
					if (val.size() > 2) {
						cur->backgroundImageEnabled = true;
						cur->backgroundImagePath = val;
					}
					else
						cur->backgroundImageEnabled = false;
				}
			}
			else if (spCur) { // Inside a [SpCfgN] section
				if (key == L"Name") spCur->name = wstring_to_utf8(val);
				else if (key == L"AutoExecute") {
					spCur->autoExecute = (val != L"0");
					if (spCur->autoExecute)
						specialConfigMode = true;
				}
				else if (key == L"ExitAfter") spCur->exitAfterExecution = (val != L"0");
				else if (key == L"HideWindow") spCur->hideWindow = (val != L"0");
				else if (key == L"RunOnStartup") spCur->runOnStartup = (val != L"0");
				else if (key == L"Command") spCur->commands.push_back(val);
				else if (key == L"AutoBackupTask") {
					wstringstream ss(val);
					AutomatedTask task;
					wchar_t delim;
					ss >> task.configIndex >> delim >> task.worldIndex >> delim >> task.backupType >> delim >> task.intervalMinutes >> delim >> task.schedMonth >> delim >> task.schedDay >> delim >> task.schedHour >> delim >> task.schedMinute;
					spCur->tasks.push_back(task);
				}
				else if (key == L"ZipLevel") spCur->zipLevel = stoi(val);
				else if (key == L"KeepCount") spCur->keepCount = stoi(val);
				else if (key == L"CpuThreads") spCur->cpuThreads = stoi(val);
				else if (key == L"UseLowPriority") spCur->useLowPriority = (val != L"0");
				else if (key == L"HotBackup") spCur->hotBackup = (val != L"0");
				else if (key == L"BackupOnExit") spCur->backupOnGameExit = (val != L"0");
				else if (key == L"BlacklistItem") spCur->blacklist.push_back(val);
			}
			else if (section == L"General") { // Inside [General] section
				if (key == L"CurrentConfig") {
					currentConfigIndex = stoi(val);
				}
				else if (key == L"Language") {
					g_CurrentLang = wstring_to_utf8(val);
				}
				else if (key == L"CheckForUpdates") {
					g_CheckForUpdates = (val != L"0");
				}
				else if (key == L"EnableKnotLink") { 
					g_enableKnotLink = (val != L"0");
				}
			}
		}
	}
}

static void SaveConfigs(const wstring& filename) {
	//lock_guard<mutex> lock(g_configsMutex);
	wofstream out(filename, ios::binary);
	if (!out.is_open()) {
		MessageBoxW(nullptr, utf8_to_wstring(L("ERROR_CONFIG_WRITE_FAIL")).c_str(), utf8_to_wstring(L("ERROR_TITLE")).c_str(), MB_OK | MB_ICONERROR);
		return;
	}
	//out.imbue(locale("chs"));//不能用这个，变ANSI啦
	out.imbue(locale(out.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
	//out.imbue(locale(out.getloc(), new codecvt_utf8<wchar_t>));//将UTF8转为UTF，现在C++17也不对了……但是我们有define！
	out << L"[General]\n";
	out << L"CurrentConfig=" << currentConfigIndex << L"\n";
	out << L"Language=" << utf8_to_wstring(g_CurrentLang) << L"\n";
	out << L"CheckForUpdates=" << (g_CheckForUpdates ? 1 : 0) << L"\n\n";
	out << L"EnableKnotLink=" << (g_enableKnotLink ? 1 : 0) << L"\n\n";

	for (auto& kv : configs) {
		int idx = kv.first;
		Config& c = kv.second;
		out << L"[Config" << idx << L"]\n";
		out << L"ConfigName=" << utf8_to_wstring(c.name) << L"\n";
		out << L"SavePath=" << c.saveRoot << L"\n";
		out << L"# One line for name, one line for description, terminated by '*'\n";
		out << L"WorldData=\n";
		for (auto& p : c.worlds)
			out << p.first << L"\n" << p.second << L"\n";
		out << L"*\n";
		out << L"BackupPath=" << c.backupPath << L"\n";
		out << L"ZipProgram=" << c.zipPath << L"\n";
		out << L"ZipFormat=" << c.zipFormat << L"\n";
		out << L"ZipLevel=" << c.zipLevel << L"\n";
		out << L"CpuThreads=" << c.cpuThreads << L"\n";
		out << L"UseLowPriority=" << (c.useLowPriority ? 1 : 0) << L"\n";
		out << L"KeepCount=" << c.keepCount << L"\n";
		out << L"SmartBackup=" << c.backupMode << L"\n";
		out << L"RestoreBeforeBackup=" << (c.backupBefore ? 1 : 0) << L"\n";
		out << L"HotBackup=" << (c.hotBackup ? 1 : 0) << L"\n";
		out << L"ManualRestore=" << (c.manualRestore ? 1 : 0) << L"\n";
		out << L"SilenceMode=" << (isSilence ? 1 : 0) << L"\n";
		out << L"Theme=" << c.theme << L"\n";
		out << L"Font=" << c.zipFonts << L"\n";
		out << L"ThemeColor=" << c.themeColor << L"\n";
		out << L"BackupNaming=" << c.folderNameType << L"\n";
		out << L"SilenceMode=" << (isSilence ? 1 : 0) << L"\n";
		out << L"BackgroundImage=" << c.backgroundImagePath << L"\n";
		out << L"SkipIfUnchanged=" << (c.skipIfUnchanged ? 1 : 0) << L"\n";
		out << L"MaxSmartBackups=" << c.maxSmartBackupsPerFull << L"\n";
		out << L"BackupOnExit=" << (c.backupOnGameExit ? 1 : 0) << L"\n";
		for (const auto& item : c.blacklist) {
			out << L"BlacklistItem=" << item << L"\n";
		}
		out << L"\n";
	}

	for (auto& kv : specialConfigs) {
		int idx = kv.first;
		SpecialConfig& sc = kv.second;
		out << L"[SpCfg" << idx << L"]\n";
		out << L"Name=" << utf8_to_wstring(sc.name) << L"\n";
		out << L"AutoExecute=" << (sc.autoExecute ? 1 : 0) << L"\n";
		for (const auto& cmd : sc.commands) out << L"Command=" << cmd << L"\n";
		// 新的任务结构
		for (const auto& task : sc.tasks) {
			out << L"AutoBackupTask=" << task.configIndex << L"," << task.worldIndex << L"," << task.backupType
				<< L"," << task.intervalMinutes << L"," << task.schedMonth << L"," << task.schedDay
				<< L"," << task.schedHour << L"," << task.schedMinute << L"\n";
		}
		out << L"ExitAfter=" << (sc.exitAfterExecution ? 1 : 0) << L"\n";
		out << L"HideWindow=" << (sc.hideWindow ? 1 : 0) << L"\n";
		out << L"RunOnStartup=" << (sc.runOnStartup ? 1 : 0) << L"\n";
		out << L"ZipLevel=" << sc.zipLevel << L"\n";
		out << L"KeepCount=" << sc.keepCount << L"\n";
		out << L"CpuThreads=" << sc.cpuThreads << L"\n";
		out << L"UseLowPriority=" << (sc.useLowPriority ? 1 : 0) << L"\n";
		out << L"HotBackup=" << (sc.hotBackup ? 1 : 0) << L"\n";
		out << L"BackupOnExit=" << (sc.backupOnGameExit ? 1 : 0) << L"\n";
		for (const auto& item : sc.blacklist) {
			out << L"BlacklistItem=" << item << L"\n";
		}
		out << L"\n\n";
	}
}

void ShowSettingsWindow() {
	ImGui::Begin(L("SETTINGS"), &showSettings, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
	ImGui::SeparatorText(L("CONFIG_MANAGEMENT"));

	// 加锁，避免远程命令冲突
	lock_guard<mutex> lock(g_configsMutex);

	if (configs.empty()) {
		configs[1] = Config();
		currentConfigIndex = 1;
	}
	Config& cfg = configs[currentConfigIndex];

	string current_config_label = "None";
	if (specialSetting && specialConfigs.count(currentConfigIndex)) {
		current_config_label = "[Sp." + to_string(currentConfigIndex) + "] " + specialConfigs[currentConfigIndex].name;
	}
	else if (!specialSetting && configs.count(currentConfigIndex)) {
		current_config_label = "[No." + to_string(currentConfigIndex) + "] " + configs[currentConfigIndex].name;
	}
	//string(L("CONFIG_N")) + to_string(currentConfigIndex)
	if (ImGui::BeginCombo(L("CURRENT_CONFIG"), current_config_label.c_str())) {
		// 普通配置
		for (auto const& [idx, val] : configs) {
			const bool is_selected = (currentConfigIndex == idx);
			string label = "[No." + to_string(idx) + "] " + val.name;

			if (ImGui::Selectable(label.c_str(), is_selected)) {
				currentConfigIndex = idx;
				specialSetting = false;
			}
			if (is_selected) {
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::Separator();
		// 特殊配置
		for (auto const& [idx, val] : specialConfigs) {
			const bool is_selected = (currentConfigIndex == (idx));
			string label = "[Sp." + to_string((idx)) + "] " + val.name;
			if (ImGui::Selectable(label.c_str(), is_selected)) {
				currentConfigIndex = (idx);
				specialSetting = true;
				//specialConfigMode = true;
			}
			if (is_selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	if (ImGui::Button(L("BUTTON_ADD_CONFIG"))) {
		int new_index = configs.empty() ? 1 : configs.rbegin()->first + 1;
		configs[new_index] = Config(); // Create default config
		currentConfigIndex = new_index; // Switch to the new one
		specialConfigMode = false;

		Config& new_cfg = configs[currentConfigIndex];
		new_cfg.zipFormat = L"7z";
		new_cfg.zipLevel = 5;
		new_cfg.keepCount = 0;
		new_cfg.backupMode = 1;
		new_cfg.hotBackup = false;
		new_cfg.backupBefore = false;
		new_cfg.manualRestore = true;
		new_cfg.cpuThreads = 0;
		new_cfg.useLowPriority = false;
		isSilence = false;
		if (g_CurrentLang == "zh-CN") {
			if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttc"))
				new_cfg.zipFonts = L"C:\\Windows\\Fonts\\msyh.ttc";
			else if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttf"))
				new_cfg.zipFonts = L"C:\\Windows\\Fonts\\msyh.ttf";
		}
		else
			new_cfg.zipFonts = L"C:\\Windows\\Fonts\\SegoeUI.ttf";
		new_cfg.themeColor = L"0.45 0.55 0.60 1.00";
	}
	ImGui::SameLine();

	if (ImGui::Button(L("ADD_SPECIAL_CONFIG"))) {
		int new_index = specialConfigs.empty() ? 1 : (specialConfigs.rbegin()->first + 1);
		specialConfigs[new_index] = SpecialConfig();
		//specialConfigs[new_index].name = "New Auto Task";
		currentConfigIndex = new_index;
		specialSetting = true;
		//specialConfigMode = true; 不能直接进入特殊模式，这样都没法设置了
	}

	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_DELETE_CONFIG"))) {
		if ((!specialSetting && configs.size() > 1) || (specialSetting && !specialConfigs.empty())) { // 至少保留一个
			ImGui::OpenPopup(L("CONFIRM_DELETE_TITLE"));
		}
	}

	if (ImGui::BeginPopupModal(L("CONFIRM_DELETE_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (specialSetting) {
			ImGui::Text("[Sp.]");
			ImGui::SameLine();
			ImGui::Text(L("CONFIRM_DELETE_MSG"), currentConfigIndex);
		}
		else {
			ImGui::Text(L("CONFIRM_DELETE_MSG"), currentConfigIndex);
		}
		ImGui::Separator();
		if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
			if (specialSetting) {
				specialConfigs.erase(currentConfigIndex);
				specialConfigMode = false;
				currentConfigIndex = configs.empty() ? 0 : configs.begin()->first;
			}
			else {
				configs.erase(currentConfigIndex);
				currentConfigIndex = configs.begin()->first;
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (!specialSetting) {
		ImGui::Checkbox(L("CHECK_FOR_UPDATES_ON_STARTUP"), &g_CheckForUpdates);
		ImGui::SameLine();
		ImGui::Checkbox(L("ENABLE_KNOTLINK"), &g_enableKnotLink);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_ENABLE_KNOTLINK"));
	}

	ImGui::Dummy(ImVec2(0.0f, 10.0f));
	ImGui::SeparatorText(L("CURRENT_CONFIG_DETAILS"));

	if (specialSetting) {
		if (!specialConfigs.count(currentConfigIndex)) {
			specialSetting = false;
			currentConfigIndex = configs.empty() ? 1 : configs.begin()->first;
		}
		else {
			SpecialConfig& spCfg = specialConfigs[currentConfigIndex];

			char buf[128];
			strncpy_s(buf, spCfg.name.c_str(), sizeof(buf));
			if (ImGui::InputText(L("CONFIG_NAME"), buf, sizeof(buf))) spCfg.name = buf;

			ImGui::Checkbox(L("EXECUTE_ON_STARTUP"), &spCfg.autoExecute);
			ImGui::Checkbox(L("EXIT_WHEN_FINISHED"), &spCfg.exitAfterExecution);
			if (ImGui::Checkbox(L("RUN_ON_WINDOWS_STARTUP"), &spCfg.runOnStartup)) {
				wchar_t selfPath[MAX_PATH];
				GetModuleFileNameW(NULL, selfPath, MAX_PATH);
				SetAutoStart("MineBackup_AutoTask_" + to_string(currentConfigIndex), selfPath, currentConfigIndex, spCfg.runOnStartup);
			}
			ImGui::Checkbox(L("HIDE_CONSOLE_WINDOW"), &spCfg.hideWindow);

			ImGui::SeparatorText(L("GROUP_BACKUP_BEHAVIOR"));
			ImGui::Checkbox(L("IS_HOT_BACKUP"), &spCfg.hotBackup);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_HOT_BACKUP"));
			ImGui::SameLine();
			ImGui::Checkbox(L("BACKUP_ON_EXIT"), &spCfg.backupOnGameExit);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_BACKUP_ON_EXIT"));
			ImGui::SameLine();
			ImGui::Checkbox(L("USE_LOW_PRIORITY"), &spCfg.useLowPriority);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_LOW_PRIORITY"));

			int max_threads = thread::hardware_concurrency();
			ImGui::SliderInt(L("CPU_THREAD_COUNT"), &spCfg.cpuThreads, 0, max_threads);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_CPU_THREADS"));
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

				string current_task_config_label = configs.count(task.configIndex) ? (string(L("CONFIG_N")) + to_string(task.configIndex)) : "None";
				if (ImGui::BeginCombo(L("CONFIG_COMBO"), current_task_config_label.c_str())) {
					for (auto const& [idx, val] : configs) {
						if (ImGui::Selectable((string(L("CONFIG_N")) + to_string(idx)).c_str(), task.configIndex == idx)) {
							task.configIndex = idx;
							task.worldIndex = val.worlds.empty() ? -1 : 0; // 重置世界idx
						}
					}
					ImGui::EndCombo();
				}

				if (configs.count(task.configIndex)) {
					Config& selected_cfg = configs[task.configIndex];
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

				ImGui::Combo(L("TASK_BACKUP_TYPE"), &task.backupType, "Once\0Interval\0Scheduled\0");

				if (task.backupType == 1) { // 间隔
					ImGui::InputInt(L("INTERVAL_MINUTES"), &task.intervalMinutes);
					if (task.intervalMinutes < 1) task.intervalMinutes = 1;
				}
				else if (task.backupType == 2) { // 计划
					ImGui::Text("At:"); ImGui::SameLine();
					ImGui::SetNextItemWidth(50); ImGui::InputInt(L("SCHED_HOUR"), &task.schedHour);
					ImGui::SameLine(); ImGui::Text(":"); ImGui::SameLine();
					ImGui::SetNextItemWidth(50); ImGui::InputInt(L("SCHED_MINUTE"), &task.schedMinute);
					ImGui::SameLine(); ImGui::Text("On (Month/Day):"); ImGui::SameLine();
					ImGui::SetNextItemWidth(50); ImGui::InputInt(L("SCHED_MONTH"), &task.schedMonth);
					ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
					ImGui::SetNextItemWidth(50); ImGui::InputInt(L("SCHED_DAY"), &task.schedDay);
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
		if (!configs.count(currentConfigIndex)) {
			// 如果配置被删除
			if (configs.empty()) configs[1] = Config(); // 如果1被删，新建
			currentConfigIndex = configs.begin()->first;
		}
		Config& cfg = configs[currentConfigIndex];
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
		}

		// Language selection
		int lang_idx = 0;
		for (int i = 0; i < IM_ARRAYSIZE(lang_codes); ++i) {
			if (g_CurrentLang == lang_codes[i]) {
				lang_idx = i;
				break;
			}
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
				char name[CONSTANT1], desc[CONSTANT2];
				strncpy_s(name, wstring_to_utf8(cfg.worlds[i].first).c_str(), sizeof(name));
				strncpy_s(desc, wstring_to_utf8(cfg.worlds[i].second).c_str(), sizeof(desc));

				if (ImGui::InputText(L("WORLD_NAME"), name, CONSTANT1))
					cfg.worlds[i].first = utf8_to_wstring(name);
				if (cfg.worlds[i].second.find(L"\"") != wstring::npos || cfg.worlds[i].second.find(L":") != wstring::npos || cfg.worlds[i].second.find(L"\\") != wstring::npos || cfg.worlds[i].second.find(L"/") != wstring::npos || cfg.worlds[i].second.find(L">") != wstring::npos || cfg.worlds[i].second.find(L"<") != wstring::npos || cfg.worlds[i].second.find(L"|") != wstring::npos || cfg.worlds[i].second.find(L"?") != wstring::npos || cfg.worlds[i].second.find(L"*") != wstring::npos) {
					memset(desc, '\0', sizeof(desc));
					cfg.worlds[i].second = L"";
				}
				if (ImGui::InputText(L("WORLD_DESC"), desc, CONSTANT2))
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

			ImGui::Checkbox(L("BACKUP_BEFORE_RESTORE"), &cfg.backupBefore); ImGui::SameLine();
			ImGui::Checkbox(L("IS_HOT_BACKUP"), &cfg.hotBackup); ImGui::SameLine();
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(L("TIP_HOT_BACKUP"));
			}
			ImGui::Checkbox(L("BACKUP_ON_EXIT"), &cfg.backupOnGameExit);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(L("TIP_BACKUP_ON_EXIT"));
			ImGui::Checkbox(L("MANUAL_RESTORE_SELECT"), &cfg.manualRestore); ImGui::SameLine();
			ImGui::Checkbox(L("SHOW_PROGRESS"), &isSilence);
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
			// CPU 线程
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
				wstring sel = SelectFileDialog(); if (!sel.empty()) cfg.blacklist.push_back(sel);
			}
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_ADD_FOLDER_BLACKLIST"))) {
				wstring sel = SelectFolderDialog(); if (!sel.empty()) cfg.blacklist.push_back(sel);
			}
			static int sel_bl_item = -1;

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
					}
				}
				ImGui::EndListBox();
			}


			if (ImGui::Button(L("BUTTON_REMOVE_BLACKLIST")) && sel_bl_item != -1) {
				cfg.blacklist.erase(cfg.blacklist.begin() + sel_bl_item); sel_bl_item = -1;
			}
		}

		if (ImGui::CollapsingHeader(L("GROUP_APPEARANCE"))) {
			if (ImGui::Combo(L("LANGUAGE"), &lang_idx, langs, IM_ARRAYSIZE(langs))) {
				g_CurrentLang = lang_codes[lang_idx];
				//ReloadFonts(); // Reload fonts for the new language
			}
			ImGui::Separator();
			ImGui::Text(L("THEME_SETTINGS"));
			int theme_choice = (int)cfg.theme;
			if (ImGui::RadioButton(L("THEME_DARK"), &theme_choice, 0)) { cfg.theme = 0; ApplyTheme(cfg.theme); } ImGui::SameLine();
			if (ImGui::RadioButton(L("THEME_LIGHT"), &theme_choice, 1)) { cfg.theme = 1; ApplyTheme(cfg.theme); } ImGui::SameLine();
			if (ImGui::RadioButton(L("THEME_CLASSIC"), &theme_choice, 2)) { cfg.theme = 2; ApplyTheme(cfg.theme); }

			static float window_alpha = ImGui::GetStyle().Alpha;
			if (ImGui::SliderFloat(L("WINDOW_OPACITY"), &window_alpha, 0.2f, 1.0f, "%.2f")) {
				ImGui::GetStyle().Alpha = window_alpha;
			}
			ImGui::ColorEdit3(L("BG_COLOR"), (float*)&clear_color);

			ImGui::Text(L("FONT_SETTINGS"));
			char Fonts[CONSTANT1];
			strncpy_s(Fonts, wstring_to_utf8(cfg.zipFonts).c_str(), sizeof(Fonts));
			if (ImGui::Button(L("BUTTON_SELECT_FONT"))) {
				wstring sel = SelectFileDialog();
				if (!sel.empty()) {
					cfg.zipFonts = sel;
					Fontss = sel;
					//ReloadFonts();
				}
			}
			ImGui::SameLine();
			if (ImGui::InputText("##zipFontsValue", Fonts, CONSTANT1)) {
				cfg.zipFonts = utf8_to_wstring(Fonts);
				Fontss = cfg.zipFonts;
				//ReloadFonts();
			}
			ImGui::SeparatorText(L("BACKGROUND_IMAGE"));
			Config& cfg = configs[currentConfigIndex];
			ImGui::Checkbox(L("ENABLE_BACKGROUND_IMAGE"), &cfg.backgroundImageEnabled);

			if (cfg.backgroundImageEnabled) {
				ImGui::SliderFloat(L("BACKGROUND_IMAGE_OPACITY"), &cfg.backgroundImageAlpha, 0.0f, 1.0f);

				char bgPathBuf[CONSTANT1];
				strncpy_s(bgPathBuf, wstring_to_utf8(cfg.backgroundImagePath).c_str(), sizeof(bgPathBuf));

				if (ImGui::Button(L("BUTTON_SELECT_IMAGE"))) {
					wstring sel = SelectFileDialog();
					if (!sel.empty()) {
						cfg.backgroundImagePath = sel;
					}
				}
				ImGui::SameLine();
				ImGui::InputText(L("BACKGROUND_IMAGE_PATH"), bgPathBuf, CONSTANT1, ImGuiInputTextFlags_ReadOnly);
			}
		}
	}

	ImGui::Dummy(ImVec2(0.0f, 10.0f));
	if (ImGui::Button(L("BUTTON_SAVE_AND_CLOSE"), ImVec2(120, 0))) {
		if (!specialConfigMode) {
			wchar_t colorBuf[64];
			swprintf(colorBuf, 64, L"%.2f %.2f %.2f %.2f", clear_color.x, clear_color.y, clear_color.z, clear_color.w);
			configs[currentConfigIndex].themeColor = colorBuf;
		}
		SaveConfigs();
		showSettings = false;
	}
	ImGui::End();
}

// 限制备份文件数量，超出则自动删除最旧的
void LimitBackupFiles(const wstring& folderPath, int limit, Console* console)
{
	if (limit <= 0) return;
	namespace fs = filesystem;
	vector<fs::directory_entry> files;

	// 收集所有常规文件
	try {
		if (!fs::exists(folderPath) || !fs::is_directory(folderPath))
			return;
		for (const auto& entry : fs::directory_iterator(folderPath)) {
			if (entry.is_regular_file())
				files.push_back(entry);
		}
	}
	catch (const fs::filesystem_error& e) {
		if (console) console->AddLog(L("LOG_ERROR_SCAN_BACKUP_DIR"), e.what());
		return;
	}

	// 如果未超出限制，无需处理
	if ((int)files.size() <= limit) return;

	// 按最后写入时间升序排序（最旧的在前）
	sort(files.begin(), files.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
		return fs::last_write_time(a) < fs::last_write_time(b);
		});

	// 删除多余的最旧文件
	int to_delete = (int)files.size() - limit;
	for (int i = 0; i < to_delete && to_delete <= (int)files.size(); ++i) {
		try {
			if (files[i].path().filename().wstring().find(L"[Smart]") == 0 || files[i + 1].path().filename().wstring().find(L"[Smart]") == 0) // 如果是智能备份，不能删除！如果是完整备份，不能是基底
			{
				to_delete += 1;
				continue;
			}
			fs::remove(files[i]);
			if (console) console->AddLog(L("LOG_DELETE_OLD_BACKUP"), wstring_to_utf8(files[i].path().filename().wstring()).c_str());
		}
		catch (const fs::filesystem_error& e) {
			if (console) console->AddLog(L("LOG_ERROR_DELETE_BACKUP"), e.what());
		}
	}
}

//在后台静默执行一个命令行程序（如7z.exe），并等待其完成。
//这是实现备份和还原功能的核心，避免了GUI卡顿和黑窗口弹出。
// 参数:
//   - command: 要执行的完整命令行（宽字符）。
//   - console: 监控台对象的引用，用于输出日志信息。
bool RunCommandInBackground(wstring command, Console& console, bool useLowPriority, const wstring& workingDirectory) {
	// CreateProcessW需要一个可写的C-style字符串，所以我们将wstring复制到vector<wchar_t>
	vector<wchar_t> cmd_line(command.begin(), command.end());
	cmd_line.push_back(L'\0'); // 添加字符串结束符

	STARTUPINFOW si = {};
	PROCESS_INFORMATION pi = {};
	si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE; // 隐藏子进程的窗口

	DWORD creationFlags = CREATE_NO_WINDOW;
	if (useLowPriority) {
		creationFlags |= BELOW_NORMAL_PRIORITY_CLASS;
	}

	// 开始创建进程
	const wchar_t* pWorkingDir = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
	console.AddLog(L("LOG_EXEC_CMD"), wstring_to_utf8(command).c_str());

	if (!CreateProcessW(NULL, cmd_line.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, pWorkingDir, &si, &pi)) {
		console.AddLog(L("LOG_ERROR_CREATE_PROCESS"), GetLastError());
		return false;
	}

	// 等待子进程执行完毕
	WaitForSingleObject(pi.hProcess, INFINITE);

	// 检查子进程的退出代码
	DWORD exit_code;
	if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
		if (exit_code == 0) {
			console.AddLog(L("LOG_SUCCESS_CMD"));
		}
		else {
			console.AddLog(L("LOG_ERROR_CMD_FAILED"), exit_code);
			if (exit_code == 1)
				console.AddLog(L("LOG_ERROR_CMD_FAILED_HOTBACKUP_SUGGESTION"));
		}
	}
	else {
		console.AddLog(L("LOG_ERROR_GET_EXIT_CODE"));
	}

	// 清理句柄
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return true;
}

// 创建快照，用于热备份
wstring CreateWorldSnapshot(const filesystem::path& worldPath, Console& console) {
	try {
		// 创建一个唯一的临时目录
		filesystem::path tempDir = filesystem::temp_directory_path() / L"MineBackup_Snapshot" / worldPath.filename();

		// 如果旧的临时目录存在，先清理掉
		if (filesystem::exists(tempDir)) {
			filesystem::remove_all(tempDir);
		}
		filesystem::create_directories(tempDir);
		console.AddLog(L("LOG_BACKUP_HOT_INFO"));

		// 递归复制，并尝试忽略单个文件错误
		auto copyOptions = filesystem::copy_options::recursive | filesystem::copy_options::overwrite_existing;
		error_code ec;
		filesystem::copy(worldPath, tempDir, copyOptions, ec);

		if (ec) {
			// 虽然发生了错误（可能是某个文件被锁定了），但大部分文件可能已经复制成功
			console.AddLog(L("LOG_BACKUP_HOT_INFO2"), ec.message().c_str());
			wstring xcopyCmd = L"xcopy \"" + worldPath.wstring() + L"\" \"" + tempDir.wstring() + L"\" /s /e /y /c";
			RunCommandInBackground(xcopyCmd, console, false);
		}
		else {
			console.AddLog(L("LOG_BACKUP_HOT_INFO3"), wstring_to_utf8(tempDir.wstring()).c_str());
		}

		return tempDir.wstring();

	}
	catch (const filesystem::filesystem_error& e) {
		console.AddLog(L("LOG_BACKUP_HOT_INFO4"), e.what());
		return L"";
	}
}

// 执行单个世界的备份操作。
// 参数:
//   - config: 当前使用的配置。
//   - world:  要备份的世界（名称+描述）。
//   - console: 监控台对象的引用，用于输出日志信息。
   //- commend: 用户注释
void DoBackup(const Config config, const pair<wstring, wstring> world, Console& console, const wstring& comment) {
	console.AddLog(L("LOG_BACKUP_START_HEADER"));
	console.AddLog(L("LOG_BACKUP_PREPARE"), wstring_to_utf8(world.first).c_str());

	if (!filesystem::exists(config.zipPath)) {
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return;
	}

	// 准备路径
	wstring originalSourcePath = config.saveRoot + L"\\" + world.first;
	wstring sourcePath = originalSourcePath; // 默认使用原始路径
	wstring destinationFolder = config.backupPath + L"\\" + world.first;
	wstring metadataFolder = config.backupPath + L"\\_metadata\\" + world.first; // 元数据文件夹
	wstring command;
	wstring archivePath;
	wstring archiveNameBase = world.second.empty() ? world.first : world.second;

	if (!comment.empty()) {
		archiveNameBase += L" [" + SanitizeFileName(comment) + L"]";
	}

	// 建立黑名单
	wstringstream exclusion_ss;
	for (const auto& item : config.blacklist) {
		exclusion_ss << L" -x!\"" << item << L"\"";
	}
	wstring exclusion_args = exclusion_ss.str();

	// 生成带时间戳的文件名
	time_t now = time(0);
	tm ltm;
	localtime_s(&ltm, &now);
	wchar_t timeBuf[80];
	wcsftime(timeBuf, sizeof(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);
	archivePath = destinationFolder + L"\\" + L"[" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;

	wstring baseArchivePath = destinationFolder + L"\\" + archiveNameBase + L"." + config.zipFormat;

	// 创建备份目标文件夹（如果不存在）
	try {
		filesystem::create_directories(destinationFolder);
		filesystem::create_directories(metadataFolder); // 确保元数据文件夹存在
		console.AddLog(L("LOG_BACKUP_DIR_IS"), wstring_to_utf8(destinationFolder).c_str());
	}
	catch (const filesystem::filesystem_error& e) {
		console.AddLog(L("LOG_ERROR_CREATE_BACKUP_DIR"), e.what());
		return;
	}

	// 如果打开了热备份
	if (config.hotBackup) {
		wstring snapshotPath = CreateWorldSnapshot(sourcePath, console);
		if (!snapshotPath.empty()) {
			sourcePath = snapshotPath; // 如果快照成功，则后续所有操作都基于快照路径
			originalSourcePath = snapshotPath;

		}
		else {
			console.AddLog(L("LOG_ERROR_SNAPSHOT"));
			return;
		}
	}

	bool forceFullBackup = true;
	if (filesystem::exists(destinationFolder)) {
		for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
			if (entry.is_regular_file()) {
				forceFullBackup = false;
				break;
			}
		}
	}
	if (forceFullBackup)
		console.AddLog(L("LOG_FORCE_FULL_BACKUP"));

	// 限制备份链长度
	bool forceFullBackupDueToLimit = false;
	if (config.backupMode == 2 && config.maxSmartBackupsPerFull > 0 && !forceFullBackup) {
		vector<filesystem::path> worldBackups;
		try {
			for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
				if (entry.is_regular_file()) {
					worldBackups.push_back(entry.path());
				}
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog(L("LOG_ERROR_SCAN_BACKUP_DIR"), e.what());
		}

		if (!worldBackups.empty()) {
			sort(worldBackups.begin(), worldBackups.end(), [](const auto& a, const auto& b) {
				return filesystem::last_write_time(a) < filesystem::last_write_time(b);
				});

			int smartCount = 0;
			bool fullFound = false;
			for (auto it = worldBackups.rbegin(); it != worldBackups.rend(); ++it) {
				wstring filename = it->filename().wstring();
				if (filename.find(L"[Full]") != wstring::npos) {
					fullFound = true;
					break;
				}
				if (filename.find(L"[Smart]") != wstring::npos) {
					smartCount++;
				}
			}

			if (fullFound && smartCount >= config.maxSmartBackupsPerFull) {
				forceFullBackupDueToLimit = true;
				console.AddLog(L("LOG_FORCE_FULL_BACKUP_LIMIT_REACHED"), config.maxSmartBackupsPerFull);
			}
		}
	}

	// 无论什么备份模式，都要获得状态，便于成功后更新状态
	vector<filesystem::path> filesToBackup = GetChangedFiles(sourcePath, metadataFolder);

	if (config.skipIfUnchanged && filesToBackup.empty()) {
		console.AddLog(L("LOG_NO_CHANGE_FOUND"));
		if (config.hotBackup && !sourcePath.empty()) {
			console.AddLog(L("LOG_CLEAN_SNAPSHOT"));
			error_code ec;
			filesystem::remove_all(sourcePath, ec);
			if (ec) console.AddLog(L("LOG_WARNING_CLEAN_SNAPSHOT"), ec.message().c_str());
		}
		return;
	}

	wstring backupTypeStr; // 用于历史记录

	if (config.backupMode == 1 || forceFullBackup) // 普通备份
	{
		backupTypeStr = L"Full";
		archivePath = destinationFolder + L"\\" + L"[Full][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
		command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
			L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" \"" + archivePath + L"\"" + L" \"" + sourcePath + L"\\*\"" + exclusion_args;
	}
	else if (config.backupMode == 2) // 智能备份
	{
		backupTypeStr = L"Smart";
		if (!config.blacklist.empty()) {
			filesToBackup.erase(
				remove_if(filesToBackup.begin(), filesToBackup.end(),
					[&](const filesystem::path& p) {
						for (const auto& blacklistedItemW : config.blacklist) {
							if (p.wstring().length() == blacklistedItemW.length() || p.wstring()[blacklistedItemW.length()] == L'\\') {
								return true;
							}
						}
						return false;
					}),
				filesToBackup.end()
			);
		}
		if (filesToBackup.empty()) {
			console.AddLog(L("LOG_NO_CHANGE_FOUND"));
			if (config.hotBackup) // 清理快照
				filesystem::remove_all(sourcePath);
			return; // 没有变化，直接返回
		}

		console.AddLog(L("LOG_BACKUP_SMART_INFO"), filesToBackup.size());

		// 7z 支持用 @文件名 的方式批量指定要压缩的文件。把所有要备份的文件路径写到一个文本文件避免超过cmd 8191限长
		filesystem::path tempDir = filesystem::temp_directory_path() / L"MineBackup_Snapshot";
		if (!filesystem::exists(tempDir))
			filesystem::create_directories(tempDir);
		wofstream ofs(tempDir.string() + "\\7z.txt");
		ofs.imbue(locale(ofs.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
		for (const auto& file : filesToBackup) {
			ofs << file.wstring().substr(originalSourcePath.size() + 1) << endl; // 输出相对路径，以及，必须utf8！
		}
		ofs.close();

		archivePath = destinationFolder + L"\\" + L"[Smart][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
		// 智能备份还未加入黑名单功能
		command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx="
			+ to_wstring(config.zipLevel) + L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" \"" + archivePath + L"\" @" + tempDir.wstring() + L"\\7z.txt";
	}
	else if (config.backupMode == 3) // 覆盖备份
	{
		backupTypeStr = L"Overwrite";
		console.AddLog(L("LOG_OVERWRITE"));
		filesystem::path latestBackupPath;
		auto latest_time = filesystem::file_time_type{}; // 默认构造就是最小时间点，不需要::min()
		bool found = false;

		for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
			if (entry.is_regular_file() && entry.path().extension().wstring() == L"." + config.zipFormat) {
				if (entry.last_write_time() > latest_time) {
					latest_time = entry.last_write_time();
					latestBackupPath = entry.path();
					found = true;
				}
			}
		}
		if (found) {
			console.AddLog(L("LOG_FOUND_LATEST"), wstring_to_utf8(latestBackupPath.filename().wstring()).c_str());
			command = L"\"" + config.zipPath + L"\" u \"" + latestBackupPath.wstring() + L"\" \"" + sourcePath + L"\\*\" -mx=" + to_wstring(config.zipLevel) + exclusion_args;
			archivePath = latestBackupPath.wstring(); // 记录被更新的文件
		}
		else {
			console.AddLog(L("LOG_NO_BACKUP_FOUND"));
			archivePath = destinationFolder + L"\\" + L"[Full][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
			command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
				L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" -spf \"" + archivePath + L"\"" + L" \"" + sourcePath + L"\\*\"" + exclusion_args;
			// -spf 强制使用完整路径，-spf2 使用相对路径
		}
	}
	// 在后台线程中执行命令
	if (RunCommandInBackground(command, console, config.useLowPriority, originalSourcePath)) // 工作目录不能丢！
	{
		console.AddLog(L("LOG_BACKUP_END_HEADER"));
		LimitBackupFiles(destinationFolder, config.keepCount, &console);
		SaveStateFile(metadataFolder);
		// 历史记录
		AddHistoryEntry(currentConfigIndex, world.first, filesystem::path(archivePath).filename().wstring(), backupTypeStr, comment);
		string payload = "event=backup_success;config=" + to_string(currentConfigIndex) + ";world=" + wstring_to_utf8(world.first) + ";file=" + wstring_to_utf8(filesystem::path(archivePath).filename().wstring());
		BroadcastEvent(payload);
	}
	else {
		BroadcastEvent("event=backup_failed;config=" + to_string(currentConfigIndex) + ";world=" + wstring_to_utf8(world.first) + ";error=command_failed");
	}


	filesystem::remove(filesystem::temp_directory_path() / L"MineBackup_Snapshot" / L"7z.txt");
	if (config.hotBackup && sourcePath != (config.saveRoot + L"\\" + world.first)) {
		console.AddLog(L("LOG_CLEAN_SNAPSHOT"));
		error_code ec;
		filesystem::remove_all(sourcePath, ec);
		if (ec) console.AddLog(L("LOG_WARNING_CLEAN_SNAPSHOT"), ec.message().c_str());
	}
}
void DoModsBackup(const Config config, const wstring& comment) {
	console.AddLog(L("LOG_BACKUP_MODS_START"));

	filesystem::path saveRoot(config.saveRoot);
	if (saveRoot.filename() != L"saves") {
		console.AddLog("[Warning] saveRoot does not point to a 'saves' folder. Guessing mods folder location.");
	}
	filesystem::path modsPath = saveRoot.parent_path() / "mods";

	if (!filesystem::exists(modsPath) || !filesystem::is_directory(modsPath)) {
		console.AddLog(L("LOG_ERROR_MODS_NOT_FOUND"), wstring_to_utf8(modsPath.wstring()).c_str());
		console.AddLog(L("LOG_BACKUP_MODS_END"));
		return;
	}

	filesystem::path destinationFolder = filesystem::path(config.backupPath) / L"_MODS_";
	wstring archiveNameBase = L"Mods";

	if (!comment.empty()) {
		archiveNameBase += L" [" + SanitizeFileName(comment) + L"]";
	}

	// Timestamp
	time_t now = time(0);
	tm ltm;
	localtime_s(&ltm, &now);
	wchar_t timeBuf[80];
	wcsftime(timeBuf, sizeof(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);
	wstring archivePath = destinationFolder.wstring() + L"\\" + L"[" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;

	try {
		filesystem::create_directories(destinationFolder);
		console.AddLog(L("LOG_BACKUP_DIR_IS"), wstring_to_utf8(destinationFolder.wstring()).c_str());
	}
	catch (const filesystem::filesystem_error& e) {
		console.AddLog(L("LOG_ERROR_CREATE_BACKUP_DIR"), e.what());
		console.AddLog(L("LOG_BACKUP_MODS_END"));
		return;
	}

	wstring command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
		L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" \"" + archivePath + L"\"" + L" \"" + modsPath.wstring() + L"\\*\"";

	if (RunCommandInBackground(command, console, config.useLowPriority)) {
		LimitBackupFiles(destinationFolder.wstring(), config.keepCount, &console);
		// 用特殊名字添加到历史
		AddHistoryEntry(currentConfigIndex, L"__MODS__", filesystem::path(archivePath).filename().wstring(), L"Mods", comment);
	}

	console.AddLog(L("LOG_BACKUP_MODS_END"));
}
void DoRestore2(const Config config, const wstring& worldName, const filesystem::path& fullBackupPath, Console& console, int restoreMethod) {
	console.AddLog(L("LOG_RESTORE_START_HEADER"));
	console.AddLog(L("LOG_RESTORE_PREPARE"), wstring_to_utf8(worldName).c_str());
	console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(fullBackupPath.wstring()).c_str());

	if (!filesystem::exists(config.zipPath)) {
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return;
	}

	wstring destinationFolder = config.saveRoot + L"\\" + worldName;

	if (restoreMethod == 0) { // Clean Restore
		console.AddLog(L("LOG_DELETING_EXISTING_WORLD"), wstring_to_utf8(destinationFolder).c_str());
		try {
			if (filesystem::exists(destinationFolder)) {
				filesystem::remove_all(destinationFolder);
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog("[ERROR] Failed to delete existing world folder: %s. Continuing with overwrite.", e.what());
		}
	}

	// For a manually selected file, we treat it as a single restore operation.
	// Smart Restore logic does not apply as we don't know the history.
	wstring command = L"\"" + config.zipPath + L"\" x \"" + fullBackupPath.wstring() + L"\" -o\"" + destinationFolder + L"\" -y";
	RunCommandInBackground(command, console, config.useLowPriority);

	console.AddLog(L("LOG_RESTORE_END_HEADER"));
}
void DoRestore(const Config config, const wstring& worldName, const wstring& backupFile, Console& console, int restoreMethod) {
	console.AddLog(L("LOG_RESTORE_START_HEADER"));
	console.AddLog(L("LOG_RESTORE_PREPARE"), wstring_to_utf8(worldName).c_str());
	console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(backupFile).c_str());

	// 检查7z.exe是否存在
	if (!filesystem::exists(config.zipPath)) {
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return;
	}

	// 准备路径
	wstring sourceDir = config.backupPath + L"\\" + worldName;
	wstring destinationFolder = config.saveRoot + L"\\" + worldName;

	if (restoreMethod == 0) { // Clean Restore
		console.AddLog(L("LOG_DELETING_EXISTING_WORLD"), wstring_to_utf8(destinationFolder).c_str());
		try {
			if (filesystem::exists(destinationFolder)) {
				filesystem::remove_all(destinationFolder);
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog("[ERROR] Failed to delete existing world folder: %s. Continuing with overwrite.", e.what());
		}
	}

	// 收集所有相关的备份文件
	vector<filesystem::path> backupsToApply;
	filesystem::path targetBackupPath = filesystem::path(sourceDir) / backupFile;

	// 如果目标是完整备份，直接还原它
	if (backupFile.find(L"[Smart]") != wstring::npos) { // 目标是增量备份
		// 寻找基础的完整备份
		filesystem::path baseFullBackup;
		auto baseFullTime = filesystem::file_time_type{};

		for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
			if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Full]") != wstring::npos) {
				if (entry.last_write_time() < filesystem::last_write_time(targetBackupPath) && entry.last_write_time() > baseFullTime) {
					baseFullTime = entry.last_write_time();
					baseFullBackup = entry.path();
				}
			}
		}

		if (baseFullBackup.empty()) {
			console.AddLog(L("LOG_BACKUP_SMART_NO_FOUND"));
			return;
		}

		console.AddLog(L("LOG_BACKUP_SMART_FOUND"), wstring_to_utf8(baseFullBackup.filename().wstring()).c_str());
		backupsToApply.push_back(baseFullBackup);

		// 收集从基础备份到目标备份之间的所有增量备份
		for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
			if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Smart]") != wstring::npos) {
				if (entry.last_write_time() > baseFullTime && entry.last_write_time() <= filesystem::last_write_time(targetBackupPath)) {
					backupsToApply.push_back(entry.path());
				}
			}
		}
	}
	else { //当成完整备份处理
		backupsToApply.push_back(targetBackupPath);
	}

	// 格式: "C:\7z.exe" x "源压缩包路径" -o"目标文件夹路径" -y
	// 'x' 表示带路径解压, '-o' 指定输出目录, '-y' 表示对所有提示回答“是”（例如覆盖文件）
	// 按时间顺序排序所有需要应用的备份
	sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
		return filesystem::last_write_time(a) < filesystem::last_write_time(b);
		});

	// 依次执行还原
	for (size_t i = 0; i < backupsToApply.size(); ++i) {
		const auto& backup = backupsToApply[i];
		console.AddLog(L("RESTORE_STEPS"), i + 1, backupsToApply.size(), wstring_to_utf8(backup.filename().wstring()).c_str());
		wstring command = L"\"" + config.zipPath + L"\" x \"" + backup.wstring() + L"\" -o\"" + destinationFolder + L"\" -y";
		RunCommandInBackground(command, console, config.useLowPriority);
	}
	console.AddLog(L("LOG_RESTORE_END_HEADER"));
	//BroadcastEvent("event=restore_success;config=" + to_string(currentConfigIndex) ...);
	return;
}

void AutoBackupThreadFunction(int worldIdx, int configIdx, int intervalMinutes, Console* console) {
	console->AddLog(L("LOG_AUTOBACKUP_START"), worldIdx, intervalMinutes);
	while (true) {
		// 等待指定的时间，但每秒检查一次是否需要停止
		for (int i = 0; i < intervalMinutes * 60; ++i) {
			// 安全地检查停止标志
			if (g_active_auto_backups.at(worldIdx).stop_flag) {
				console->AddLog(L("LOG_AUTOBACKUP_STOPPED"), worldIdx);
				return; // 结束线程
			}
			this_thread::sleep_for(chrono::seconds(1));
		}

		// 时间到了，开始备份
		console->AddLog(L("LOG_AUTOBACKUP_ROUTINE"), worldIdx);
		// 直接调用已经存在的 DoBackup 函数
		DoBackup(configs[configIdx], configs[configIdx].worlds[worldIdx], *console);
	}
}

void RunSpecialMode(int configId) {
	SpecialConfig spCfg;
	if (specialConfigs.count(configId)) {
		spCfg = specialConfigs[configId];
	}
	else {
		ConsoleLog(L("SPECIAL_CONFIG_NOT_FOUND"), configId);
		Sleep(3000);
		return;
	}

	// 隐藏控制台窗口（如果配置要求）
	if (spCfg.hideWindow) {
		ShowWindow(GetConsoleWindow(), SW_HIDE);
	}

	// 设置控制台标题和头部信息
	system(("title MineBackup - Automated Task: " + spCfg.name).c_str());
	ConsoleLog(L("AUTOMATED_TASK_RUNNER_HEADER"));
	ConsoleLog(L("EXECUTING_CONFIG_NAME"), spCfg.name.c_str());
	ConsoleLog("----------------------------------------------");
	if (!spCfg.hideWindow) {
		ConsoleLog(L("CONSOLE_QUIT_PROMPT"));
		ConsoleLog("----------------------------------------------");
	}

	atomic<bool> shouldExit = false;
	vector<thread> taskThreads;
	static Console dummyConsole; // 用于传递给 DoBackup

	// --- 1. 执行一次性命令 ---
	for (const auto& cmd : spCfg.commands) {
		ConsoleLog(L("LOG_CMD_EXECUTING"), wstring_to_utf8(cmd).c_str());
		system(utf8_to_gbk(wstring_to_utf8(cmd)).c_str()); // 使用 system 简化实现
	}

	// --- 2. 处理并启动所有自动备份任务 ---
	for (const auto& task : spCfg.tasks) {
		if (!configs.count(task.configIndex) ||
			task.worldIndex < 0 ||
			task.worldIndex >= configs[task.configIndex].worlds.size())
		{
			ConsoleLog(L("ERROR_INVALID_WORLD_IN_TASK"), task.configIndex, task.worldIndex);
			continue;
		}

		// 创建任务专用配置（合并基础配置和特殊设置）
		Config taskConfig = configs[task.configIndex];
		const auto& worldData = taskConfig.worlds[task.worldIndex];
		taskConfig.hotBackup = spCfg.hotBackup;
		taskConfig.zipLevel = spCfg.zipLevel;
		taskConfig.keepCount = spCfg.keepCount;
		taskConfig.cpuThreads = spCfg.cpuThreads;
		taskConfig.useLowPriority = spCfg.useLowPriority;
		//taskConfig.blacklist = spCfg.blacklist; 沿用普通配置的黑名单

		if (task.backupType == 0) { // 类型 0: 一次性备份
			ConsoleLog(L("TASK_QUEUE_ONETIME_BACKUP"), utf8_to_gbk(wstring_to_utf8(worldData.first)).c_str());
			DoBackup(taskConfig, worldData, dummyConsole);
		}
		else { // 类型 1 (间隔) 和 2 (计划) 在后台线程运行
			taskThreads.emplace_back([task, taskConfig, worldData, &shouldExit]() {
				ConsoleLog(L("THREAD_STARTED_FOR_WORLD"), utf8_to_gbk(wstring_to_utf8(worldData.first)).c_str());

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
						ConsoleLog(L("SCHEDULE_NEXT_BACKUP_AT"), utf8_to_gbk(wstring_to_utf8(worldData.first).c_str()), time_buf);

						// 等待直到目标时间，同时检查退出信号
						while (time(nullptr) < next_run_t && !shouldExit) {
							this_thread::sleep_for(chrono::seconds(1));
						}
					}

					if (shouldExit) break;

					ConsoleLog(L("BACKUP_PERFORMING_FOR_WORLD"), utf8_to_gbk(wstring_to_utf8(worldData.first).c_str()));
					DoBackup(taskConfig, worldData, dummyConsole);
				}
				ConsoleLog(L("THREAD_STOPPED_FOR_WORLD"), utf8_to_gbk(wstring_to_utf8(worldData.first).c_str()));
				});
		}
	}

	ConsoleLog(L("INFO_TASKS_INITIATED"));

	// --- 3. 用户输入主循环（如果控制台可见）---
	while (!shouldExit) {
		if (!spCfg.hideWindow && _kbhit()) {
			char c = tolower(_getch());
			if (c == 'q') {
				shouldExit = true;
				ConsoleLog(L("INFO_QUIT_SIGNAL_RECEIVED"));
			}
			else if (c == 'm') {
				shouldExit = true;
				specialConfigs[configId].autoExecute = false;
				SaveConfigs();
				ConsoleLog(L("INFO_SWITCHING_TO_GUI_MODE"));
				wchar_t selfPath[MAX_PATH];
				GetModuleFileNameW(NULL, selfPath, MAX_PATH); // 获得程序路径
				ShellExecuteW(NULL, L"open", selfPath, NULL, NULL, SW_SHOWNORMAL); // 开启
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

	ConsoleLog(L("INFO_ALL_TASKS_SHUT_DOWN"));
	return;
}

void CheckForConfigConflicts() {
	map<wstring, vector<pair<int, wstring>>> worldMap; // Key: World Name, Value: {ConfigIndex, BackupPath}

	for (const auto& conf_pair : configs) {
		int config_idx = conf_pair.first;
		const Config& cfg = conf_pair.second;
		for (const auto& world_pair : cfg.worlds) {
			const wstring& worldName = world_pair.first;
			worldMap[worldName].push_back({ config_idx, cfg.backupPath });
		}
	}

	wstring conflictDetails = L"";
	bool ifConf = false;

	for (const auto& map_pair : worldMap) {
		const vector<pair<int, wstring>>& entries = map_pair.second;
		if (entries.size() > 1) { // 如果有多个配置使用同一个世界名
			for (size_t i = 0; i < entries.size(); ++i) {
				for (size_t j = i + 1; j < entries.size(); ++j) { // 比较每对配置
					if (entries[i].second == entries[j].second && !entries[i].second.empty()) {
						ifConf = true;
						break;
						//wchar_t buffer[512];
						/*swprintf_s(buffer, 50, L"%d plus %d is %d", 10, 20, (10 + 20));
						swprintf_s(buffer, CONSTANT1, L(L("CONFIG_CONFLICT_ENTRY")),
							entries[i].first,
							entries[j].first,
							map_pair.first.c_str(),
							entries[i].second.c_str());*/
							//conflictDetails += buffer;
					}
				}
			}
			if (ifConf)
				break;
		}
	}
	if (ifConf) {
		wchar_t finalMessage[CONSTANT1];
		//strncpy_s(finalMessage, L("CONFIG_CONFLICT_MESSAGE"),100);
		swprintf_s(finalMessage, CONSTANT1, utf8_to_wstring(L("CONFIG_CONFLICT_MESSAGE")).c_str());
		MessageBoxW(nullptr, finalMessage, utf8_to_wstring(L("CONFIG_CONFLICT_TITLE")).c_str(), MB_OK | MB_ICONWARNING);
	}

}
void ShowHistoryWindow() {
	static bool history_restore = false;

	ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(L("HISTORY_WINDOW_TITLE"), &showHistoryWindow)) {
		ImGui::End();
		return;
	}

	// 使用常量指针来确保数据不会被意外修改
	static const HistoryEntry* selected_entry_for_restore = nullptr;

	if (configs.find(currentConfigIndex) == configs.end()) {
		ImGui::Text(L("ERROR_NO_CONFIG_SELECTED"));
		ImGui::End();
		return;
	}

	Config& cfg = configs[currentConfigIndex];
	ImGui::Text(L("HISTORY_FOR_CONFIG"), cfg.name.c_str());
	ImGui::Separator();

	if (g_history.find(currentConfigIndex) == g_history.end() || g_history[currentConfigIndex].empty()) {
		ImGui::Text(L("HISTORY_EMPTY"));
	}
	else {
		ImGui::BeginChild("HistoryScroll", ImVec2(0, 0), true);

		map<wstring, vector<const HistoryEntry*>> worldHistory;
		if (g_history.count(currentConfigIndex)) {
			const vector<HistoryEntry>& history_vec = g_history.at(currentConfigIndex);
			for (const auto& entry : history_vec) {
				worldHistory[entry.worldName].push_back(&entry);
			}
		}

		for (const auto& pair : worldHistory) {
			// The TreeNode already pushes the world name to the ID stack, ensuring uniqueness for the content inside.
			if (ImGui::TreeNode(wstring_to_utf8(pair.first).c_str())) {

				for (const HistoryEntry* entry : pair.second) {
					// Use the stable pointer address as a unique and stable ID for this widget.
					ImGui::PushID(entry);

					ImVec2 start_cursor_pos = ImGui::GetCursorPos(); // Record start position for the invisible button.

					ImGui::BeginGroup();
					ImDrawList* drawList = ImGui::GetWindowDrawList();
					const float node_radius = 5.0f;
					const float row_height = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
					ImVec2 p = ImGui::GetCursorScreenPos();
					drawList->AddLine(ImVec2(p.x + node_radius, p.y - 1), ImVec2(p.x + node_radius, p.y + row_height), ImGui::GetColorU32(ImGuiCol_TextDisabled), 1.5f);
					drawList->AddCircleFilled(ImVec2(p.x + node_radius, p.y + row_height * 0.5f), node_radius, ImGui::GetColorU32(ImGuiCol_Button));
					ImGui::Dummy(ImVec2(node_radius * 3.0f, 0)); // Spacer
					ImGui::EndGroup();

					ImGui::SameLine();

					// Text Content
					ImGui::BeginGroup();
					ImGui::TextColored(ImVec4(0.839f, 0.616f, 0.490f, 1.0f), "[%s]", wstring_to_utf8(entry->backupType).c_str());
					ImGui::SameLine();
					if (!entry->comment.empty()) {
						ImGui::TextWrapped("%s", wstring_to_utf8(entry->comment).c_str());
					}
					else {
						ImGui::TextDisabled(L("HISTORY_NO_COMMENT"));
					}
					ImGui::TextDisabled("%s | %s", wstring_to_utf8(entry->timestamp_str).c_str(), wstring_to_utf8(entry->backupFile).c_str());
					ImGui::EndGroup();

					// 在自定义绘制的控件上方创建一个整行不可见的按钮
					ImVec2 end_cursor_pos = ImGui::GetCursorPos();
					ImGui::SetCursorPos(start_cursor_pos); // Reset cursor to the start of the row
					if (ImGui::InvisibleButton("##history_item", ImVec2(ImGui::GetContentRegionAvail().x, row_height))) {
						selected_entry_for_restore = entry;
						history_restore = true;
						//ImGui::OpenPopup(L("CONFIRM_RESTORE_HISTORY_TITLE")); 这里不能直接弄，很奇怪
					}

					// 悬浮效果
					if (ImGui::IsItemHovered()) {
						ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_FrameBgHovered, 0.5f), 4.0f);
					}

					ImGui::SetCursorPos(end_cursor_pos); // Restore cursor position after the button.
					ImGui::PopID();
				}
				ImGui::TreePop();
			}
		}
		ImGui::EndChild();
	}
	if(history_restore)
		ImGui::OpenPopup(L("CONFIRM_RESTORE_HISTORY_TITLE"));
	if (ImGui::BeginPopupModal(L("CONFIRM_RESTORE_HISTORY_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		
		if (selected_entry_for_restore) {
			ImGui::Text(L("CONFIRM_RESTORE_HISTORY_MSG"), wstring_to_utf8(selected_entry_for_restore->backupFile).c_str(), wstring_to_utf8(selected_entry_for_restore->worldName).c_str());
			ImGui::Separator();

			if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
				thread restore_thread(DoRestore, cfg, selected_entry_for_restore->worldName, selected_entry_for_restore->backupFile, ref(console), 0);
				restore_thread.detach();
				ImGui::CloseCurrentPopup();
				selected_entry_for_restore = nullptr;
			}
			ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
				ImGui::CloseCurrentPopup();
				selected_entry_for_restore = nullptr;
			}
		}
		history_restore = false;
		ImGui::EndPopup();
	}

	ImGui::End();
}

string ProcessCommand(const string& commandStr, Console* console) {
	stringstream ss(commandStr);
	string command;
	ss >> command;

	auto error_response = [&](const string& msg) {
		BroadcastEvent(L("KNOTLINK_COMMAND_ERROR") + msg);
		console->AddLog(L("KNOTLINK_COMMAND_ERROR"), command.c_str(), msg.c_str());
		return "ERROR:" + msg;
		};

	// 使用 lock_guard 确保在函数作用域内访问 configs 是线程安全的
	lock_guard<mutex> lock(g_configsMutex);

	if (command == "LIST_CONFIGS") {
		string result = "OK:";
		for (const auto& pair : configs) {
			result += to_string(pair.first) + "," + pair.second.name + ";";
		}
		if (!result.empty()) result.pop_back(); // 移除最后的';'
		BroadcastEvent("event=list_configs;data=" + result);
		return result;
	}
	else if (command == "LIST_WORLDS") {
		int config_idx, world_idx;
		if (!(ss >> config_idx) || configs.find(config_idx) == configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return error_response(L("BROADCAST_CONFIG_INDEX_ERROR"));
		}
		if (!(ss >> world_idx) || world_idx >= configs[config_idx].worlds.size() || world_idx < 0) {
			BroadcastEvent(L("BROADCAST_WORLD_INDEX_ERROR"));
			return error_response(L("BROADCAST_WORLD_INDEX_ERROR"));
		}
		const auto& cfg = configs[config_idx];
		wstring backupDir = cfg.backupPath + L"\\" + cfg.worlds[world_idx].first;
		string result = "OK:";
		if (filesystem::exists(backupDir)) {
			for (const auto& entry : filesystem::directory_iterator(backupDir)) {
				if (entry.is_regular_file()) {
					result += wstring_to_utf8(entry.path().filename().wstring()) + ";";
				}
			}
		}
		if (result.back() == ';') result.pop_back();
		BroadcastEvent("event=list_worlds;config=" + to_string(config_idx) + ";world=" + to_string(world_idx) + ";data=" + result);
		return result;
	}
	else if (command == "GET_CONFIG") {
		int config_idx;
		if (!(ss >> config_idx) || configs.find(config_idx) == configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return L("BROADCAST_CONFIG_INDEX_ERROR");
		}
		const auto& cfg = configs[config_idx];
		BroadcastEvent("event=get_config;config=" + to_string(config_idx) + ";name=" + cfg.name +
			";backup_mode=" + to_string(cfg.backupMode) + ";hot_backup=" + (cfg.hotBackup ? "true" : "false") +
			";keep_count=" + to_string(cfg.keepCount));
		return "OK:name=" + cfg.name + ";backup_mode=" + to_string(cfg.backupMode) +
			";hot_backup=" + (cfg.hotBackup ? "true" : "false") + ";keep_count=" + to_string(cfg.keepCount);
	}
	else if (command == "SET_CONFIG") {
		int config_idx;
		string key, value;

		if (!(ss >> config_idx >> key >> value) || configs.find(config_idx) == configs.end()) {
			BroadcastEvent("ERROR:Invalid arguments. Usage: SET_CONFIG <config_idx> <key> <value>");
			return "ERROR:Invalid arguments.Usage : SET_CONFIG <config_idx> <key> <value>";
		}
		auto& cfg = configs[config_idx];
		string response_msg = "OK:Set " + key + " to " + value;

		if (key == "backup_mode") cfg.backupMode = stoi(value);
		else if (key == "hot_backup") cfg.hotBackup = (value == "true");
		else return "ERROR:Unknown key '" + key + "'.";

		SaveConfigs(); // 保存更改
		BroadcastEvent("event=config_changed;config=" + to_string(config_idx) + ";key=" + key + ";value=" + value);
		BroadcastEvent(response_msg);
		return response_msg;
	}
	else if (command == "BACKUP") {
		int config_idx, world_idx;
		string comment_part;
		if (!(ss >> config_idx >> world_idx) || configs.find(config_idx) == configs.end() || world_idx >= configs[config_idx].worlds.size()) {
			BroadcastEvent("ERROR:Invalid arguments. Usage: BACKUP <config_idx> <world_idx> [comment]");
			return "ERROR:Invalid arguments. Usage: BACKUP <config_idx> <world_idx> [comment]";
		}
		getline(ss, comment_part); // 获取剩余部分作为注释
		if (!comment_part.empty() && comment_part.front() == ' ') comment_part.erase(0, 1); // 去除前导空格

		// 在后台线程中执行备份，避免阻塞命令处理器
		thread([=]() {
			// 在新线程中再次加锁，因为 configs 可能在主线程中被修改
			lock_guard<mutex> thread_lock(g_configsMutex);
			if (configs.count(config_idx)) // 确保配置仍然存在
				DoBackup(configs[config_idx], configs[config_idx].worlds[world_idx], *console, utf8_to_wstring(comment_part));
			}).detach();
		BroadcastEvent("event=backup_started;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(configs[config_idx].worlds[world_idx].first));
		return "OK:Backup started for world '" + wstring_to_utf8(configs[config_idx].worlds[world_idx].first) + "'";
	}
	else if (command == "RESTORE") {
		int config_idx, world_idx;
		string backup_file;
		if (!(ss >> config_idx) || configs.find(config_idx) == configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return error_response(L("BROADCAST_CONFIG_INDEX_ERROR"));
		}
		if (!(ss >> world_idx) || world_idx >= configs[config_idx].worlds.size() || world_idx < 0) {
			BroadcastEvent(L("BROADCAST_WORLD_INDEX_ERROR"));
			return error_response(L("BROADCAST_WORLD_INDEX_ERROR"));
		}
		if (!(ss >> backup_file)) {
			BroadcastEvent(L("BROADCAST_MISSING_BACKUP_FILE"));
			return error_response(L("BROADCAST_MISSING_BACKUP_FILE"));
		}

		// In a background thread to avoid blocking
		thread([=]() {
			lock_guard<mutex> thread_lock(g_configsMutex);
			if (configs.count(config_idx)) {
				// Default to clean restore (method 0) for remote commands for safety
				DoRestore(configs[config_idx], configs[config_idx].worlds[world_idx].first, utf8_to_wstring(backup_file), *console, 0);
			}
			}).detach();

		console->AddLog(L("KNOTLINK_COMMAND_SUCCESS"), command.c_str());
		BroadcastEvent("event=restore_started;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(configs[config_idx].worlds[world_idx].first));
		return "OK:Restore started for world '" + wstring_to_utf8(configs[config_idx].worlds[world_idx].first) + "'";
	}
	else if (command == "BACKUP_MODS") {
		int config_idx;
		string comment_part;
		if (!(ss >> config_idx) || configs.find(config_idx) == configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return error_response(L("BROADCAST_CONFIG_INDEX_ERROR"));
		}
		getline(ss, comment_part); // Get rest of the line as comment
		if (!comment_part.empty() && comment_part.front() == ' ') comment_part.erase(0, 1);

		thread([=]() {
			lock_guard<mutex> thread_lock(g_configsMutex);
			if (configs.count(config_idx)) {
				DoModsBackup(configs[config_idx], utf8_to_wstring(comment_part));
			}
			}).detach();
		BroadcastEvent("event=mods_backup_started;config=" + to_string(config_idx));
		console->AddLog(L("KNOTLINK_COMMAND_SUCCESS"), command.c_str());
		return "OK:Mods backup started.";
	}

	return "ERROR:Unknown command '" + command + "'.";
}

void ConsoleLog(const char* format, ...) {
	lock_guard<mutex> lock(consoleMutex);
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	printf("\n");
	va_end(args);
}

void TriggerHotkeyBackup() {
	console.AddLog(L("LOG_HOTKEY_BACKUP_TRIGGERED"));
	lock_guard<mutex> lock(g_configsMutex);

	for (const auto& config_pair : configs) {
		int config_idx = config_pair.first;
		const Config& cfg = config_pair.second;

		for (int world_idx = 0; world_idx < cfg.worlds.size(); ++world_idx) {
			const auto& world = cfg.worlds[world_idx];
			wstring levelDatPath = cfg.saveRoot + L"\\" + world.first + L"\\session.lock";
			if(!filesystem::exists(levelDatPath)) { // 没有 session.lock 文件，可能是基岩版存档，需要遍历db文件夹下的所有文件看看有没有被锁定的
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

				string payload = "event=pre_hot_backup;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(world.first);
				BroadcastEvent(payload);
				BroadcastEvent("minebackup save");
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

void ExitWatcherThreadFunction() {
	console.AddLog(L("LOG_EXIT_WATCHER_START"));

	while (!g_stopExitWatcher) {
		map<pair<int, int>, wstring> currently_locked_worlds;

		{
			lock_guard<mutex> lock(g_configsMutex);
			
			for (const auto& config_pair : configs) {
				const Config& cfg = config_pair.second;
				if (!cfg.backupOnGameExit) continue;
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
			
			for (const auto& sp_config_pair : specialConfigs) {
				const SpecialConfig& sp_cfg = sp_config_pair.second;
				if (!sp_cfg.backupOnGameExit) continue;
				for (const auto& task : sp_cfg.tasks) {
					if (configs.count(task.configIndex) && task.worldIndex < configs[task.configIndex].worlds.size()) {
						const Config& base_cfg = configs[task.configIndex];
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
			lock_guard<mutex> lock(g_activeWorldsMutex);

			// 检查新启动的世界
			for (const auto& locked_pair : currently_locked_worlds) {
				if (g_activeWorlds.find(locked_pair.first) == g_activeWorlds.end()) {
					console.AddLog(L("LOG_GAME_SESSION_STARTED"), wstring_to_utf8(locked_pair.second).c_str());
					string payload = "event=game_session_start;config=" + to_string(locked_pair.first.first) + ";world=" + wstring_to_utf8(locked_pair.second);
					BroadcastEvent(payload);
				}
			}

			// 检查已关闭的世界
			vector<pair<int, int>> worlds_to_backup;
			for (const auto& active_pair : g_activeWorlds) {
				if (currently_locked_worlds.find(active_pair.first) == currently_locked_worlds.end()) {
					console.AddLog(L("LOG_GAME_SESSION_ENDED"), wstring_to_utf8(active_pair.second).c_str());
					string payload = "event=game_session_end;config=" + to_string(active_pair.first.first) + ";world=" + wstring_to_utf8(active_pair.second);
					BroadcastEvent(payload);
					worlds_to_backup.push_back(active_pair.first);
				}
			}

			// 更新当前活动的世界列表
			g_activeWorlds = currently_locked_worlds;

			// 对于刚关闭的世界，启动备份
			if (!worlds_to_backup.empty()) {
				lock_guard<mutex> config_lock(g_configsMutex);
				for (const auto& backup_target : worlds_to_backup) {
					int config_idx = backup_target.first;
					int world_idx = backup_target.second;
					if (configs.count(config_idx) && world_idx < configs[config_idx].worlds.size()) {
						Config backupConfig = configs[config_idx];
						backupConfig.hotBackup = true; // 必须热备份
						thread backup_thread(DoBackup, backupConfig, backupConfig.worlds[world_idx], ref(console), L"OnExit");
						backup_thread.detach();
					}
				}
			}
		}

		this_thread::sleep_for(chrono::seconds(10));
	}
	console.AddLog(L("LOG_EXIT_WATCHER_STOP"));
}