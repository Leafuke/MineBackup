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
#include <atomic> // �����̰߳�ȫ�ı�־
#include <mutex>  // ���ڻ�����
#include <shellapi.h> // ����ͼ�����
#include <conio.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
using namespace std;
constexpr int CONSTANT1 = 256;
constexpr int CONSTANT2 = 512;
constexpr int MINEBACKUP_HOTKEY_ID = 1;
constexpr int MINERESTORE_HOTKEY_ID = 2;

static vector<int> worldIconWidths, worldIconHeights;
static atomic<bool> g_UpdateCheckDone(false);
static atomic<bool> g_NewVersionAvailable(false);
static string g_LatestVersionStr;
static string g_ReleaseNotes;
const string CURRENT_VERSION = "1.9.0";

// �ṹ����
struct Config {
	int backupMode;
	wstring saveRoot;
	vector<pair<wstring, wstring>> worlds; // {name, desc}
	wstring backupPath;
	wstring zipPath;
	wstring zipFormat = L"7z";
	wstring fontPath;
	wstring zipMethod = L"LZMA2";
	int zipLevel;
	int keepCount;
	bool hotBackup;
	bool backupBefore;
	int theme = 1;
	int folderNameType = 0;
	string name;
	int cpuThreads = 0; // 0 for auto/default
	bool useLowPriority = false;
	bool skipIfUnchanged = true;
	int maxSmartBackupsPerFull = 5;
	bool backupOnGameStart = false;
	vector<wstring> blacklist;
	bool cloudSyncEnabled = false;
	wstring rclonePath;
	wstring rcloneRemotePath;
	wstring snapshotPath;
	wstring othersPath;
};
struct AutomatedTask {
	int configIndex = -1;
	int worldIndex = -1;
	int backupType = 0; // 0: ����, 1: ���, 2: �ƻ�
	int intervalMinutes = 15;
	int schedMonth = 0, schedDay = 0, schedHour = 0, schedMinute = 0; // 0 ��ζ�š�ÿһ��
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
	int theme = 1;
	bool useLowPriority = true;
	bool hotBackup = false;
	vector<wstring> blacklist;
	bool runOnStartup = false;
	bool hideWindow = false;
	bool backupOnGameStart = false;
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
	atomic<bool> stop_flag{ false }; // ԭ�Ӳ���ֵ�����ڰ�ȫ��֪ͨ�߳�ֹͣ
};
struct DisplayWorld { // һ���µĽṹ�壬�� UI ����ֱ�Ӷ�ȡ configs[currentConfigIndex].worlds����ʹ�� DisplayWorld
	wstring name;      // ���������ļ�������
	wstring desc;      // ����
	int baseConfigIndex = -1; // ��Դ���� id
	int baseWorldIndex = -1;  // ��Դ��������������
	Config effectiveConfig;   // �ϲ�������ã�������
};

// KnotLink ʵ��ָ��
SignalSender* g_signalSender = nullptr;
OpenSocketResponser* g_commandResponser = nullptr;

static mutex g_configsMutex;			// ���ڱ���ȫ�����õĻ�����
static mutex consoleMutex;				// ����̨ģʽ����
static mutex g_task_mutex;		// ר�����ڱ��� g_active_auto_backups
static mutex g_activeWorldsMutex;

// �����������ȫ�֣�
ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
wstring Fontss;
vector<wstring> savePaths = { L"" };
wchar_t backupPath[CONSTANT1] = L"", zipPath[CONSTANT1] = L"";
bool done = false, showMainApp = false;
bool showSettings = false;
bool isSilence = false;
bool isRespond = false;
bool specialSetting = false;
bool g_CheckForUpdates = true, g_RunOnStartup = false;
static bool showHistoryWindow = false;
static bool specialConfigMode = false; // ����������UI
static bool g_enableKnotLink = true;
int currentConfigIndex = 1, realConfigIndex = -1; // ���realConfigIndex��Ϊ-1��˵������������
static int nextConfigId = 2; // �� 2 ��ʼ����Ϊ 1 ����ռ��
map<int, Config> configs;
map<int, vector<HistoryEntry>> g_history;
map<int, SpecialConfig> specialConfigs;
static atomic<bool> specialTasksRunning = false;
static atomic<bool> specialTasksComplete = false;
static map<pair<int, int>, AutoBackupTask> g_active_auto_backups; // Key: {configIdx, worldIdx}
static thread g_exitWatcherThread;
static atomic<bool> g_stopExitWatcher(false);
static map<pair<int, int>, wstring> g_activeWorlds; // Key: {configIdx, worldIdx}, Value: worldName
static wstring g_worldToFocusInHistory = L"";
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

// ����
NOTIFYICONDATA nid = { 0 };

// ������������
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
void RemoveHistoryEntry(int configIndex, const wstring& backupFileToRemove);

void BroadcastEvent(const string& eventPayload); // KnotLink �㲥
const char* L(const char* key);
inline void ApplyTheme(int& theme);
string find_json_value(const string& json, const string& key);
wstring SanitizeFileName(const wstring& input);
void CheckForUpdatesThread();
void SetAutoStart(const string& appName, const wstring& appPath, bool configType, int& configId, bool& enable);
//bool LoadTextureFromFile(const char* filename, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height);
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
vector<filesystem::path> GetChangedFiles(const filesystem::path& worldPath, const filesystem::path& metadataPath, const filesystem::path& backupPath, BackupCheckResult& out_result, map<wstring, size_t>& out_currentState);
size_t CalculateFileHash(const filesystem::path& filepath);
string GetRegistryValue(const string& keyPath, const string& valueName);
wstring GetLastOpenTime(const wstring& worldPath);
wstring GetLastBackupTime(const wstring& backupDir);

void UpdateMetadataFile(const filesystem::path& metadataPath, const wstring& newBackupFile, const wstring& basedOnBackupFile, const map<wstring, size_t>& currentState);
static void LoadConfigs(const string& filename = "config.ini");
static void SaveConfigs(const wstring& filename = L"config.ini");

void ShowSettingsWindow();
void ShowHistoryWindow(int& tempCurrentConfigIndex);
static vector<DisplayWorld> BuildDisplayWorldsForSelection();
static int CreateNewNormalConfig(const string& name_hint = "New Config");
static int CreateNewSpecialConfig(const string& name_hint = "New Special");

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
	mutex            logMutex; //���������ڱ�����־���ݵĻ����� 

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
		Commands.push_back("LIST_BACKUPS");
		Commands.push_back("LIST_WORLDS");
		Commands.push_back("LIST_CONFIGS");
		AutoScroll = true;                  //�Զ�������ѽ
		ScrollToBottom = false;             //���ù�����������������
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
		lock_guard<mutex> lock(logMutex);//����
		for (int i = 0; i < Items.Size; i++)
			ImGui::MemFree(Items[i]);
		Items.clear();
	}

	//��ʾ��Ϣ
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
		// Here we create a context menu only available from the title bar.(��ʱ����
		/*if (ImGui::BeginPopupContextItem())
		{
			if (ImGui::MenuItem(u8"�ر�"))
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
				if (ImGui::Selectable("���")) ClearLog();
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
				else if (strncmp(item, u8"[��ʾ] ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
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
			strcpy_s(s, strlen(s) + 1, "");//��Ҫ���strcpy�ĳ�strcpy_s�������м�Ҫ�Ӹ����Ȳ����Ų�������
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
		ImGui::SameLine();
		if (ImGui::SmallButton(L("BUTTON_EXPORT_LOG"))) {
			ofstream out("console_log.txt", ios::out | ios::trunc);
			if (!out.is_open()) return;
			for (int i = 0; i < Items.Size; ++i)
			{
				out << Items[i] << endl;
			}
			out.close();
			// �Զ�����־����Ŀ¼ ��ѡ�и��ļ�
			wstring cmd = L"/select,\"console_log.txt\"";
			ShellExecuteW(NULL, L"open", L"explorer.exe", cmd.c_str(), NULL, SW_SHOWNORMAL);
		}
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
				else if (strncmp(item, u8"[��ʾ] ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
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
			strcpy_s(s, strlen(s) + 1, "");//��Ҫ���strcpy�ĳ�strcpy_s�������м�Ҫ�Ӹ����Ȳ����Ų�������
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
				// ��Tab�Զ�ɸѡ
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
wstring CreateWorldSnapshot(const filesystem::path& worldPath, const wstring& snapshotPath, Console& console);
void DoBackup(const Config config, const pair<wstring, wstring> world, Console& console, const wstring& comment = L"");
void DoRestore2(const Config config, const wstring& worldName, const filesystem::path& fullBackupPath, Console& console, int restoreMethod);
void DoRestore(const Config config, const wstring& worldName, const wstring& backupFile, Console& console, int restoreMethod, const string& customRestoreList = "");
void DoOthersBackup(const Config config, filesystem::path backupWhat, const wstring& comment);
void DoDeleteBackup(const Config& config, const HistoryEntry& entryToDelete, Console& console);
void DoExportForSharing(Config tempConfig, wstring worldName, wstring worldPath, wstring outputPath, wstring description, Console& console);
void AutoBackupThreadFunction(int configIdx, int worldIdx, int intervalMinutes, Console* console, atomic<bool>& stop_flag);
void RunSpecialMode(int configId);
void CheckForConfigConflicts();
void ConsoleLog(Console* console, const char* format, ...);

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
	// ���õ�ǰ����Ŀ¼Ϊ��ִ���ļ�����Ŀ¼�����⿪������Ѱ��config����
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(NULL, exePath, MAX_PATH);
	SetCurrentDirectoryW(filesystem::path(exePath).parent_path().c_str());

	//_setmode(_fileno(stdout), _O_U8TEXT);
	//_setmode(_fileno(stdin), _O_U8TEXT);
	//CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	//ImGui_ImplWin32_EnableDpiAwareness(); ����win32���еģ�����Ǩ�Ƶ�glfw����Ҫ����ʵ��


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


	// Create window with graphics context
	float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor()); // Valid on GLFW 3.3+ only
	GLFWwindow* wc = glfwCreateWindow((int)(1280 * main_scale), (int)(800 * main_scale), "MineBackup", nullptr, nullptr);
	if (wc == nullptr)
		return 1;
	glfwMakeContextCurrent(wc);
	glfwSwapInterval(1); // Enable vsync

	// �������ش���
	//HWND hwnd = CreateWindowEx(0, L"STATIC", L"HotkeyWnd", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);

	//HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"MineBackup - v1.9.0", WS_OVERLAPPEDWINDOW, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, wc.hInstance, nullptr);
	//HWND hwnd2 = ::CreateWindowW(wc.lpszClassName, L"MineBackup", WS_OVERLAPPEDWINDOW, 100, 100, 1000, 1000, nullptr, nullptr, wc.hInstance, nullptr);
	// ע���ȼ���Alt + Ctrl + S
	//::RegisterHotKey(hwnd, MINEBACKUP_HOTKEY_ID, MOD_ALT | MOD_CONTROL, 'S');
	//::RegisterHotKey(hwnd, MINERESTORE_HOTKEY_ID, MOD_ALT | MOD_CONTROL, 'Z');
	

	// Show the window
	//::ShowWindow(hwnd, SW_SHOW);
	//::ShowWindow(wc, SW_HIDE);
	//::UpdateWindow(hwnd);
	//::ShowWindow(hwnd2, SW_HIDE);
	//::UpdateWindow(hwnd2);

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
	//����������ͼ��
	HICON hIcon = (HICON)LoadImage(
		GetModuleHandle(NULL),
		MAKEINTRESOURCE(IDI_ICON3),
		IMAGE_ICON,
		0, 0,
		LR_DEFAULTSIZE
	);

	if (hIcon) {
		// ���ô�ͼ�꣨������/Alt+Tab��
		//SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
		// ����Сͼ�꣨���ڱ�������
		//SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
	}

	//float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
	//io.FontGlobalScale = dpi_scale;

	bool errorShow = false;
	bool isFirstRun = !filesystem::exists("config.ini");
	static bool showConfigWizard = isFirstRun;
	showMainApp = !isFirstRun;
	ImGui::StyleColorsLight();//Ĭ����ɫ
	//LoadConfigs("config.ini"); 
	if (configs.count(currentConfigIndex))
		ApplyTheme(configs[currentConfigIndex].theme); // ��������ط���������
	else
		ApplyTheme(specialConfigs[currentConfigIndex].theme);

	// Ϊ�������һ������ͼ��

	

	// �������ͼ�굽ϵͳ����
	Shell_NotifyIcon(NIM_ADD, &nid);



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
			g_CurrentLang = "en-US"; //Ӣ��
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
	while (!done && !glfwWindowShouldClose(wc))
	{

		Sleep(1); // �����Ȼ����������CPUռ�á���

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
				done = true;

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
						currentConfigIndex = 1;
						Config& initialConfig = configs[currentConfigIndex];

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

						specialConfigs.clear();

						// 4. ���浽�ļ����л�����Ӧ�ý���
						SaveConfigs();
						showConfigWizard = false;
						showMainApp = true;
					}
				}
				ImGui::Text(L("WIZARD_WARNING_TIPS"));
			}

			ImGui::End();
		}
		else if (showMainApp) {
			static int selectedWorldIndex = -1;       // �����û����б���ѡ�������
			static char backupComment[CONSTANT1] = "";// ����ע������������
			// ��ȡ��ǰ����
			if (!configs.count(currentConfigIndex)) { // �Ҳ�����˵��Ӧ�ö�Ӧ������������
				specialSetting = true;
			}

			//Config& cfg = configs[currentConfigIndex];


			// --- ��̬��������ͼ������ͳߴ������Ĵ�С ---
			vector<DisplayWorld> displayWorlds = BuildDisplayWorldsForSelection();
			int worldCount = (int)displayWorlds.size();
			//if ((int)worldIconTextures.size() != worldCount) {
			//	// �ڵ�����Сǰ���ͷžɵġ�������Ҫ��������Դ
			//	for (auto& tex : worldIconTextures) {
			//		if (tex) {
			//			tex->Release();
			//			tex = nullptr;
			//		}
			//	}
			//	worldIconTextures.assign(worldCount, nullptr); // ʹ�� assign ��ղ�����Ϊָ����С�Ŀ�ָ��
			//	worldIconWidths.resize(worldCount, 0);
			//	worldIconHeights.resize(worldCount, 0);
			//}

			ImGui::SetNextWindowSize(ImVec2(1300, 720), ImGuiCond_FirstUseEver);
			ImGui::Begin(L("MAIN_WINDOW_TITLE"), &showMainApp, ImGuiWindowFlags_MenuBar);

			float totalW = ImGui::GetContentRegionAvail().x;
			float leftW = totalW * 0.32f;
			float midW = totalW * 0.25f;
			float rightW = totalW * 0.42f;
			static bool showAboutWindow = false;
			// --- �����˵��� ---
			if (ImGui::BeginMenuBar()) {
				
				if (ImGui::BeginMenu(L("MENU_FILE"))) {
					if (ImGui::MenuItem(L("EXIT"))) {
						done = true;
						SaveConfigs();
					}
					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu(L("SETTINGS"))) {

					if (ImGui::Checkbox(L("RUN_ON_WINDOWS_STARTUP"), &g_RunOnStartup)) {
						wchar_t selfPath[MAX_PATH];
						GetModuleFileNameW(NULL, selfPath, MAX_PATH);
						SetAutoStart("MineBackup_AutoTask_" + to_string(currentConfigIndex), selfPath, false, currentConfigIndex, g_RunOnStartup);
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
						if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(ImGui::GetContentRegionAvail().x/2, 0))) {
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
						showMainApp = false;
					}
					ImGui::SameLine(0, 0);

					// Maximize/Restore Button
					static bool is_maximized = false;
					if (ImGui::Button("O", ImVec2(buttonSize, buttonSize))) {
						if (is_maximized) {
							ImGui::SetWindowSize(ImVec2(1300, 720));
							is_maximized = false;
						}
						else {
							ImVec2 displaySize = ImGui::GetIO().DisplaySize;
							ImGui::SetWindowSize(displaySize);
							ImGui::SetWindowPos(ImVec2(0, 0));
							is_maximized = true;
						}
					}
					ImGui::SameLine(0, 0);

					// Close Button
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.1f, 0.1f, 0.8f));
					if (ImGui::Button("x", ImVec2(buttonSize, buttonSize))) {
						done = true; // ��������
						SaveConfigs();
					}
					ImGui::PopStyleColor();
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



			// --- �����壺�����б�Ͳ��� ---
			//ImGui::BeginChild("LeftPane", ImVec2(ImGui::GetContentRegionAvail().x * 0.875f, 0), true);
			
			float left_pane_width = ImGui::GetContentRegionAvail().x * 0.55f;
			if (selectedWorldIndex == -1) {
				left_pane_width = ImGui::GetContentRegionAvail().x; // ���δѡ���κ��������ռ��
			}
			
			ImGui::BeginChild("LeftPane", ImVec2(leftW, 0), true);

			ImGui::SeparatorText(L("QUICK_CONFIG_SWITCHER"));
			ImGui::SetNextItemWidth(-1);
			string current_config_label = "None";
			if (specialSetting && specialConfigs.count(currentConfigIndex)) {
				current_config_label = "[Sp." + to_string(currentConfigIndex) + "] " + specialConfigs[currentConfigIndex].name;
			}
			else if (!specialSetting && configs.count(currentConfigIndex)) {
				current_config_label = "[No." + to_string(currentConfigIndex) + "] " + configs[currentConfigIndex].name;
			}
			//string(L("CONFIG_N")) + to_string(currentConfigIndex)
			static bool showAddConfigPopup = false, showDeleteConfigPopup = false;

			if (ImGui::BeginCombo("##ConfigSwitcher", current_config_label.c_str())) {
				// ��ͨ����
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
				// ��������
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
				ImGui::Separator();
				if (ImGui::Selectable(L("BUTTON_ADD_CONFIG"))) {
					showAddConfigPopup = true;
				}
				
				if (ImGui::Selectable(L("BUTTON_DELETE_CONFIG"))) {
					if ((!specialSetting && configs.size() > 1) || (specialSetting && !specialConfigs.empty())) { // ���ٱ���һ��
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
					ImGui::Text(L("CONFIRM_DELETE_MSG"), currentConfigIndex, specialConfigs[currentConfigIndex].name);
				}
				else {
					ImGui::Text(L("CONFIRM_DELETE_MSG"), currentConfigIndex, configs[currentConfigIndex].name);
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
							//int new_index = configs.empty() ? 1 : configs.rbegin()->first + 1;
							// ԭ���� configs.rbegin()->first + 1��������̫�ã�����ͳһ��nextConfigId
							int new_index = CreateNewNormalConfig(new_config_name);
							// �̳е�ǰ���ã�����У���������·��Ϊ��
							if (configs.count(currentConfigIndex)) {
								configs[new_index] = configs[currentConfigIndex];
								configs[new_index].name = new_config_name;
								configs[new_index].saveRoot.clear();
								configs[new_index].backupPath.clear();
								configs[new_index].worlds.clear();
							}
							currentConfigIndex = new_index;
							specialSetting = false;
						}
						else { // Special
							int new_index = CreateNewSpecialConfig(new_config_name);
							currentConfigIndex = new_index;
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

				// �ټ���
				//if (!worldIconTextures[i]) {
				//	string iconPath = utf8_to_gbk(wstring_to_utf8(worldFolder + L"\\icon.png"));
				//	if (filesystem::exists(iconPath)) {
				//		LoadTextureFromFile(iconPath.c_str(), &worldIconTextures[i], &worldIconWidths[i], &worldIconHeights[i]);
				//	}
				//	else if (filesystem::exists(utf8_to_gbk(wstring_to_utf8(worldFolder + L"\\world_icon.jpeg")))) {
				//		// ���Ұ�� world_icon.jpeg
				//		LoadTextureFromFile(utf8_to_gbk(wstring_to_utf8(worldFolder + L"\\world_icon.jpeg")).c_str(), &worldIconTextures[i], &worldIconWidths[i], &worldIconHeights[i]);
				//	}
				//}

				/*if (worldIconTextures[i]) {
					ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)worldIconTextures[i], icon_pos, icon_end_pos, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4.0f);
				}
				else {
					const char* placeholder_icon = ICON_FA_FOLDER;
					ImVec2 text_size = ImGui::CalcTextSize(placeholder_icon);
					ImVec2 text_pos = ImVec2(icon_pos.x + (iconSz - text_size.x) * 0.5f, icon_pos.y + (iconSz - text_size.y) * 0.5f);
					draw_list->AddText(text_pos, IM_COL32(200, 200, 200, 255), placeholder_icon);
				}*/

				// ������ƹ�ͼ������
				ImGui::Dummy(ImVec2(iconSz, iconSz));

				ImGui::SetCursorScreenPos(icon_pos);
				ImGui::InvisibleButton("##icon_button", ImVec2(iconSz, iconSz));
				// �������ͼ��
				//if (ImGui::IsItemClicked()) {
				//	wstring sel = SelectFileDialog();
				//	if (!sel.empty()) {
				//		// ����ԭ icon.png
				//		CopyFileW(sel.c_str(), (worldFolder + L"\\icon.png").c_str(), FALSE);
				//		// �ͷž��������¼���
				//		if (worldIconTextures[i]) {
				//			worldIconTextures[i]->Release();
				//			worldIconTextures[i] = nullptr;
				//		}
				//		LoadTextureFromFile(utf8_to_gbk(wstring_to_utf8(worldFolder + L"\\icon.png")).c_str(), &worldIconTextures[i], &worldIconWidths[i], &worldIconHeights[i]);
				//	}
				//}
				ImGui::SameLine();
				// --- ״̬�߼� (Ϊͼ����׼��) ---
				lock_guard<mutex> lock(g_task_mutex); // ���� g_active_auto_backups ��Ҫ����
                bool is_task_running = g_active_auto_backups.count(make_pair(displayWorlds[i].baseConfigIndex, i)) > 0;
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
				if(desc_utf8.empty()) {
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

			ImGui::EndChild();
			
			if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size()) {
				ImGui::SameLine();
				ImGui::BeginChild("MidPane", ImVec2(midW, 0), true);
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
						if (configs.count(dw.baseConfigIndex)) {
							Config& cfg = configs.at(dw.baseConfigIndex);
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
							DisplayWorld dw_copy = displayWorlds[selectedWorldIndex]; // ��һ��ֵ������֮��������������ȥ�� configs

							bool did_change = false;

							// ���޸�ȫ�� configs ǰ��������ֹ�����̲߳�����/д���±���
							{
								lock_guard<mutex> cfg_lock(g_configsMutex);

								auto it = configs.find(dw_copy.baseConfigIndex);
								if (it != configs.end()) {
									Config& cfg = it->second;
									if (dw_copy.baseWorldIndex >= 0 && dw_copy.baseWorldIndex < (int)cfg.worlds.size()) {
										cfg.worlds[dw_copy.baseWorldIndex].second = L"#";
										did_change = true;
									}
								}
							} // ���� g_configsMutex 
						}
					}

					if (ImGui::Button(L("BUTTON_PIN_WORLD"), ImVec2(-1, 0))) {
						// ��������Ƿ���Ч�Ҳ��ǵ�һ��
						if (selectedWorldIndex > 0 && selectedWorldIndex < displayWorlds.size()) {
							DisplayWorld& dw = displayWorlds[selectedWorldIndex];
							int configIdx = dw.baseConfigIndex;
							int worldIdx = dw.baseWorldIndex;

							// ȷ�����ǲ���������ͨ�����е������б�
							if (!specialSetting && configs.count(configIdx)) {
								Config& cfg = configs[configIdx];
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
							ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
						}
						else {
							ShellExecuteW(NULL, L"open", displayWorlds[selectedWorldIndex].effectiveConfig.backupPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
						}
					}
					if (ImGui::Button(L("OPEN_SAVEROOT_FOLDER"), ImVec2(-1, 0))) {
						wstring path = displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot + L"\\" + displayWorlds[selectedWorldIndex].name;
						ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
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
							if (configs.count(currentConfigIndex)) {
								filesystem::path tempPath = displayWorlds[selectedWorldIndex].effectiveConfig.saveRoot;
								filesystem::path modsPath = tempPath.parent_path() / "mods";
								if (!filesystem::exists(modsPath) && filesystem::exists(tempPath / "mods")) { // ��������ģ����ܷ���worldͬ���ļ�����
									modsPath = tempPath / "mods";
								}
								thread backup_thread(DoOthersBackup, configs[currentConfigIndex], modsPath, utf8_to_wstring(mods_comment));
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
					// ����������Ҫ���ݵ��������ݵ�·�������� D:\Games\configs
					static char buf[CONSTANT1] = "";
					strcpy(buf, wstring_to_utf8(displayWorlds[selectedWorldIndex].effectiveConfig.othersPath).c_str());
					if (ImGui::InputTextWithHint("##OTHERS", L("HINT_BACKUP_WHAT"), buf, IM_ARRAYSIZE(buf))) {
						displayWorlds[selectedWorldIndex].effectiveConfig.othersPath = utf8_to_wstring(buf);
						configs[displayWorlds[selectedWorldIndex].baseConfigIndex].othersPath = displayWorlds[selectedWorldIndex].effectiveConfig.othersPath;
					}

					if (ImGui::BeginPopupModal("Others", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
						static char others_comment[CONSTANT1] = "";
						ImGui::TextUnformatted(L("CONFIRM_BACKUP_OTHERS_MSG"));
						ImGui::InputText(L("HINT_BACKUP_COMMENT"), others_comment, IM_ARRAYSIZE(others_comment));
						ImGui::Separator();

						if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
							if (configs.count(currentConfigIndex)) {
								thread backup_thread(DoOthersBackup, configs[currentConfigIndex], buf, utf8_to_wstring(others_comment));
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
						const Config& config = configs[displayWorlds[selectedWorldIndex].baseConfigIndex];
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
						lock_guard<mutex> lock(g_task_mutex);
						if (selectedWorldIndex >= 0) {
							// ���ʹ�� displayWorlds��
							localDisplayWorlds = BuildDisplayWorldsForSelection();
							if (selectedWorldIndex < (int)localDisplayWorlds.size()) {
								taskKey = { localDisplayWorlds[selectedWorldIndex].baseConfigIndex, localDisplayWorlds[selectedWorldIndex].baseWorldIndex };
								is_task_running = (g_active_auto_backups.count(taskKey) > 0);
							}
						}
					}

					if (is_task_running) {
						ImGui::Text(L("AUTOBACKUP_RUNNING"), wstring_to_utf8(localDisplayWorlds[selectedWorldIndex].name).c_str());
						ImGui::Separator();
						if (ImGui::Button(L("BUTTON_STOP_AUTOBACKUP"), ImVec2(240, 0))) {
							if (g_active_auto_backups.count(taskKey)) {
								// 1. ����ֹͣ��־
								g_active_auto_backups.at(taskKey).stop_flag = true;
								// 2. �ȴ��߳̽���
								if (g_active_auto_backups.at(taskKey).worker.joinable())
									g_active_auto_backups.at(taskKey).worker.join();
								// 3. �ӹ��������Ƴ�
								g_active_auto_backups.erase(taskKey);
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
						static int interval = 15;
						ImGui::InputInt(L("INTERVAL_MINUTES"), &interval);
						if (interval < 1) interval = 1;
						if (ImGui::Button(L("BUTTON_START"), ImVec2(120, 0))) {
							// ע�Ტ�����߳�
							lock_guard<mutex> lock(g_task_mutex);
							if (taskKey.first >= 0) {
								AutoBackupTask& task = g_active_auto_backups[taskKey];
								task.stop_flag = false;

								task.worker = thread(AutoBackupThreadFunction, taskKey.first, taskKey.second, interval, &console, ref(task.stop_flag));

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

			if (showSettings) ShowSettingsWindow();
			if (showHistoryWindow) {
				if (specialSetting) {
					if (selectedWorldIndex >= 0 && selectedWorldIndex < displayWorlds.size())
						ShowHistoryWindow(displayWorlds[selectedWorldIndex].baseConfigIndex);
					else if (!specialConfigs[currentConfigIndex].tasks.empty())
						ShowHistoryWindow(specialConfigs[currentConfigIndex].tasks[0].configIndex);
				}
				else {
					ShowHistoryWindow(currentConfigIndex);
				}
			}
			//console.Draw(L("CONSOLE_TITLE"), &showMainApp);
			//ImGui::EndChild();
			ImGui::End();
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
	lock_guard<mutex> lock(g_task_mutex);
	for (auto& pair : g_active_auto_backups) {
		pair.second.stop_flag = true; // ֪ͨ�߳�ֹͣ
		if (pair.second.worker.joinable()) {
			pair.second.worker.join(); // �ȴ��߳�ִ�����
		}
	}
	/*for (auto& tex : worldIconTextures) {
		if (tex) {
			tex->Release();
			tex = nullptr;
		}
	}
	worldIconTextures.clear();*/
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


	// �����ȼ�
	//::UnregisterHotKey(hwnd, MINEBACKUP_HOTKEY_ID);
	//::UnregisterHotKey(hwnd, MINERESTORE_HOTKEY_ID);
	return 0;
}


// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
//LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
//{
//	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
//		return true;
//
//	switch (msg)
//	{
//		// ��������ͼ���¼������/�Ҽ������
//	case WM_USER + 1: {
//		// lParam ��ʾ�����¼��������������Ҽ������
//		switch (lParam) {
//		case WM_LBUTTONUP: {
//			// ��ʾ�����������ڣ��������/��С����
//			showMainApp = true;
//			break;
//		}
//		// �Ҽ��������ͼ�꣺���������Ĳ˵�
//		case WM_RBUTTONUP: {
//			// �����˵�
//			HMENU hMenu = CreatePopupMenu();
//			AppendMenu(hMenu, MF_STRING, 1001, utf8_to_wstring((string)L("OPEN")).c_str());
//			AppendMenu(hMenu, MF_STRING, 1002, utf8_to_wstring((string)L("EXIT")).c_str());
//
//			// ��ȡ���λ�ã��˵���ʾ������Ҽ������λ�ã�
//			POINT pt;
//			GetCursorPos(&pt);
//
//			// ��ʾ�˵���TPM_BOTTOMALIGN���˵��ײ��������λ�ã�
//			TrackPopupMenu(
//				hMenu,
//				TPM_BOTTOMALIGN | TPM_LEFTBUTTON,  // �˵���ʽ
//				pt.x, pt.y,
//				0,
//				hWnd,
//				NULL
//			);
//
//			// ������ô˺���������˵������޷������ر�
//			SetForegroundWindow(hWnd);
//			// ���ٲ˵��������ڴ�й©��
//			DestroyMenu(hMenu);
//			break;
//		}
//		}
//		break;
//	}
//					// ����˵����������򿪽��桱�򡰹رա��󴥷���
//	case WM_COMMAND: {
//		switch (LOWORD(wParam)) {
//		case 1001:  // ������򿪽��桱
//			showMainApp = true;
//			SetForegroundWindow(hWnd);
//			break;
//		case 1002:  // ������رա�
//			// ���Ƴ�����ͼ�꣬���˳�����
//			done = true;
//			Shell_NotifyIcon(NIM_DELETE, &nid);
//			PostQuitMessage(0);
//			break;
//		}
//		break;
//	}
//	case WM_HOTKEY:
//		if (wParam == MINEBACKUP_HOTKEY_ID) {
//			TriggerHotkeyBackup();
//		}
//		else if (wParam == MINERESTORE_HOTKEY_ID) {
//			TriggerHotkeyRestore();
//		}
//		break;
//	case WM_SIZE:
//		if (wParam == SIZE_MINIMIZED)
//			return 0;
//		g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
//		g_ResizeHeight = (UINT)HIWORD(lParam);
//		return 0;
//	case WM_SYSCOMMAND:
//		if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
//			return 0;
//		break;
//	case WM_DESTROY:
//		Shell_NotifyIcon(NIM_DELETE, &nid);  // ��������ͼ��
//		done = true;
//		::PostQuitMessage(0);
//		return 0;
//	}
//	return ::DefWindowProcW(hWnd, msg, wParam, lParam);
//}
//
//// --- Helper function to load an image into a D3D11 texture ---
//bool LoadTextureFromFile(const char* filename, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height) {
//	// Load from disk into a raw RGBA buffer
//	int image_width = 0;
//	int image_height = 0;
//	unsigned char* image_data = stbi_load(filename, &image_width, &image_height, NULL, 4);
//	if (image_data == NULL)
//		return false;
//
//	// Create texture
//	D3D11_TEXTURE2D_DESC desc;
//	ZeroMemory(&desc, sizeof(desc));
//	desc.Width = image_width;
//	desc.Height = image_height;
//	desc.MipLevels = 1;
//	desc.ArraySize = 1;
//	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
//	desc.SampleDesc.Count = 1;
//	desc.Usage = D3D11_USAGE_DEFAULT;
//	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
//	desc.CPUAccessFlags = 0;
//
//	ID3D11Texture2D* pTexture = NULL;
//	D3D11_SUBRESOURCE_DATA subResource;
//	subResource.pSysMem = image_data;
//	subResource.SysMemPitch = desc.Width * 4;
//	subResource.SysMemSlicePitch = 0;
//	g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);
//
//	// Create texture view
//	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
//	ZeroMemory(&srvDesc, sizeof(srvDesc));
//	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
//	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
//	srvDesc.Texture2D.MipLevels = desc.MipLevels;
//	srvDesc.Texture2D.MostDetailedMip = 0;
//	g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
//	pTexture->Release();
//
//	*out_width = image_width;
//	*out_height = image_height;
//	stbi_image_free(image_data);
//
//	return true;
//}

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
		//string latestVersion = find_json_value(responseBody, "tag_name");
		// ʹ�ø��ɿ��� JSON ������
		string latestVersion = nlohmann::json::parse(responseBody)["tag_name"].get<std::string>();
		// �Ƴ��汾��ǰ�� 'v'
		if (!latestVersion.empty() && (latestVersion[0] == 'v' || latestVersion[0] == 'V')) {
			latestVersion = latestVersion.substr(1);
		}
		short pos1 = latestVersion.find('.');
		short pos2 = latestVersion.find('.', pos1 + 1);
		short pos3 = latestVersion.find('-');
		short pos11 = CURRENT_VERSION.find('.');
		short pos22 = CURRENT_VERSION.find('.', pos1 + 1);
		short pos33 = CURRENT_VERSION.find('-');
		bool isNew = false;
		// ��������ֵ����ֵ
		short curMajor = stoi(CURRENT_VERSION.substr(0, pos11)), curMinor1 = stoi(CURRENT_VERSION.substr(pos11 + 1, pos22)), newMajor = stoi(latestVersion.substr(0, pos1)), newMinor1 = stoi(latestVersion.substr(pos1 + 1, pos2));
		short curMinor2 = pos33 == string::npos ? stoi(CURRENT_VERSION.substr(pos22 + 1)) : stoi(CURRENT_VERSION.substr(pos22 + 1, pos33)), newMinor2 = pos3 == string::npos ? stoi(latestVersion.substr(pos2 + 1)) : stoi(latestVersion.substr(pos2 + 1, pos3));
		short curSp = pos33 == string::npos ? 0 : stoi(CURRENT_VERSION.substr(pos33 + 3)), newSp = pos3 == string::npos ? 0 : stoi(latestVersion.substr(pos3 + 3));
		// ���⼸�ְ汾�� v1.7.9 v1.7.10 v1.7.9-sp1
		// ��һ����д�÷ǳ��ǳ������⣬���ǡ��������Ű�
		
		
		if (newMajor > curMajor) {
			isNew = true;
		}
		else if (newMajor == curMajor) {
			if (newMinor1 > curMinor1) {
				isNew = true;
			}
			else if (newMinor1 == curMinor1) {
				if (newMinor2 > curMinor2) {
					isNew = true;
				}
				else if (newMinor2 == curMinor2) {
					if (newSp > curSp) {
						isNew = true;
					}
				}
			}
		}

		// �򵥰汾�Ƚ� (���� "1.7.0" > "1.6.7")
		if (!latestVersion.empty() && isNew) {
			g_LatestVersionStr = "v" + latestVersion;
			g_NewVersionAvailable = true;
			g_ReleaseNotes = nlohmann::json::parse(responseBody)["body"].get<std::string>();;
			for (int i = 0; i < g_ReleaseNotes.size() - 1; ++i)
			{
				if (g_ReleaseNotes[i] == '#')
					g_ReleaseNotes[i] = ' ';
				else if (g_ReleaseNotes[i] == '\\' && g_ReleaseNotes[i + 1] == 'n')
					g_ReleaseNotes[i] = '\n', g_ReleaseNotes[i + 1] = ' ';
				else if (g_ReleaseNotes[i] == '\\')
					g_ReleaseNotes[i] = ' ', g_ReleaseNotes[i + 1] = ' ';
			}
			// ���� .exe ��������  -- ֱ���ֶ�ƴ�Ӿ���
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
			//			break; // �ҵ����˳�
			//		}
			//		search_pos = asset_obj_end;
			//	}
			//}
		}
	}
	catch (...) {
		// ����ʧ�ܣ���Ĭ����
	}

cleanup:
	if (hRequest) WinHttpCloseHandle(hRequest);
	if (hConnect) WinHttpCloseHandle(hConnect);
	if (hSession) WinHttpCloseHandle(hSession);
	g_UpdateCheckDone = true;
}

// �㲥
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
	lock_guard<mutex> lock(g_configsMutex);
	configs.clear();
	specialConfigs.clear();
	ifstream in(filename, ios::binary);
	if (!in.is_open()) return;
	string line1;
	wstring line, section;
	// cur��Ϊһ��ָ�룬ָ�� configs ���ȫ�� map<int, Config> �е�Ԫ�� Config
	Config* cur = nullptr;
	SpecialConfig* spCur = nullptr;
	bool restoreWhiteList = false;

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
				else if (key == L"ZipMethod") cur->zipMethod = val;
				else if (key == L"KeepCount") cur->keepCount = stoi(val);
				else if (key == L"SmartBackup") cur->backupMode = stoi(val);
				else if (key == L"RestoreBeforeBackup") cur->backupBefore = (val != L"0");
				else if (key == L"HotBackup") cur->hotBackup = (val != L"0");
				else if (key == L"SilenceMode") isSilence = (val != L"0");
				else if (key == L"BackupNaming") cur->folderNameType = stoi(val);
				else if (key == L"SilenceMode") isSilence = (val != L"0");
				else if (key == L"CpuThreads") cur->cpuThreads = stoi(val);
				else if (key == L"UseLowPriority") cur->useLowPriority = (val != L"0");
				else if (key == L"SkipIfUnchanged") cur->skipIfUnchanged = (val != L"0");
				else if (key == L"MaxSmartBackups") cur->maxSmartBackupsPerFull = stoi(val);
				else if (key == L"BackupOnStart") cur->backupOnGameStart = (val != L"0");
				else if (key == L"BlacklistItem") cur->blacklist.push_back(val);
				else if (key == L"CloudSyncEnabled") cur->cloudSyncEnabled = (val != L"0");
				else if (key == L"RclonePath") cur->rclonePath = val;
				else if (key == L"RcloneRemotePath") cur->rcloneRemotePath = val;
				else if (key == L"SnapshotPath") cur->snapshotPath = val;
				else if (key == L"OtherPath") cur->othersPath = val;
				else if (key == L"Theme") {
					cur->theme = stoi(val);
					//ApplyTheme(cur->theme); ���Ҫת������gui֮�󣬷����ֱ�ӵ��±���
				}
				else if (key == L"Font") {
					cur->fontPath = val;
					Fontss = val;
					if (val.empty() || !filesystem::exists(val)) { // ����û�лᵼ�±���������������������
						LANGID lang_id = GetUserDefaultUILanguage();

						if (lang_id == 2052 || lang_id == 1028) {
							g_CurrentLang = "zh-CN";
							if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttc"))
								Fontss = L"C:\\Windows\\Fonts\\msyh.ttc";
							else if (filesystem::exists("C:\\Windows\\Fonts\\msyh.ttf"))
								Fontss = L"C:\\Windows\\Fonts\\msyh.ttf";
						}
						else {
							g_CurrentLang = "en-US"; //Ӣ��
							Fontss = L"C:\\Windows\\Fonts\\SegoeUI.ttf";
						}
					}
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
				else if (key == L"Theme") spCur->theme = stoi(val);
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
				else if (key == L"BackupOnStart") spCur->backupOnGameStart = (val != L"0");
				else if (key == L"BlacklistItem") spCur->blacklist.push_back(val);
			}
			else if (section == L"General") { // Inside [General] section
				if (key == L"CurrentConfig") {
					currentConfigIndex = stoi(val);
				}
				else if (key == L"NextConfigId") {
					nextConfigId = stoi(val);
					int maxId = 0;
					for (auto& kv : configs) if (kv.first > maxId) maxId = kv.first;
					for (auto& kv : specialConfigs) if (kv.first > maxId) maxId = kv.first;
					if (nextConfigId <= maxId) nextConfigId = maxId + 1;
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
				else if (key == L"RunOnStartup") {
					g_RunOnStartup = (val != L"0");
				}
				else if (key == L"RestoreWhitelistItem") {
					restoreWhiteList = true;
					restoreWhitelist.push_back(val);
				}
			}
		}
	}
	if (!restoreWhiteList) {
		restoreWhitelist.push_back(L"fake_player.gca.json");
	}
}

static void SaveConfigs(const wstring& filename) {
	lock_guard<mutex> lock(g_configsMutex);
	wofstream out(filename, ios::binary);
	if (!out.is_open()) {
		MessageBoxW(nullptr, utf8_to_wstring(L("ERROR_CONFIG_WRITE_FAIL")).c_str(), utf8_to_wstring(L("ERROR_TITLE")).c_str(), MB_OK | MB_ICONERROR);
		return;
	}
	//out.imbue(locale("chs"));//�������������ANSI��
	out.imbue(locale(out.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
	//out.imbue(locale(out.getloc(), new codecvt_utf8<wchar_t>));//��UTF8תΪUTF������C++17Ҳ�����ˡ�������������define��
	out << L"[General]\n";
	out << L"CurrentConfig=" << currentConfigIndex << L"\n";
	out << L"NextConfigId=" << nextConfigId << L"\n";
	out << L"Language=" << utf8_to_wstring(g_CurrentLang) << L"\n";
	out << L"CheckForUpdates=" << (g_CheckForUpdates ? 1 : 0) << L"\n";
	out << L"EnableKnotLink=" << (g_enableKnotLink ? 1 : 0) << L"\n";
	out << L"RunOnStartup=" << (g_RunOnStartup ? 1 : 0) << L"\n";
	for (const auto& item : restoreWhitelist) {
		out << L"RestoreWhitelistItem=" << item << L"\n\n";
	}

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
		out << L"ZipMethod=" << c.zipMethod << L"\n";
		out << L"CpuThreads=" << c.cpuThreads << L"\n";
		out << L"UseLowPriority=" << (c.useLowPriority ? 1 : 0) << L"\n";
		out << L"KeepCount=" << c.keepCount << L"\n";
		out << L"SmartBackup=" << c.backupMode << L"\n";
		out << L"RestoreBeforeBackup=" << (c.backupBefore ? 1 : 0) << L"\n";
		out << L"HotBackup=" << (c.hotBackup ? 1 : 0) << L"\n";
		out << L"SilenceMode=" << (isSilence ? 1 : 0) << L"\n";
		out << L"Theme=" << c.theme << L"\n";
		out << L"Font=" << c.fontPath << L"\n";
		out << L"BackupNaming=" << c.folderNameType << L"\n";
		out << L"SilenceMode=" << (isSilence ? 1 : 0) << L"\n";
		out << L"SkipIfUnchanged=" << (c.skipIfUnchanged ? 1 : 0) << L"\n";
		out << L"MaxSmartBackups=" << c.maxSmartBackupsPerFull << L"\n";
		out << L"BackupOnStart=" << (c.backupOnGameStart ? 1 : 0) << L"\n";
		out << L"CloudSyncEnabled=" << (c.cloudSyncEnabled ? 1 : 0) << L"\n";
		out << L"RclonePath=" << c.rclonePath << L"\n";
		out << L"RcloneRemotePath=" << c.rcloneRemotePath << L"\n";
		out << L"SnapshotPath=" << c.snapshotPath << L"\n";
		out << L"OtherPath=" << c.othersPath << L"\n";
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
		// �µ�����ṹ
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
		out << L"BackupOnStart=" << (sc.backupOnGameStart ? 1 : 0) << L"\n";
		out << L"Theme=" << sc.theme << L"\n";
		for (const auto& item : sc.blacklist) {
			out << L"BlacklistItem=" << item << L"\n";
		}
		out << L"\n\n";
	}
}

void ShowSettingsWindow() {
	ImGui::Begin(L("SETTINGS"), &showSettings, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
	ImGui::SeparatorText(L("CONFIG_MANAGEMENT"));

	string current_config_label = "None";
	if (specialConfigs.count(currentConfigIndex)) {
		specialSetting = true;
		current_config_label = "[Sp." + to_string(currentConfigIndex) + "] " + specialConfigs[currentConfigIndex].name;
	}
	else if (configs.count(currentConfigIndex)) {
		specialSetting = false;
		current_config_label = "[No." + to_string(currentConfigIndex) + "] " + configs[currentConfigIndex].name;
	}
	else {
		return;
	}
	//string(L("CONFIG_N")) + to_string(currentConfigIndex)
	static bool showAddConfigPopup = false, showDeleteConfigPopup = false;
	if (ImGui::BeginCombo(L("CURRENT_CONFIG"), current_config_label.c_str())) {
		// ��ͨ����
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
		// ��������
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

		ImGui::Separator();
		if (ImGui::Selectable(L("BUTTON_ADD_CONFIG"))) {
			showAddConfigPopup = true;
		}

		if (ImGui::Selectable(L("BUTTON_DELETE_CONFIG"))) {
			if ((!specialSetting && configs.size() > 1) || (specialSetting && !specialConfigs.empty())) { // ���ٱ���һ��
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
			ImGui::Text(L("CONFIRM_DELETE_MSG"), currentConfigIndex, specialConfigs[currentConfigIndex].name);
		}
		else {
			ImGui::Text(L("CONFIRM_DELETE_MSG"), currentConfigIndex, configs[currentConfigIndex].name);
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
					if (configs.count(currentConfigIndex)) {
						configs[new_index] = configs[currentConfigIndex];
						configs[new_index].name = new_config_name;
						configs[new_index].saveRoot.clear();
						configs[new_index].backupPath.clear();
						configs[new_index].worlds.clear();
					}
					currentConfigIndex = new_index;
					specialSetting = false;
				}
				else { // Special
					int new_index = CreateNewSpecialConfig(new_config_name);
					currentConfigIndex = new_index;
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
		if (!specialConfigs.count(currentConfigIndex)) {
			specialSetting = false;
			currentConfigIndex = configs.empty() ? 1 : configs.begin()->first;
		}
		else {
			SpecialConfig& spCfg = specialConfigs[currentConfigIndex];

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
					SetAutoStart("MineBackup_AutoTask_" + to_string(currentConfigIndex), selfPath, true, currentConfigIndex, spCfg.runOnStartup);
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
				specialConfigs[currentConfigIndex].autoExecute = true;
				SaveConfigs();
				wchar_t selfPath[MAX_PATH];
				GetModuleFileNameW(NULL, selfPath, MAX_PATH);
				ShellExecuteW(NULL, L"open", selfPath, NULL, NULL, SW_SHOWNORMAL);
				PostQuitMessage(0);
				done = true;
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

				string current_task_config_label = configs.count(task.configIndex) ? (string(L("CONFIG_N")) + to_string(task.configIndex)) : "None";
				if (ImGui::BeginCombo(L("CONFIG_COMBO"), current_task_config_label.c_str())) {
					for (auto const& [idx, val] : configs) {
						if (ImGui::Selectable((string(L("CONFIG_N")) + to_string(idx)).c_str(), task.configIndex == idx)) {
							task.configIndex = idx;
							task.worldIndex = val.worlds.empty() ? -1 : 0; // ��������idx
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
		if (!configs.count(currentConfigIndex)) {
			// ������ñ�ɾ��
			if (configs.empty()) configs[1] = Config(); // ���1��ɾ���½�
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
							specialConfigs[currentConfigIndex].blacklist.push_back(utf8_to_wstring(regex_buf));
						}
						else {
							configs[currentConfigIndex].blacklist.push_back(utf8_to_wstring(regex_buf));
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

// ���Ʊ����ļ��������������Զ�ɾ����ɵ�
void LimitBackupFiles(const wstring& folderPath, int limit, Console* console)
{
	if (limit <= 0) return;
	namespace fs = filesystem;
	vector<fs::directory_entry> files;

	// �ռ����г����ļ�
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

	// ���δ�������ƣ����账��
	if ((int)files.size() <= limit) return;

	// �����д��ʱ������������ɵ���ǰ��
	sort(files.begin(), files.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
		return fs::last_write_time(a) < fs::last_write_time(b);
		});

	// ɾ�����������ļ�
	int to_delete = (int)files.size() - limit;
	for (int i = 0; i < to_delete && to_delete <= (int)files.size(); ++i) {
		try {
			if (files[i].path().filename().wstring().find(L"[Smart]") == 0 || files[i + 1].path().filename().wstring().find(L"[Smart]") == 0) // ��������ܱ��ݣ�����ɾ����������������ݣ������ǻ���
			{
				if (console) console->AddLog(L("LOG_WARNING_DELETE_SMART_BACKUP"), wstring_to_utf8(files[i].path().filename().wstring()).c_str());
			}
			fs::remove(files[i]);
			if (console) console->AddLog(L("LOG_DELETE_OLD_BACKUP"), wstring_to_utf8(files[i].path().filename().wstring()).c_str());
		}
		catch (const fs::filesystem_error& e) {
			if (console) console->AddLog(L("LOG_ERROR_DELETE_BACKUP"), e.what());
		}
	}
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

// �������գ������ȱ���
wstring CreateWorldSnapshot(const filesystem::path& worldPath, const wstring& snapshotPath, Console& console) {
	try {
		// ����һ��Ψһ����ʱĿ¼
		filesystem::path tempDir;
		if (snapshotPath.size() >= 2 && filesystem::exists(snapshotPath)) {
			tempDir = snapshotPath + L"\\MineBackup_Snapshot\\" + worldPath.filename().wstring();
		}
		else {
			tempDir = filesystem::temp_directory_path() / L"MineBackup_Snapshot" / worldPath.filename();
		}
		
		// ����ɵ���ʱĿ¼���ڣ��������
		if (filesystem::exists(tempDir)) {
			error_code ec_remove;
			filesystem::remove_all(tempDir, ec_remove);
			if (ec_remove) {
				console.AddLog("[Error] Failed to clean up old snapshot directory: %s", ec_remove.message().c_str());
				// ��ʹ����ʧ��Ҳ���Լ����������Ĵ������ܻ�ʧ�ܲ�������
			}
		}
		filesystem::create_directories(tempDir);
		console.AddLog(L("LOG_BACKUP_HOT_INFO"));

		// �ݹ鸴�ƣ������Ժ��Ե����ļ�����
		auto copyOptions = filesystem::copy_options::recursive | filesystem::copy_options::overwrite_existing;
		error_code ec;
		filesystem::copy(worldPath, tempDir, copyOptions, ec);

		if (ec) {
			// ��Ȼ�����˴��󣨿�����ĳ���ļ��������ˣ������󲿷��ļ������Ѿ����Ƴɹ�
			console.AddLog(L("LOG_BACKUP_HOT_INFO2"), ec.message().c_str());
			wstring xcopyCmd = L"xcopy \"" + worldPath.wstring() + L"\" \"" + tempDir.wstring() + L"\" /s /e /y /c";
			RunCommandInBackground(xcopyCmd, console, false);
		}
		else {
			console.AddLog(L("LOG_BACKUP_HOT_INFO3"), wstring_to_utf8(tempDir.wstring()).c_str());
		}
		// ���Ӷ�����ʱ��ȷ���ļ�ϵͳ�������ر��� xcopy����ȫ���
		this_thread::sleep_for(chrono::milliseconds(500));

		return tempDir.wstring();

	}
	catch (const filesystem::filesystem_error& e) {
		console.AddLog(L("LOG_BACKUP_HOT_INFO4"), e.what());
		return L"";
	}
}

// ִ�е�������ı��ݲ�����
// ����:
//   - config: ��ǰʹ�õ����á�
//   - world:  Ҫ���ݵ����磨����+��������
//   - console: ���̨��������ã����������־��Ϣ��
   //- commend: �û�ע��
void DoBackup(const Config config, const pair<wstring, wstring> world, Console& console, const wstring& comment) {
	console.AddLog(L("LOG_BACKUP_START_HEADER"));
	console.AddLog(L("LOG_BACKUP_PREPARE"), wstring_to_utf8(world.first).c_str());

	if (!filesystem::exists(config.zipPath)) {
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return;
	}

	// ׼��·��
	wstring originalSourcePath = config.saveRoot + L"\\" + world.first;
	wstring sourcePath = originalSourcePath; // Ĭ��ʹ��ԭʼ·��
	wstring destinationFolder = config.backupPath + L"\\" + world.first;
	wstring metadataFolder = config.backupPath + L"\\_metadata\\" + world.first; // Ԫ�����ļ���
	wstring command;
	wstring archivePath;
	wstring archiveNameBase = world.second.empty() ? world.first : world.second;

	if (!comment.empty()) {
		archiveNameBase += L" [" + SanitizeFileName(comment) + L"]";
	}


	// ���ɴ�ʱ������ļ���
	time_t now = time(0);
	tm ltm;
	localtime_s(&ltm, &now);
	wchar_t timeBuf[80];
	wcsftime(timeBuf, sizeof(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);
	archivePath = destinationFolder + L"\\" + L"[" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
	
	// ��������Ŀ���ļ��У���������ڣ�
	try {
		filesystem::create_directories(destinationFolder);
		filesystem::create_directories(metadataFolder); // ȷ��Ԫ�����ļ��д���
		console.AddLog(L("LOG_BACKUP_DIR_IS"), wstring_to_utf8(destinationFolder).c_str());
	}
	catch (const filesystem::filesystem_error& e) {
		console.AddLog(L("LOG_ERROR_CREATE_BACKUP_DIR"), e.what());
		return;
	}

	// ��������ȱ���
	if (config.hotBackup) {
		BroadcastEvent("event=pre_hot_backup;");
		wstring snapshotPath = CreateWorldSnapshot(sourcePath, config.snapshotPath, console);
		if (!snapshotPath.empty()) {
			sourcePath = snapshotPath; // ������ճɹ�����������в��������ڿ���·��
			this_thread::sleep_for(chrono::milliseconds(200));//�ڴ������պ���������ʱ�����ļ�ϵͳ��Ӧʱ��
			//originalSourcePath = snapshotPath;
		}
		else {
			console.AddLog(L("LOG_ERROR_SNAPSHOT"));
			return;
		}
	}

	bool forceFullBackup = true;
	if (filesystem::exists(destinationFolder)) {
		for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
			if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Full]") != wstring::npos) {
				forceFullBackup = false;
				break;
			}
		}
	}
	if (forceFullBackup)
		console.AddLog(L("LOG_FORCE_FULL_BACKUP"));

	// ���Ʊ���������
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
					++smartCount;
				}
			}

			if (fullFound && smartCount >= config.maxSmartBackupsPerFull) {
				forceFullBackupDueToLimit = true;
				console.AddLog(L("LOG_FORCE_FULL_BACKUP_LIMIT_REACHED"), config.maxSmartBackupsPerFull);
			}
		}
	}

	// --- �µ�ͳһ�ļ������߼� ---

	vector<filesystem::path> candidate_files;
	BackupCheckResult checkResult;
	map<wstring, size_t> currentState;
	candidate_files = GetChangedFiles(sourcePath, metadataFolder, destinationFolder, checkResult, currentState);
	// ���ݼ����������־��¼
	if (checkResult == BackupCheckResult::NO_CHANGE && config.skipIfUnchanged) {
		console.AddLog(L("LOG_NO_CHANGE_FOUND"));
		return;
	}
	else if (checkResult == BackupCheckResult::FORCE_FULL_BACKUP_METADATA_INVALID) {
		console.AddLog(L("LOG_METADATA_INVALID"));
	}
	else if (checkResult == BackupCheckResult::FORCE_FULL_BACKUP_BASE_MISSING && config.backupMode == 2) {
		console.AddLog(L("LOG_BASE_BACKUP_NOT_FOUND"));
	}

	forceFullBackup = (checkResult == BackupCheckResult::FORCE_FULL_BACKUP_METADATA_INVALID ||
		checkResult == BackupCheckResult::FORCE_FULL_BACKUP_BASE_MISSING ||
		forceFullBackupDueToLimit) || forceFullBackup;

	// ���ݱ���ģʽȷ����ѡ�ļ��б�
	if (config.backupMode == 2 && !forceFullBackup) { // ���ܱ���ģʽ
		
		// GetChangedFiles ���ص����Ѹı���ļ��б�
		candidate_files = GetChangedFiles(sourcePath, metadataFolder, destinationFolder, checkResult, currentState);
		// ... (���� checkResult ���߼����ֲ���, �� LOG_NO_CHANGE_FOUND ��)
	}
	else { // ��ͨ���ݻ�ǿ����������
		// ��ѡ�б���Դ·���µ������ļ�
		try {
			candidate_files.clear();
			for (const auto& entry : filesystem::recursive_directory_iterator(sourcePath)) {
				if (entry.is_regular_file()) {
					candidate_files.push_back(entry.path());
				}
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog("[Error] Failed to scan source directory %s: %s", wstring_to_utf8(sourcePath).c_str(), e.what());
			if (config.hotBackup) {
				filesystem::remove_all(sourcePath); 
			}
			return;
		}
	}

	// ���˺�ѡ�ļ��б�Ӧ�ú�����
	vector<filesystem::path> files_to_backup;
	for (const auto& file : candidate_files) {
		if (!is_blacklisted(file, sourcePath, originalSourcePath, config.blacklist)) {
			files_to_backup.push_back(file);
			//console.AddLog("%s", wstring_to_utf8(file.wstring()).c_str());
		}
	}

	// ������˺�û���ļ���Ҫ���ݣ�����ǰ����
	if (files_to_backup.empty()) {
		console.AddLog(L("LOG_NO_CHANGE_FOUND"));
		if (config.hotBackup) {
			filesystem::remove_all(sourcePath); 
		}
		return;
	}

	// �������ļ��б�д����ʱ�ļ�����7z��ȡ
	filesystem::path tempDir = filesystem::temp_directory_path() / L"MineBackup_Filelist";
	filesystem::create_directories(tempDir);
	wstring filelist_path = (tempDir / (L"_filelist.txt")).wstring();

	wofstream ofs(filelist_path);
	if (ofs.is_open()) {
		// ʹ��UTF-8����д���ļ��б�
		ofs.imbue(locale(ofs.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
		for (const auto& file : files_to_backup) {
			// д������ڱ���Դ�����·��
			ofs << filesystem::relative(file, sourcePath).wstring() << endl;
		}
		ofs.close();
		{
			HANDLE h = CreateFileW(filelist_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (h != INVALID_HANDLE_VALUE) {
				FlushFileBuffers(h); // ǿ�ưѻ�������д��
				CloseHandle(h);
			}
		}
	}
	else {
		console.AddLog("[Error] Failed to create temporary file list for 7-Zip.");
		if (config.hotBackup) {
			filesystem::remove_all(sourcePath);
		}
		return;
	}

	wstring backupTypeStr; // ������ʷ��¼
	wstring basedOnBackupFile; // ����Ԫ���ݼ�¼���ܱ��ݻ��ڵ����������ļ�

	if (config.backupMode == 1 || forceFullBackup) // ��ͨ����
	{
		backupTypeStr = L"Full";
		archivePath = destinationFolder + L"\\" + L"[Full][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
		command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
			L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" \"" + archivePath + L"\"" + L" @" + filelist_path;
		// ��������
		basedOnBackupFile = filesystem::path(archivePath).filename().wstring();
	}
	else if (config.backupMode == 2) // ���ܱ���
	{
		backupTypeStr = L"Smart";
		
		if (files_to_backup.empty()) {
			console.AddLog(L("LOG_NO_CHANGE_FOUND"));
			if (config.hotBackup) // �������
				filesystem::remove_all(sourcePath);
			return; // û�б仯��ֱ�ӷ���
		}

		console.AddLog(L("LOG_BACKUP_SMART_INFO"), files_to_backup.size());

		// ���ܱ�����Ҫ�ҵ��������ڵ��ļ�
		// �����ͨ���ٴζ�ȡԪ���ݻ�ã�GetChangedFiles �ڲ��Ѿ���֤��������
		nlohmann::json oldMetadata;
		ifstream f(metadataFolder + L"\\metadata.json");
		oldMetadata = nlohmann::json::parse(f);
		basedOnBackupFile = utf8_to_wstring(oldMetadata.at("lastBackupFile"));

		// 7z ֧���� @�ļ��� �ķ�ʽ����ָ��Ҫѹ�����ļ���������Ҫ���ݵ��ļ�·��д��һ���ı��ļ����ⳬ��cmd 8191�޳�
		archivePath = destinationFolder + L"\\" + L"[Smart][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;

		command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
			L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" \"" + archivePath + L"\"" + L" @" + filelist_path;
	}
	else if (config.backupMode == 3) // ���Ǳ��� - v1.7.8 ��ʱ�Ƴ�����ģʽ�ĺ���������
	{
		backupTypeStr = L"Overwrite";
		console.AddLog(L("LOG_OVERWRITE"));
		filesystem::path latestBackupPath;
		auto latest_time = filesystem::file_time_type{}; // Ĭ�Ϲ��������Сʱ��㣬����Ҫ::min()
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
			command = L"\"" + config.zipPath + L"\" u \"" + latestBackupPath.wstring() + L"\" \"" + sourcePath + L"\\*\" -mx=" + to_wstring(config.zipLevel);
			archivePath = latestBackupPath.wstring(); // ��¼�����µ��ļ�
		}
		else {
			console.AddLog(L("LOG_NO_BACKUP_FOUND"));
			archivePath = destinationFolder + L"\\" + L"[Full][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
			command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
				L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" -spf \"" + archivePath + L"\"" + L" \"" + sourcePath + L"\\*\"";
			// -spf ǿ��ʹ������·����-spf2 ʹ�����·��
		}
	}
	// �ں�̨�߳���ִ������
	if (RunCommandInBackground(command, console, config.useLowPriority, sourcePath)) // ����Ŀ¼���ܶ���
	{
		console.AddLog(L("LOG_BACKUP_END_HEADER"));

		// �����ļ���С���
		try {
			if (filesystem::exists(archivePath)) {
				uintmax_t fileSize = filesystem::file_size(archivePath);
				// ��ֵ����Ϊ 10 KB
				if (fileSize < 10240) {
					console.AddLog(L("BACKUP_FILE_TOO_SMALL_WARNING"), wstring_to_utf8(filesystem::path(archivePath).filename().wstring()).c_str());
					// �㲥һ������
					BroadcastEvent("event=backup_warning;type=file_too_small;");
				}
			}
		}
		catch (const filesystem::filesystem_error& e) {
			console.AddLog("[Error] Could not check backup file size: %s", e.what());
		}

		LimitBackupFiles(destinationFolder, config.keepCount, &console);
		UpdateMetadataFile(metadataFolder, filesystem::path(archivePath).filename().wstring(), basedOnBackupFile, currentState);
		// ��ʷ��¼
		if (realConfigIndex != -1)
			AddHistoryEntry(realConfigIndex, world.first, filesystem::path(archivePath).filename().wstring(), backupTypeStr, comment);
		else
			AddHistoryEntry(currentConfigIndex, world.first, filesystem::path(archivePath).filename().wstring(), backupTypeStr, comment);
		realConfigIndex = -1; // ����
		// �㲥һ���ɹ��¼�
		string payload = "event=backup_success;config=" + to_string(currentConfigIndex) + ";world=" + wstring_to_utf8(world.first) + ";file=" + wstring_to_utf8(filesystem::path(archivePath).filename().wstring());
		BroadcastEvent(payload);

		// ��ͬ���߼�
		if (config.cloudSyncEnabled && !config.rclonePath.empty() && !config.rcloneRemotePath.empty()) {
			console.AddLog(L("CLOUD_SYNC_START"));
			wstring rclone_command = L"\"" + config.rclonePath + L"\" copy \"" + archivePath + L"\" \"" + config.rcloneRemotePath + L"/" + world.first + L"\" --progress";
			// ����һ���߳���ִ����ͬ��������������������
			thread([rclone_command, &console, config]() {
				RunCommandInBackground(rclone_command, console, config.useLowPriority);
				console.AddLog(L("CLOUD_SYNC_FINISH"));
				}).detach();
		}
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
void DoOthersBackup(const Config config, filesystem::path backupWhat, const wstring& comment) {
	console.AddLog(L("LOG_BACKUP_OTHERS_START"));

	filesystem::path saveRoot(config.saveRoot);
	
	filesystem::path othersPath = backupWhat;
	backupWhat = backupWhat.filename().wstring(); // ֻ���������ļ�����

	//filesystem::path modsPath = saveRoot.parent_path() / "mods";

	if (!filesystem::exists(othersPath) || !filesystem::is_directory(othersPath)) {
		console.AddLog(L("LOG_ERROR_OTHERS_NOT_FOUND"), wstring_to_utf8(othersPath.wstring()).c_str());
		console.AddLog(L("LOG_BACKUP_OTHERS_END"));
		return;
	}

	filesystem::path destinationFolder;
	wstring archiveNameBase;

	destinationFolder = filesystem::path(config.backupPath) / backupWhat;
	archiveNameBase = backupWhat;

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
		console.AddLog(L("LOG_BACKUP_OTHERS_END"));
		return;
	}

	wstring command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
		L" -mmt" + (config.cpuThreads == 0 ? L"" : to_wstring(config.cpuThreads)) + L" \"" + archivePath + L"\"" + L" \"" + othersPath.wstring() + L"\\*\"";

	if (RunCommandInBackground(command, console, config.useLowPriority)) {
		LimitBackupFiles(destinationFolder.wstring(), config.keepCount, &console);
		// ������������ӵ���ʷ
		AddHistoryEntry(currentConfigIndex, backupWhat, filesystem::path(archivePath).filename().wstring(), backupWhat, comment);
	}

	console.AddLog(L("LOG_BACKUP_OTHERS_END"));
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
// restoreMethod: 0=Clean Restore, 1=Overwrite Restore, 2=�����µ�ѡ�����򸲸ǻ�ԭ
void DoRestore(const Config config, const wstring& worldName, const wstring& backupFile, Console& console, int restoreMethod, const string& customRestoreList) {
	console.AddLog(L("LOG_RESTORE_START_HEADER"));
	console.AddLog(L("LOG_RESTORE_PREPARE"), wstring_to_utf8(worldName).c_str());
	console.AddLog(L("LOG_RESTORE_USING_FILE"), wstring_to_utf8(backupFile).c_str());

	// ���7z.exe�Ƿ����
	if (!filesystem::exists(config.zipPath)) {
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND"), wstring_to_utf8(config.zipPath).c_str());
		console.AddLog(L("LOG_ERROR_7Z_NOT_FOUND_HINT"));
		return;
	}

	// ׼��·��
	wstring sourceDir = config.backupPath + L"\\" + worldName;
	wstring destinationFolder = config.saveRoot + L"\\" + worldName;
	filesystem::path targetBackupPath = filesystem::path(sourceDir) / backupFile;

	// ��鱸���ļ��Ƿ����
	if ((backupFile.find(L"[Smart]") == wstring::npos && backupFile.find(L"[Full]") == wstring::npos) || !filesystem::exists(sourceDir + L"\\" + backupFile)) {
		console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(backupFile).c_str());
		return;
	}

	// ��ԭǰ��������Ƿ���������
	if (IsFileLocked(destinationFolder + L"\\session.lock")) {
		int msgboxID = MessageBoxW(
			NULL,
			utf8_to_wstring(L("RESTORE_OVER_RUNNING_WORLD_MSG")).c_str(),
			utf8_to_wstring(L("RESTORE_OVER_RUNNING_WORLD_TITLE")).c_str(),
			MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2
		);
		if (msgboxID == IDNO) {
			console.AddLog("[Info] Restore cancelled by user due to active game session.");
			return;
		}
	}

	if (restoreMethod == 0) {
		console.AddLog(L("LOG_DELETING_EXISTING_WORLD"), wstring_to_utf8(destinationFolder).c_str());
		bool deletion_ok = true;
		if (filesystem::exists(destinationFolder)) {
			try {
				for (const auto& entry : filesystem::directory_iterator(destinationFolder)) {
					// ʹ�� is_blacklisted �����ж��Ƿ��ڰ�������
					if (is_blacklisted(entry.path(), destinationFolder, destinationFolder, restoreWhitelist)) {
						console.AddLog(L("LOG_SKIPPING_WHITELISTED_ITEM"), wstring_to_utf8(entry.path().filename().wstring()).c_str());
						continue;
					}

					console.AddLog(L("LOG_DELETING_EXISTING_WORLD_ITEM"), wstring_to_utf8(entry.path().filename().wstring()).c_str());
					error_code ec;
					if (entry.is_directory()) {
						filesystem::remove_all(entry.path(), ec);
					}
					else {
						filesystem::remove(entry.path(), ec);
					}
					if (ec) {
						console.AddLog(L("LOG_DELETION_ERROR"), wstring_to_utf8(entry.path().filename().wstring()).c_str(), ec.message().c_str());
						deletion_ok = false; // ���ɾ��ʧ��
					}
				}
			}
			catch (const filesystem::filesystem_error& e) {
				console.AddLog("[Error] An exception occurred during pre-restore cleanup: %s.", e.what());
				deletion_ok = false;
			}
		}
		if (!deletion_ok) {
			console.AddLog(L("ERROR_CLEAN_RESTORE_FAILED"));
			return; // ��ֹ��ԭ�Ա�������
		}
	}

	// �ռ�������صı����ļ�
	vector<filesystem::path> backupsToApply;

	// ���Ŀ�����������ݣ�ֱ�ӻ�ԭ��
	if (backupFile.find(L"[Smart]") != wstring::npos) { // Ŀ������������
		// Ѱ�һ�������������
		filesystem::path baseFullBackup;
		auto baseFullTime = filesystem::file_time_type{};

		// ���������ԭ�����ҵ��������ڵ���������
		if (restoreMethod == 1 || restoreMethod == 0) {
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
			// �ռ��ӻ������ݵ�Ŀ�걸��֮���������������
			for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
				if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Smart]") != wstring::npos) {
					if (entry.last_write_time() > baseFullTime && entry.last_write_time() <= filesystem::last_write_time(targetBackupPath)) {
						backupsToApply.push_back(entry.path());
					}
				}
			}
		}
		else if(restoreMethod == 2) {
			// ����ԭ���������Smart���ݿ�ʼ��һֱ��Ŀ�걸��
			for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
				if (entry.is_regular_file()) { // ����Ҫ����Smart��Full��ȫ����ԭ��ȥ
					if (entry.last_write_time() > filesystem::last_write_time(targetBackupPath)) {
						backupsToApply.push_back(entry.path());
					}
				}
			}
		}
	}
	else { //�����������ݴ���
		backupsToApply.push_back(targetBackupPath);
	}

	// ��ʽ: "C:\7z.exe" x "Դѹ����·��" -o"Ŀ���ļ���·��" -y
	// 'x' ��ʾ��·����ѹ, '-o' ָ�����Ŀ¼, '-y' ��ʾ��������ʾ�ش��ǡ������縲���ļ���
	
	if (restoreMethod == 2)
	{
		// ��ʱ����������������ҪӦ�õı���
		sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
			return filesystem::last_write_time(a) > filesystem::last_write_time(b);
			});
	}
	else {
		// ��ʱ��˳������������ҪӦ�õı���
		sort(backupsToApply.begin(), backupsToApply.end(), [](const auto& a, const auto& b) {
			return filesystem::last_write_time(a) < filesystem::last_write_time(b);
			});
	}

	wstring filesToExtractStr;
	// �����Զ��廹ԭģʽ�¹����ļ��б�
	if (restoreMethod == 3 && !customRestoreList.empty()) {
		console.AddLog(L("LOG_CUSTOM_RESTORE_START"));
		stringstream ss(customRestoreList);
		string item;
		while (getline(ss, item, ',')) {
			item.erase(0, item.find_first_not_of(" \t\n\r"));
			item.erase(item.find_last_not_of(" \t\n\r") + 1);
			if (!item.empty()) {
				filesToExtractStr += L" \"" + utf8_to_wstring(item) + L"\"";
			}
		}
	}


	// ����ִ�л�ԭ
	for (size_t i = 0; i < backupsToApply.size(); ++i) {
		const auto& backup = backupsToApply[i];
		console.AddLog(L("RESTORE_STEPS"), i + 1, backupsToApply.size(), wstring_to_utf8(backup.filename().wstring()).c_str());
		wstring command = L"\"" + config.zipPath + L"\" x \"" + backup.wstring() + L"\" -o\"" + destinationFolder + L"\" -y" + filesToExtractStr;
		RunCommandInBackground(command, console, config.useLowPriority);
	}
	console.AddLog(L("LOG_RESTORE_END_HEADER"));
	BroadcastEvent("event=restore_success;config=" + to_string(currentConfigIndex) + ";world=" + wstring_to_utf8(worldName) + ";backup=" + wstring_to_utf8(backupFile));
	return;
}

// ������� worldIdx ��Ϊ key ���µĳ�ͻ��ʹ��{ configIdx, worldIdx }
void AutoBackupThreadFunction(int configIdx, int worldIdx, int intervalMinutes, Console* console, atomic<bool>& stop_flag) {
	auto key = make_pair(configIdx, worldIdx);
	console->AddLog(L("LOG_AUTOBACKUP_START"), worldIdx, intervalMinutes);

	while (true) {
		// �ȴ�ָ����ʱ�䣬��ÿ����һ���Ƿ���Ҫֹͣ
		for (int i = 0; i < intervalMinutes * 60; ++i) {
			// ���޸���ֱ�Ӽ�鴫���ԭ�����ã����������
			if (stop_flag) { // ���� stop_flag.load()
				console->AddLog(L("LOG_AUTOBACKUP_STOPPED"), worldIdx);
				return; // �̰߳�ȫ���˳�
			}
			this_thread::sleep_for(chrono::seconds(1));
		}

		// ����ڳ�ʱ��ĵȴ��󣬷�����Ҫֹͣ����ִ�б���ֱ���˳�
		if (stop_flag) {
			console->AddLog(L("LOG_AUTOBACKUP_STOPPED"), worldIdx);
			return;
		}

		// ʱ�䵽�ˣ���ʼ����
		console->AddLog(L("LOG_AUTOBACKUP_ROUTINE"), worldIdx);
		{
			lock_guard<mutex> lock(g_configsMutex);
			if (configs.count(configIdx) && worldIdx >= 0 && worldIdx < configs[configIdx].worlds.size()) {
				DoBackup(configs[configIdx], configs[configIdx].worlds[worldIdx], *console);
			}
			else {
				console->AddLog(L("ERROR_INVALID_WORLD_IN_TASK"), configIdx, worldIdx);
				// ������Ч���˳����Ƴ�
				lock_guard<mutex> lock2(g_task_mutex);
				if (g_active_auto_backups.count(key)) {
					g_active_auto_backups.erase(key);
				}
				return;
			}
		}
	}
}

void RunSpecialMode(int configId) {
	SpecialConfig spCfg;
	if (specialConfigs.count(configId)) {
		spCfg = specialConfigs[configId];
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
		if (!configs.count(task.configIndex) ||
			task.worldIndex < 0 ||
			task.worldIndex >= configs[task.configIndex].worlds.size())
		{
			ConsoleLog(&console, L("ERROR_INVALID_WORLD_IN_TASK"), task.configIndex, task.worldIndex);
			continue;
		}

		// ��������ר�����ã��ϲ��������ú��������ã�
		Config taskConfig = configs[task.configIndex];
		const auto& worldData = taskConfig.worlds[task.worldIndex];
		taskConfig.hotBackup = spCfg.hotBackup;
		taskConfig.zipLevel = spCfg.zipLevel;
		taskConfig.keepCount = spCfg.keepCount;
		taskConfig.cpuThreads = spCfg.cpuThreads;
		taskConfig.useLowPriority = spCfg.useLowPriority;
		//taskConfig.blacklist = spCfg.blacklist; ������ͨ���õĺ�����

		if (task.backupType == 0) { // ���� 0: һ���Ա���
			ConsoleLog(&console, L("TASK_QUEUE_ONETIME_BACKUP"), utf8_to_gbk(wstring_to_utf8(worldData.first)).c_str());
			realConfigIndex = task.configIndex;
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
					realConfigIndex = task.configIndex;
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
				specialConfigs[configId].autoExecute = false;
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
		lock_guard<mutex> lock(g_task_mutex);
		for (auto& kv : g_active_auto_backups) {
			kv.second.stop_flag = true;
		}
	}
	for (auto& kv : g_active_auto_backups) {
		if (kv.second.worker.joinable()) kv.second.worker.join();
	}
	g_active_auto_backups.clear();

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

void CheckForConfigConflicts() {
	lock_guard<mutex> lock(g_configsMutex);
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
		if (entries.size() > 1) { // ����ж������ʹ��ͬһ��������
			for (size_t i = 0; i < entries.size(); ++i) {
				for (size_t j = i + 1; j < entries.size(); ++j) { // �Ƚ�ÿ������
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
void ShowHistoryWindow(int& tempCurrentConfigIndex) {
	// ʹ��static�������־û�UI״̬
	static HistoryEntry* selected_entry = nullptr;
	static ImGuiTextFilter filter;
	static char rename_buf[MAX_PATH];
	static char comment_buf[512];
	static string original_comment; // ����֧�֡�ȡ�����༭
	static bool is_comment_editing = false;
	static HistoryEntry* entry_to_delete = nullptr;
	Config& cfg = configs[tempCurrentConfigIndex];

	ImGui::SetNextWindowSize(ImVec2(850, 600), ImGuiCond_FirstUseEver);

	if (!ImGui::Begin(L("HISTORY_WINDOW_TITLE"), &showHistoryWindow)) {
		ImGui::End();
		return;
	}

	// �����ڹرջ����øı�ʱ������ѡ����
	if (!showHistoryWindow || (selected_entry && g_history.find(tempCurrentConfigIndex) == g_history.end())) {
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
			if (configs.count(tempCurrentConfigIndex) && g_history.count(tempCurrentConfigIndex)) {
				auto& history_vec = g_history.at(tempCurrentConfigIndex);
				history_vec.erase(
					remove_if(history_vec.begin(), history_vec.end(),
						[&](const HistoryEntry& entry) {
							return !filesystem::exists(filesystem::path(configs[tempCurrentConfigIndex].backupPath) / entry.worldName / entry.backupFile);
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

	if (g_history.find(tempCurrentConfigIndex) == g_history.end() || g_history.at(tempCurrentConfigIndex).empty()) {
		ImGui::TextWrapped(L("HISTORY_EMPTY"));
	}
	else {
		auto& history_vec = g_history.at(tempCurrentConfigIndex);

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

					filesystem::path backup_path = filesystem::path(configs[tempCurrentConfigIndex].backupPath) / entry->worldName / entry->backupFile;
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

		filesystem::path backup_path = filesystem::path(configs[tempCurrentConfigIndex].backupPath) / selected_entry->worldName / selected_entry->backupFile;
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
				filesystem::path path_to_delete = filesystem::path(configs[tempCurrentConfigIndex].backupPath) / entry_to_delete->worldName / entry_to_delete->backupFile;
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

string ProcessCommand(const string& commandStr, Console* console) {
	stringstream ss(commandStr);
	string command;
	ss >> command;

	auto error_response = [&](const string& msg) {
		BroadcastEvent(L("KNOTLINK_COMMAND_ERROR") + msg);
		console->AddLog(L("KNOTLINK_COMMAND_ERROR"), command.c_str(), msg.c_str());
		return "ERROR:" + msg;
		};

	// ʹ�� lock_guard ȷ���ں����������ڷ��� configs ���̰߳�ȫ��
	lock_guard<mutex> lock(g_configsMutex);

	if (command == "LIST_CONFIGS") {
		string result = "OK:";
		for (const auto& pair : configs) {
			result += to_string(pair.first) + "," + pair.second.name + ";";
		}
		if (!result.empty()) result.pop_back(); // �Ƴ�����';'
		BroadcastEvent("event=list_configs;data=" + result);
		return result;
	}
	else if (command == "LIST_BACKUPS") {
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
		BroadcastEvent("event=list_backups;config=" + to_string(config_idx) + ";world=" + to_string(world_idx) + ";data=" + result);
		return result;
	}
	else if (command == "LIST_WORLDS") {
		int config_idx;
		if (!(ss >> config_idx) || configs.find(config_idx) == configs.end()) {
			BroadcastEvent(L("BROADCAST_CONFIG_INDEX_ERROR"));
			return error_response(L("BROADCAST_CONFIG_INDEX_ERROR"));
		}
		const auto& cfg = configs[config_idx];
		string result = "OK:";
		for (const auto& world : cfg.worlds) {
			result += wstring_to_utf8(world.first) + ";";
		}
		if (!result.empty()) result.pop_back(); // �Ƴ�����';'

		BroadcastEvent("event=list_worlds;config=" + to_string(config_idx) + ";data=" + result);
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

		SaveConfigs(); // �������
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
		getline(ss, comment_part); // ��ȡʣ�ಿ����Ϊע��
		if (!comment_part.empty() && comment_part.front() == ' ') comment_part.erase(0, 1); // ȥ��ǰ���ո�

		// �ȹ㲥��Ϣ��֪ͨģ���ȱ�������
		BroadcastEvent("event=backup_started;config=" + to_string(config_idx) + ";world=" + wstring_to_utf8(configs[config_idx].worlds[world_idx].first));
		
		// �ں�̨�߳���ִ�б��ݣ����������������
		thread([=]() {
			// �����߳����ٴμ�������Ϊ configs ���������߳��б��޸�
			lock_guard<mutex> thread_lock(g_configsMutex);
			if (configs.count(config_idx)) // ȷ��������Ȼ����
				DoBackup(configs[config_idx], configs[config_idx].worlds[world_idx], *console, utf8_to_wstring(comment_part));
			}).detach();
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
				filesystem::path tempPath = configs[config_idx].saveRoot;
				filesystem::path modsPath = tempPath.parent_path() / "mods";
				DoOthersBackup(configs[config_idx], modsPath, utf8_to_wstring(comment_part));
			}
			}).detach();
		BroadcastEvent("event=mods_backup_started;config=" + to_string(config_idx));
		console->AddLog(L("KNOTLINK_COMMAND_SUCCESS"), command.c_str());
		return "OK:Mods backup started.";
	}
	else if (command == "BACKUP_CURRENT") { // ֱ�ӵ��ñ����������е�����ĺ���
		BroadcastEvent("event=pre_hot_backup");
		TriggerHotkeyBackup();
		return "OK:Backup Started";
	}
	else if (command == "AUTO_BACKUP") {
		int config_idx, world_idx, interval_minutes;
		// ��������֤����Ĳ���
		if (!(ss >> config_idx >> world_idx >> interval_minutes) || configs.find(config_idx) == configs.end() || world_idx < 0 || world_idx >= configs[config_idx].worlds.size()) {
			std::string error_msg = "ERROR:Invalid arguments. Usage: AUTO_BACKUP <config_idx> <world_idx> <interval_minutes>";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		// ��֤���ʱ�����Ч��
		if (interval_minutes < 1) {
			std::string error_msg = "ERROR:Interval must be at least 1 minute.";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		const auto& world_name = configs[config_idx].worlds[world_idx].first;
		auto taskKey = std::make_pair(config_idx, world_idx);

		// ����Ƿ����������������У������ظ�����
		if (g_active_auto_backups.count(taskKey)) {
			string error_msg = "ERROR:An auto-backup task is already running for this world.";
			BroadcastEvent(error_msg);
			return error_msg;
		}

		// �����������µ��Զ���������
		console->AddLog("[KnotLink] Received command to start auto-backup for world '%s'.", wstring_to_utf8(world_name).c_str());

		// ��ȫ��Map�л�ȡ�򴴽�һ���µ�����ʵ��
		AutoBackupTask& task = g_active_auto_backups[taskKey];
		task.stop_flag = false; // ����ֹͣ���

		// �������̣߳����������б�Ҫ�Ĳ�����
		// ʹ�� std::ref �� stop_flag �����ô��ݸ��̣߳��Ա���Զ�̿�����ֹͣ��
		task.worker = thread(AutoBackupThreadFunction, config_idx, world_idx, interval_minutes, console, ref(task.stop_flag));
		// �����̣߳�ʹ���ں�̨�������У�����ָ��������̷��سɹ���Ϣ��
		task.worker.detach();

		// ����ɹ���Ϣ���㲥�¼�
		std::string success_msg = "OK:Auto-backup started for world '" + wstring_to_utf8(world_name) + "' with an interval of " + std::to_string(interval_minutes) + " minutes.";
		BroadcastEvent("event=auto_backup_started;config=" + std::to_string(config_idx) + ";world=" + wstring_to_utf8(configs[config_idx].worlds[world_idx].first) + ";interval=" + std::to_string(interval_minutes));
		console->AddLog("[KnotLink] %s", success_msg.c_str());

		return success_msg;
		}

	else if (command == "STOP_AUTO_BACKUP") {
			int config_idx, world_idx;
			// ��������֤����
			if (!(ss >> config_idx >> world_idx) || configs.find(config_idx) == configs.end() || world_idx < 0 || world_idx >= configs[config_idx].worlds.size()) {
				std::string error_msg = "ERROR:Invalid arguments. Usage: STOP_AUTO_BACKUP <config_idx> <world_idx>";
				BroadcastEvent(error_msg);
				return error_msg;
			}

			const auto& world_name = configs[config_idx].worlds[world_idx].first;
			auto taskKey = std::make_pair(config_idx, world_idx);

			// ʹ�û�������������
			std::lock_guard<std::mutex> lock(g_task_mutex);

			// ����ָ��������
			auto it = g_active_auto_backups.find(taskKey);
			if (it == g_active_auto_backups.end()) {
				std::string error_msg = "ERROR:No active auto-backup task found for this world.";
				BroadcastEvent(error_msg);
				return error_msg;
			}

			// ����ֹͣ�źŲ��ȴ��߳̽���
			console->AddLog("[KnotLink] Received command to stop auto-backup for world '%s'.", wstring_to_utf8(world_name).c_str());

			// a. ����ԭ��ֹͣ���Ϊtrue��֪ͨ�߳�Ӧ���˳���
			it->second.stop_flag = true;

			// b. �ȴ��߳�ִ����ϡ���Ϊ�߳̿�������ִ�б��ݻ��������ڣ�
			//    �������ﲻʹ��join()��������AutoBackupThreadFunction�ڲ���ѭ�����⵽stop_flag�������˳���
			//    ��MineBackup�������˳�ʱ����ͳһ��join�߼�ȷ�������̶߳��ѽ�����

			// c. �������б����Ƴ�������
			g_active_auto_backups.erase(it);

			// ����ɹ���Ϣ���㲥�¼�
			std::string success_msg = "OK:Auto-backup task for world '" + wstring_to_utf8(world_name) + "' has been stopped.";
			BroadcastEvent("event=auto_backup_stopped;config=" + std::to_string(config_idx) + ";world_idx=" + std::to_string(world_idx));
			console->AddLog("[KnotLink] %s", success_msg.c_str());

			return success_msg;
	}
	else if (command == "SHUT_DOWN_WORLD_SUCCESS") {
		isRespond = true;
		return "OK:Start Restore";
	}

	return "ERROR:Unknown command '" + command + "'.";
}

void ConsoleLog(Console* console, const char* format, ...) {
	lock_guard<mutex> lock(consoleMutex);

	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, IM_ARRAYSIZE(buf), format, args);
	buf[IM_ARRAYSIZE(buf) - 1] = 0;
	va_end(args);

	// ����ṩ�� Console ��������־��ӵ��� Items ��
	if (console) {
		console->AddLog("%s", buf);
	}

	// ʼ�մ�ӡ����׼���
	printf("%s\n", buf);
}

void TriggerHotkeyBackup() {
	console.AddLog(L("LOG_HOTKEY_BACKUP_TRIGGERED"));

	for (const auto& config_pair : configs) {
		int config_idx = config_pair.first;
		const Config& cfg = config_pair.second;

		for (int world_idx = 0; world_idx < cfg.worlds.size(); ++world_idx) {
			const auto& world = cfg.worlds[world_idx];
			wstring levelDatPath = cfg.saveRoot + L"\\" + world.first + L"\\session.lock";
			if(!filesystem::exists(levelDatPath)) { // û�� session.lock �ļ��������ǻ��Ұ�浵����Ҫ����db�ļ����µ������ļ�������û�б�������
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
	isRespond = false;
	console.AddLog(L("LOG_HOTKEY_RESTORE_TRIGGERED"));

	for (const auto& config_pair : configs) {
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
				if (!isRespond) {
					return ;
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
				isRespond = false;
				return;
			}
		}
	}
	isRespond = false;
	console.AddLog(L("LOG_NO_ACTIVE_WORLD_FOUND"));
}

void GameSessionWatcherThread() {
	console.AddLog(L("LOG_EXIT_WATCHER_START"));

	while (!g_stopExitWatcher) {
		map<pair<int, int>, wstring> currently_locked_worlds;

		{
			lock_guard<mutex> lock(g_configsMutex);
			
			for (const auto& config_pair : configs) {
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
			
			for (const auto& sp_config_pair : specialConfigs) {
				const SpecialConfig& sp_cfg = sp_config_pair.second;
				if (!sp_cfg.backupOnGameStart) continue;
				for (const auto& task : sp_cfg.tasks) {
					if (configs.count(task.configIndex) && task.worldIndex < configs[task.configIndex].worlds.size()) {
						const Config& base_cfg = configs[task.configIndex];
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
			lock_guard<mutex> lock(g_activeWorldsMutex);

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
				lock_guard<mutex> config_lock(g_configsMutex);
				for (const auto& backup_target : worlds_to_backup) {
					int config_idx = backup_target.first;
					int world_idx = backup_target.second;
					if (configs.count(config_idx) && world_idx < configs[config_idx].worlds.size()) {
						Config backupConfig = configs[config_idx];
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
				RemoveHistoryEntry(currentConfigIndex, path.filename().wstring());
			}
			else {
				console.AddLog(L("ERROR_FILE_NO_FOUND"), wstring_to_utf8(entryToDelete.backupFile).c_str());
				RemoveHistoryEntry(currentConfigIndex, path.filename().wstring());
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
	lock_guard<mutex> lock(g_configsMutex);
	vector<DisplayWorld> out;
	// ��ͨ������ͼ
	if (!specialSetting) {
		if (!configs.count(currentConfigIndex)) return out;
		const Config& src = configs[currentConfigIndex];
		for (int i = 0; i < (int)src.worlds.size(); ++i) {
			if (src.worlds[i].second == L"#") continue; // ���ر��
			DisplayWorld dw;
			dw.name = src.worlds[i].first;
			dw.desc = src.worlds[i].second;
			dw.baseConfigIndex = currentConfigIndex;
			dw.baseWorldIndex = i;
			dw.effectiveConfig = src; // Ĭ��ʹ�û�������
			out.push_back(dw);
		}
		return out;
	}

	// ����������ͼ���� SpecialConfig.tasks ӳ��Ϊ DisplayWorld �б�
	if (!specialConfigs.count(currentConfigIndex)) return out;
	const SpecialConfig& sp = specialConfigs[currentConfigIndex];
	for (const auto& task : sp.tasks) {
		if (!configs.count(task.configIndex)) continue;
		const Config& baseCfg = configs[task.configIndex];
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

static int CreateNewNormalConfig(const string& name_hint) {
	int newId = nextConfigId++;
	Config new_cfg;
	new_cfg.name = name_hint;
	// Ĭ�Ͽյ�·��/����
	new_cfg.saveRoot.clear();
	new_cfg.backupPath.clear();
	new_cfg.worlds.clear();
	// ����Ĭ��ֵ���ڴ�����
	configs[newId] = new_cfg;
	return newId;
}

static int CreateNewSpecialConfig(const string& name_hint) {
	int newId = nextConfigId++;
	SpecialConfig sp;
	sp.name = name_hint;
	specialConfigs[newId] = sp;
	return newId;
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