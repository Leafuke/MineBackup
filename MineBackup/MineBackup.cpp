#include "imgui-all.h"
#include "i18n.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <locale>
#include <codecvt>
#include <fcntl.h>
#include <io.h>
#include <thread>
#include <atomic> // 用于线程安全的标志
#include <mutex>  // 用于互斥锁
#define constant1 256
#define constant2 512
using namespace std;
int aaa = 0;
// Data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// 声明辅助函数
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 设置项变量（全局）
vector<wstring> savePaths = { L"" };
wchar_t backupPath[constant1] = L"";
wchar_t zipPath[constant1] = L"";
int compressLevel = 5;
bool showSettings = false;

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
    bool restoreBefore;
    bool topMost;
    bool manualRestore;
    bool showProgress;
    int theme = 1;
    int folderNameType = 0;
    wstring themeColor;
};

// 全部配置
wstring Fontss;
int currentConfigIndex = 1;
map<int, Config> configs;
ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

// 放在全局变量区域
struct AutoBackupTask {
    thread worker;
    atomic<bool> stop_flag{ false }; // 原子布尔值，用于安全地通知线程停止
};

static map<int, AutoBackupTask> g_active_auto_backups; // key: worldIndex, value: task
static mutex g_task_mutex; // 专门用于保护上面 g_active_auto_backups 的互斥锁

string wstring_to_utf8(const wstring& wstr);
wstring utf8_to_wstring(const string& str);
string GbkToUtf8(const string& gbk);

void SaveStateFile(const filesystem::path& metadataPath);
bool Extract7zToTempFile(wstring& extractedPath);
wstring SelectFileDialog(HWND hwndOwner = NULL);
wstring SelectFolderDialog(HWND hwndOwner = NULL);

vector<filesystem::path> GetChangedFiles(const filesystem::path& worldPath, const filesystem::path& metadataPath);
size_t CalculateFileHash(const filesystem::path& filepath);
string GetRegistryValue(const string& keyPath, const string& valueName);
wstring GetLastOpenTime(const wstring& worldPath);
wstring GetLastBackupTime(const wstring& backupDir);

inline void ApplyTheme(int theme)
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

// 读取配置文件
static void LoadConfigs(const string& filename = "config.ini") {
    configs.clear();
    ifstream in(filename, ios::binary);
    if (!in.is_open()) return;
    string line1;
    wstring line, section;
    // cur作为一个指针，指向 configs 这个全局 map<int, Config> 中的元素 Config
    Config* cur = nullptr;
    while (getline(in, line1)) {
        line = utf8_to_wstring(line1);
        if (line.empty() || line.front() == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = line.substr(1, line.size() - 2);
            if (section == L"General") {
                cur = nullptr;
            }
            else if (section.find(L"Config", 0) == 0) {
                int idx = stoi(section.substr(6));
                configs[idx] = Config();
                cur = &configs[idx];
            }
        } else {
            auto pos = line.find(L'=');
            if (pos == wstring::npos) continue;
            wstring key = line.substr(0, pos);
            wstring val = line.substr(pos + 1);

            if (cur) { // Inside a [ConfigN] section
                if (key == L"SavePath") {
                    cur->saveRoot = val;
                    if (filesystem::exists(val)) {
                        for (auto& entry : filesystem::directory_iterator(val)) {
                            if (entry.is_directory())
                                cur->worlds.push_back({ entry.path().filename().wstring(), L"" });
                        }
                    }
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
                }
                else if (key == L"BackupPath") cur->backupPath = val;
                else if (key == L"ZipProgram") cur->zipPath = val;
                else if (key == L"ZipFormat") cur->zipFormat = val;
                else if (key == L"ZipLevel") cur->zipLevel = stoi(val);
                else if (key == L"KeepCount") cur->keepCount = stoi(val);
                else if (key == L"SmartBackup") cur->backupMode = stoi(val);
                else if (key == L"RestoreBeforeBackup") cur->restoreBefore = (val != L"0");
                else if (key == L"HotBackup") cur->hotBackup = (val != L"0");
                else if (key == L"TopMost") cur->topMost = (val != L"0");
                else if (key == L"ManualRestore") cur->manualRestore = (val != L"0");
                else if (key == L"ShowProgress") cur->showProgress = (val != L"0");
                else if (key == L"BackupNaming") cur->folderNameType = stoi(val);
                else if (key == L"Theme") {
                    cur->theme = stoi(val);
                    ApplyTheme(cur->theme);
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
            }
            else if (section == L"General") { // Inside [General] section
                if (key == L"CurrentConfig") {
                    currentConfigIndex = stoi(val);
                }
                else if (key == L"Language") {
                    g_CurrentLang = wstring_to_utf8(val);
                }
            }
        }
    }
}

// 保存
static void SaveConfigs(const wstring& filename = L"config.ini") {
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
    out << L"Language=" << utf8_to_wstring(g_CurrentLang) << L"\n\n";

    for (auto& kv : configs) {
        int idx = kv.first;
        Config& c = kv.second;
        out << L"[Config" << idx << L"]\n";
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
        out << L"KeepCount=" << c.keepCount << L"\n";
        out << L"SmartBackup=" << c.backupMode << L"\n";
        out << L"RestoreBeforeBackup=" << (c.restoreBefore ? 1 : 0) << L"\n";
        out << L"HotBackup=" << (c.hotBackup ? 1 : 0) << L"\n";
        out << L"TopMost=" << (c.topMost ? 1 : 0) << L"\n";
        out << L"ManualRestore=" << (c.manualRestore ? 1 : 0) << L"\n";
        out << L"ShowProgress=" << (c.showProgress ? 1 : 0) << L"\n";
        out << L"Theme=" << c.theme << L"\n";
        out << L"Font=" << c.zipFonts << L"\n";
        out << L"ThemeColor=" << c.themeColor << L"\n";
        out << L"BackupNaming=" << c.folderNameType << L"\n\n";
    }
}

//设置窗口
void ShowSettingsWindow() {
    if (!showSettings) return;
    ImGui::Begin(L("SETTINGS"), &showSettings, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SeparatorText(L("CONFIG_MANAGEMENT"));

    if (configs.empty()) {
        configs[1] = Config();
        currentConfigIndex = 1;
    }
    Config& cfg = configs[currentConfigIndex];

    // 1. Config selection dropdown
    string current_config_label = string(L("CONFIG_N")) + to_string(currentConfigIndex);
    if (ImGui::BeginCombo(L("CURRENT_CONFIG"), current_config_label.c_str())) {
        for (auto const& [idx, val] : configs) {
            const bool is_selected = (currentConfigIndex == idx);
            string label = string(L("CONFIG_N")) + to_string(idx);

            if (ImGui::Selectable(label.c_str(), is_selected)) {
                currentConfigIndex = idx;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // 2. Add and delete buttons
    if (ImGui::Button(L("BUTTON_ADD_CONFIG"))) {
        int new_index = configs.empty() ? 1 : configs.rbegin()->first + 1;
        configs[new_index] = Config(); // Create default config
        currentConfigIndex = new_index; // Switch to the new one

        Config& new_cfg = configs[currentConfigIndex];
        new_cfg.zipFormat = L"7z";
        new_cfg.zipLevel = 5;
        new_cfg.keepCount = 10;
        new_cfg.backupMode = 1;
        new_cfg.hotBackup = false;
        new_cfg.restoreBefore = false;
        new_cfg.topMost = false;
        new_cfg.manualRestore = true;
        new_cfg.showProgress = true;
        if (g_CurrentLang == "zh-CN")
            new_cfg.zipFonts = L"C:\\Windows\\Fonts\\msyh.ttc";
        else
            new_cfg.zipFonts = L"C:\\Windows\\Fonts\\SegoeUI.ttf";
        new_cfg.themeColor = L"0.45 0.55 0.60 1.00";
    }
    ImGui::SameLine();
    if (ImGui::Button(L("BUTTON_DELETE_CONFIG"))) {
        if (configs.size() > 1) { // Keep at least one config
            ImGui::OpenPopup(L("CONFIRM_DELETE_TITLE"));
        }
    }

    // Deletion confirmation popup
    if (ImGui::BeginPopupModal(L("CONFIRM_DELETE_TITLE"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(L("CONFIRM_DELETE_MSG"), currentConfigIndex);
        ImGui::Separator();
        if (ImGui::Button(L("BUTTON_OK"), ImVec2(120, 0))) {
            configs.erase(currentConfigIndex);
            currentConfigIndex = configs.begin()->first;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(L("BUTTON_CANCEL"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SeparatorText(L("CURRENT_CONFIG_DETAILS"));

    // Language selection
    int lang_idx = 0;
    for (int i = 0; i < IM_ARRAYSIZE(lang_codes); ++i) {
        if (g_CurrentLang == lang_codes[i]) {
            lang_idx = i;
            break;
        }
    }
    if (ImGui::Combo(L("LANGUAGE"), &lang_idx, langs, IM_ARRAYSIZE(langs))) {
        g_CurrentLang = lang_codes[lang_idx];
        //ReloadFonts(); // Reload fonts for the new language
    }


    // Saves root path
    char rootBufA[constant1];
    strncpy_s(rootBufA, wstring_to_utf8(cfg.saveRoot).c_str(), sizeof(rootBufA));
    if (ImGui::Button(L("BUTTON_SELECT_SAVES_DIR"))) {
        wstring sel = SelectFolderDialog();
        if (!sel.empty()) {
            cfg.saveRoot = sel;
        }
    }
    ImGui::SameLine();
    if (ImGui::InputText(L("SAVES_ROOT_PATH"), rootBufA, constant1)) {
        cfg.saveRoot = utf8_to_wstring(rootBufA);
    }

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
        char name[constant1], desc[constant2];
        strncpy_s(name, wstring_to_utf8(cfg.worlds[i].first).c_str(), sizeof(name));
        strncpy_s(desc, wstring_to_utf8(cfg.worlds[i].second).c_str(), sizeof(desc));

        if (ImGui::InputText(L("WORLD_NAME"), name, constant1))
            cfg.worlds[i].first = utf8_to_wstring(name);
        if (ImGui::InputText(L("WORLD_DESC"), desc, constant2))
            cfg.worlds[i].second = utf8_to_wstring(desc);

        ImGui::PopID();
    }

    // Other settings
    char buf[constant1];
    strncpy_s(buf, wstring_to_utf8(cfg.backupPath).c_str(), sizeof(buf));
    if (ImGui::Button(L("BUTTON_SELECT_BACKUP_DIR"))) {
        wstring sel = SelectFolderDialog();
        if (!sel.empty()) {
            cfg.backupPath = sel;
        }
    }
    ImGui::SameLine();
    if (ImGui::InputText(L("BACKUP_DEST_PATH_LABEL"), buf, constant1)) {
        cfg.backupPath = utf8_to_wstring(buf);
    }

    char zipBuf[constant1];
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
    if (ImGui::InputText(L("7Z_PATH_LABEL"), zipBuf, constant1)) {
        cfg.zipPath = utf8_to_wstring(zipBuf);
    }

    // Compression format
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

    ImGui::SliderInt(L("COMPRESSION_LEVEL"), &cfg.zipLevel, 0, 9);
    ImGui::InputInt(L("BACKUPS_TO_KEEP"), &cfg.keepCount);
    ImGui::Checkbox(L("RESTORE_BEFORE_BACKUP"), &cfg.restoreBefore); ImGui::SameLine();
    ImGui::Checkbox(L("IS_HOT_BACKUP"), &cfg.hotBackup); ImGui::SameLine();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(L("TIP_HOT_BACKUP"));
    }
    ImGui::Checkbox(L("ALWAYS_ON_TOP"), &cfg.topMost); ImGui::SameLine();
    ImGui::Checkbox(L("MANUAL_RESTORE_SELECT"), &cfg.manualRestore); ImGui::SameLine();
    ImGui::Checkbox(L("SHOW_PROGRESS"), &cfg.showProgress);

    ImGui::Separator();
    ImGui::Text(L("BACKUP_NAMING"));
    int folder_name_choice = (int)cfg.folderNameType;
    if (ImGui::RadioButton(L("NAME_BY_WORLD"), &folder_name_choice, 0)) { cfg.folderNameType = 0; } ImGui::SameLine();
    if (ImGui::RadioButton(L("NAME_BY_DESC"), &folder_name_choice, 1)) { cfg.folderNameType = 1; }

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
    char Fonts[constant1];
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
    if (ImGui::InputText("##zipFontsValue", Fonts, constant1)) {
        cfg.zipFonts = utf8_to_wstring(Fonts);
        Fontss = cfg.zipFonts;
        //ReloadFonts();
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    if (ImGui::Button(L("BUTTON_SAVE_AND_CLOSE"), ImVec2(120, 0))) {
        wchar_t colorBuf[64];
        swprintf(colorBuf, 64, L"%.2f %.2f %.2f %.2f", clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        cfg.themeColor = colorBuf;
        SaveConfigs();
        showSettings = false;
    }
    ImGui::End();
}

struct Console
{
    char                  InputBuf[constant1];
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
        Commands.push_back("CLASSIFY");
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
                if (strstr(item, "[error]")) { color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); has_color = true; }
                else if (strncmp(item, "# ", 2) == 0 || strncmp(item, "[INFO] ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
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

    void    ExecCommand(const char* command_line)
    {
        AddLog("# %s\n", command_line);

        // Insert into history. First find match and delete it so it can be pushed to the back.
        // This isn't trying to be smart or optimal.
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
            AddLog(L("CONSOLE_CMD_UNKNOWN"), command_line);
        }

        // On command input, we scroll to bottom even if AutoScroll==false
        ScrollToBottom = true;
    }

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
                // Multiple matches. Complete as much as we can..
                // So inputting "C"+Tab will complete to "CL" then display "CLEAR" and "CLASSIFY" as matches.
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
};

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

// 限制备份文件数量，超出则自动删除最旧的
void LimitBackupFiles(const wstring& folderPath, int limit, Console* console = nullptr)
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
    for (int i = 0; i < to_delete; ++i) {
        try {
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
bool RunCommandInBackground(wstring command, Console& console, const wstring& workingDirectory = L"") {
    // CreateProcessW需要一个可写的C-style字符串，所以我们将wstring复制到vector<wchar_t>
    vector<wchar_t> cmd_line(command.begin(), command.end());
    cmd_line.push_back(L'\0'); // 添加字符串结束符

    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // 隐藏子进程的窗口

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

// 执行单个世界的备份操作。
// 参数:
//   - config: 当前使用的配置。
//   - world:  要备份的世界（名称+描述）。
//   - console: 监控台对象的引用，用于输出日志信息。
void DoBackup(const Config config, const pair<wstring, wstring> world, Console& console) {
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

    // 生成带时间戳的文件名
    time_t now = time(0);
    tm ltm;
    localtime_s(&ltm, &now);
    wchar_t timeBuf[80];
    wcsftime(timeBuf, sizeof(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);
    archivePath = destinationFolder + L"\\" + L"[" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;

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
    if(forceFullBackup)
        console.AddLog(L("LOG_FORCE_FULL_BACKUP"));

    if (config.backupMode == 1 || forceFullBackup) // 普通备份
    {
        archivePath = destinationFolder + L"\\" + L"[Full][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
        command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
            L" \"" + archivePath + L"\"" + L" \"" + sourcePath + L"\\*\"";
    }
    else if (config.backupMode == 2) // 智能备份
    {
        
        vector<filesystem::path> filesToBackup = GetChangedFiles(sourcePath, metadataFolder);

        if (filesToBackup.empty()) {
            console.AddLog(L("LOG_NO_CHANGE_FOUND"));
            if (config.hotBackup) // 清理快照
                filesystem::remove_all(sourcePath);
            return; // 没有变化，直接返回
        }

        console.AddLog(L("LOG_BACKUP_SMART_INFO"), filesToBackup.size());

        // 7z 支持用 @文件名 的方式批量指定要压缩的文件。把所有要备份的文件路径写到一个文本文件避免超过cmd 8191限长
        filesystem::path tempDir = filesystem::temp_directory_path() / L"MineBackup_Snapshot";
        if(!filesystem::exists(tempDir))
            filesystem::create_directories(tempDir);
        wofstream ofs(tempDir.string() + "\\7z.txt");
        ofs.imbue(locale(ofs.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
        for (const auto& file : filesToBackup) {
            ofs << file.wstring().substr(originalSourcePath.size() + 1) << endl; // 输出相对路径，以及，必须utf8！
        }

        ofs.close();
        archivePath = destinationFolder + L"\\" + L"[Smart][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
        
        command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx="
            + to_wstring(config.zipLevel) + L" \"" + archivePath + L"\" @" + tempDir.wstring() + L"\\7z.txt";
    }
    else if (config.backupMode == 3) // 覆盖备份
    {
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
            // 2. A previous backup was found. Use the 7-Zip 'u' (update) command.
            console.AddLog(L("LOG_FOUND_LATEST"), wstring_to_utf8(latestBackupPath.filename().wstring()).c_str());
            command = L"\"" + config.zipPath + L"\" u \"" + latestBackupPath.wstring() + L"\" \"" + sourcePath + L"\\*\" -mx=" + to_wstring(config.zipLevel);
        }
        else {
            // 3. No previous backup found. Perform a normal full backup.
            console.AddLog(L("LOG_NO_BACKUP_FOUND"));
            archivePath = destinationFolder + L"\\" + L"[Full][" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;
            command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
                L"-spf \"" + archivePath + L"\"" + L" \"" + sourcePath + L"\\*\"";
            // -spf 强制使用完整路径，-spf2 使用相对路径
        }
    }
    // 在后台线程中执行命令
    if (RunCommandInBackground(command, console, originalSourcePath))
    {
        console.AddLog(L("LOG_BACKUP_END_HEADER"));
        LimitBackupFiles(destinationFolder, config.keepCount, &console);
        SaveStateFile(metadataFolder);
    }
    filesystem::remove_all("7z.txt");
    if (config.hotBackup && sourcePath != (config.saveRoot + L"\\" + world.first)) {
        console.AddLog(L("LOG_CLEAN_SNAPSHOT"));
        error_code ec;
        filesystem::remove_all(sourcePath, ec);
        if (ec) console.AddLog(L("LOG_WARNING_CLEAN_SNAPSHOT"), ec.message().c_str());
    }
}

// 执行单个世界的还原操作。
// 参数:
//   - config: 当前使用的配置。
//   - worldName: 要还原的世界名。
//   - backupFile: 要用于还原的备份文件名。
//   - console: 监控台对象的引用，用于输出日志信息。
void DoRestore(const Config config, const wstring& worldName, const wstring& backupFile, Console& console) {
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

    // 收集所有相关的备份文件
    vector<filesystem::path> backupsToApply;
    filesystem::path targetBackupPath = filesystem::path(sourceDir) / backupFile;

    // 如果目标是完整备份，直接还原它
    if(backupFile.find(L"[Smart]") != wstring::npos) { // 目标是增量备份
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
            console.AddLog("[error] Cannot restore: No suitable [Full] backup found before the selected [Smart] backup.");
            return;
        }

        console.AddLog("[INFO] Found base full backup: %s", wstring_to_utf8(baseFullBackup.filename().wstring()).c_str());
        backupsToApply.push_back(baseFullBackup);

        // 收集从基础备份到目标备份之间的所有增量备份
        for (const auto& entry : filesystem::directory_iterator(sourceDir)) {
            if (entry.is_regular_file() && entry.path().filename().wstring().find(L"[Smart]") != wstring::npos) {
                if (entry.last_write_time() > baseFullTime && entry.last_write_time() <= filesystem::last_write_time(targetBackupPath)) {
                    backupsToApply.push_back(entry.path());
                }
            }
        }
    } else { //当成完整备份处理
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
        console.AddLog("[INFO] Restoring step %zu/%zu: %s", i + 1, backupsToApply.size(), wstring_to_utf8(backup.filename().wstring()).c_str());
        wstring command = L"\"" + config.zipPath + L"\" x \"" + backup.wstring() + L"\" -o\"" + destinationFolder + L"\" -y";
        RunCommandInBackground(command, console);
    }
    console.AddLog(L("LOG_RESTORE_END_HEADER"));
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

// Main code
int main(int, char**)
{
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    
    static Console console;

    // Create application window
    //ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"MineBackup - v1.5.4", WS_OVERLAPPEDWINDOW, 100, 100, 1000, 800, nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
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
        MAKEINTRESOURCE(IDI_ICON5),
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

    // Our state
    bool show_demo_window = true;
    bool show_another_window = false;
    bool errorShow = false;
    string fileName = "config.ini";
    bool isFirstRun = !filesystem::exists(fileName);
    static bool showConfigWizard = isFirstRun;
    static bool showMainApp = !isFirstRun;
    ImGui::StyleColorsLight();//默认亮色
    LoadConfigs(fileName);
//C:\Windows\Fonts\SegoeUI.ttf

    
    wstring g_7zTempPath;
    bool sevenZipExtracted = Extract7zToTempFile(g_7zTempPath);
    if (isFirstRun) {
        LANGID lang_id = GetUserDefaultUILanguage();

        if (lang_id == 2052 || lang_id == 1028) {
            g_CurrentLang = "zh-CN";
            Fontss = L"C:\\Windows\\Fonts\\msyh.ttc";
        }
        else {
            g_CurrentLang = "en-US"; //英文
            Fontss = L"C:\\Windows\\Fonts\\SegoeUI.ttf";
        }
    }
    if(g_CurrentLang == "zh-CN")
        ImFont* font = io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    else
        ImFont* font = io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f, nullptr, io.Fonts->GetGlyphRangesDefault());

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
            ::Sleep(10);
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

        if (showConfigWizard) {
            // 首次启动向导使用的静态变量
            static int page = 0;
            static char saveRootPath[constant1] = "";
            static char backupPath[constant1] = "";
            static char zipPath[constant1] = "C:\\Program Files\\7-Zip\\7z.exe"; // 提供一个常用默认值

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
                        initialConfig.keepCount = 10;
                        initialConfig.backupMode = 1;
                        initialConfig.hotBackup = false;
                        initialConfig.restoreBefore = false;
                        initialConfig.topMost = false;
                        initialConfig.manualRestore = true;
                        initialConfig.showProgress = true;
                        if (g_CurrentLang == "zh-CN")
                            initialConfig.zipFonts = L"C:\\Windows\\Fonts\\msyh.ttc";
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
        if (showMainApp) {
            // 用于跟踪用户在列表中选择的世界
            static int selectedWorldIndex = -1;
            // 用于弹出还原窗口
            static bool openRestorePopup = false;
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
            ImGui::Begin(L("MAIN_WINDOW_TITLE"));

            // --- 左侧面板：世界列表和操作 ---
            ImGui::BeginChild(L("LEFT_PANE"), ImVec2(ImGui::GetContentRegionAvail().x * 0.9f, 0), true);
            ImGui::SeparatorText(L("WORLD_LIST"));
            // 获取当前配置
            if (configs.find(currentConfigIndex) == configs.end()) {
                if (!configs.empty()) currentConfigIndex = configs.begin()->first;
                else configs[1] = Config(); // 新建
            }
            Config& cfg = configs[currentConfigIndex];

            if (ImGui::BeginTable("WorldListTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn(L("TABLE_WORLD_NAME"), ImGuiTableColumnFlags_WidthStretch, 0.3f);
                ImGui::TableSetupColumn(L("TABLE_WORLD_DESC"), ImGuiTableColumnFlags_WidthStretch, 0.4f);
                ImGui::TableSetupColumn(L("TABLE_WORLD_TIMES"), ImGuiTableColumnFlags_WidthFixed, 160.0f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < cfg.worlds.size(); ++i) {
                    ImGui::PushID(i); // 为当前循环迭代创建一个唯一的ID作用域

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();

                    bool is_selected = (selectedWorldIndex == i);
                    // 为了避免Selectable的标签冲突，可以使用隐藏标签"##label"的技巧
                    // 这样即使世界名称为空，也不会有问题
                    if (ImGui::Selectable(wstring_to_utf8(cfg.worlds[i].first).c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        selectedWorldIndex = i;
                    }

                    ImGui::TableNextColumn();
                    ImGui::Text("%s", wstring_to_utf8(cfg.worlds[i].second).c_str());
                    ImGui::TableNextColumn();
                    wstring worldFolder = cfg.saveRoot + L"\\" + cfg.worlds[i].first;
                    wstring backupFolder = cfg.backupPath + L"\\" + cfg.worlds[i].first;
                    wstring openTime = GetLastOpenTime(worldFolder);
                    wstring backupTime = GetLastBackupTime(backupFolder);
                    ImGui::Text("%s\n%s", wstring_to_utf8(openTime).c_str(), wstring_to_utf8(backupTime).c_str());

                    ImGui::PopID(); // 弹出ID作用域，为下一次循环做准备
                }
                ImGui::EndTable();
            }

            ImGui::Separator();

            // --- 核心操作按钮 ---
            // 如果没有选择任何世界，则禁用按钮
            bool no_world_selected = (selectedWorldIndex == -1);
            if (no_world_selected) {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button(L("BUTTON_BACKUP_SELECTED"), ImVec2(-1, 0))) {
                // 创建一个后台线程来执行备份，防止UI卡死
                thread backup_thread(DoBackup, cfg, cfg.worlds[selectedWorldIndex], ref(console));
                backup_thread.detach(); // 分离线程，让它在后台独立运行
            }

            if (ImGui::Button(L("BUTTON_AUTO_BACKUP_SELECTED"), ImVec2(-1, 0))) {
                // 只有选中了世界才能打开弹窗
                if (selectedWorldIndex != -1) {
                    ImGui::OpenPopup(L("AUTOBACKUP_SETTINGS"));
                }
            }

            if (ImGui::Button(L("BUTTON_RESTORE_SELECTED"), ImVec2(-1, 0))) {
                openRestorePopup = true;
                ImGui::OpenPopup(L("RESTORE_POPUP_TITLE"));
            }

            if (no_world_selected) {
                ImGui::EndDisabled();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), L("PROMPT_SELECT_WORLD"));
            }

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

                // 确认还原按钮
                bool no_backup_selected = (selectedBackupIndex == -1);
                if (no_backup_selected) ImGui::BeginDisabled();

                if (ImGui::Button(L("BUTTON_CONFIRM_RESTORE"), ImVec2(120, 0))) {
                    // 创建后台线程执行还原
                    thread restore_thread(DoRestore, cfg, cfg.worlds[selectedWorldIndex].first, backupFiles[selectedBackupIndex], ref(console));
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
            ImGui::SameLine();
            ImGui::BeginChild(L("RIGHT_PANE"));
            if (ImGui::Button(L("SETTINGS"))) showSettings = true;
            if (ImGui::Button(L("EXIT"))) done = true;
            ImGui::TextLinkOpenURL(L("CHECK_FOR_UPDATES"), "github.com/Leafuke/MineBackup/releases");
            ShowSettingsWindow();
            console.Draw(L("CONSOLE_TITLE"), &showMainApp);
            ImGui::EndChild();
            ImGui::End();
        }

        // Rendering渲染清理
        ImGui::Render();
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
    lock_guard<mutex> lock(g_task_mutex);
    for (auto& pair : g_active_auto_backups) {
        pair.second.stop_flag = true; // 通知线程停止
        if (pair.second.worker.joinable()) {
            pair.second.worker.join(); // 等待线程执行完毕
        }
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    // 清除临时的7zip
    if (!g_7zTempPath.empty()) {
        DeleteFileW(g_7zTempPath.c_str());
    }
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
