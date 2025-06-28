#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#pragma comment (lib,"d3d11.lib") 
//↑需要手动添加d3d11.lib库文件，否则编译会报错。
#include <windows.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <shobjidl.h>
#include <locale>
#include <fcntl.h>
#include <io.h>

#include <string>
#include <map>
using namespace std;
using std::wstring;
using std::wifstream;
using std::wofstream;
using std::wcout;
using std::wcin;
int aaa = 0;
// Data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 设置项变量（全局）
std::vector<wstring> savePaths = { L"" };
wchar_t backupPath[256] = L"C:\\Users\\User\\Documents\\Minecraft备份";
wchar_t zipPath[256] = L"C:\\Program Files\\7-Zip\\7z.exe";
int compressLevel = 5;

// 每个配置块的结构
struct Config {
    wstring saveRoot;
    std::vector<std::pair<wstring, wstring>> worlds; // {name, desc}
    wstring backupPath;
    wstring zipPath;
    wstring zipFormat;
    int zipLevel;
    int keepCount;
    bool smartBackup;
    bool restoreBefore;
    bool topMost;
    bool manualRestore;
    bool showProgress;
};

// 全部配置
int currentConfigIndex = 1;
std::map<int, Config> configs;


//选择文件
wstring SelectFileDialog(HWND hwndOwner = NULL) {
    IFileDialog* pfd;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
        IID_IFileDialog, reinterpret_cast<void**>(&pfd));

    if (SUCCEEDED(hr)) {
        hr = pfd->Show(hwndOwner);
        if (SUCCEEDED(hr)) {
            IShellItem* psi;
            hr = pfd->GetResult(&psi);
            if (SUCCEEDED(hr)) {
                PWSTR path = nullptr;
                psi->GetDisplayName(SIGDN_FILESYSPATH, &path);
                wstring wpath(path);
                CoTaskMemFree(path);
                psi->Release();
                return wpath; 
            }
        }
        pfd->Release();
    }
    return L"";
}


//选择文件夹
static wstring SelectFolderDialog(HWND hwndOwner = NULL) {
    IFileDialog* pfd;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
        IID_IFileDialog, reinterpret_cast<void**>(&pfd));

    if (SUCCEEDED(hr)) {
        DWORD options;
        pfd->GetOptions(&options);
        pfd->SetOptions(options | FOS_PICKFOLDERS); // 设置为选择文件夹
        hr = pfd->Show(hwndOwner);
        if (SUCCEEDED(hr)) {
            IShellItem* psi;
            hr = pfd->GetResult(&psi);
            if (SUCCEEDED(hr)) {
                PWSTR path = nullptr;
                psi->GetDisplayName(SIGDN_FILESYSPATH, &path);
                wstring wpath(path);
                CoTaskMemFree(path);
                psi->Release();
                return wpath;
            }
        }
        pfd->Release();
    }
    return L"";
}

// 辅助函数：wstring <-> utf8 string（使用WinAPI，兼容C++17+）
static std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}
static std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// 读取配置文件
static void LoadConfigs(const wstring& filename = L"config.ini") {
    configs.clear();
    wifstream in(filename, std::ios::binary);
    if (!in.is_open()) return;

    wstring line, section;
    Config* cur = nullptr;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = line.substr(1, line.size() - 2);
            if (section == L"General") { cur = nullptr; }
            else if (section.rfind(L"Config", 0) == 0) {
                int idx = std::stoi(section.substr(6));
                configs[idx] = Config();
                cur = &configs[idx];
            }
            continue;
        }
        if (section == L"General") {
            auto pos = line.find(L'=');
            if (pos != wstring::npos && line.rfind(L"当前使用配置编号=", 0) == 0) {
                currentConfigIndex = std::stoi(line.substr(pos + 1));
            }
        }
        else if (cur) {
            // 键值
            auto pos = line.find(L'=');
            if (pos == wstring::npos) continue;
            wstring key = line.substr(0, pos);
            wstring val = line.substr(pos + 1);

            if (key == L"存档路径") {
                cur->saveRoot = val;
                // 自动扫描子目录为世界名
                if (std::filesystem::exists(val)) {
                    for (auto& entry : std::filesystem::directory_iterator(val)) {
                        if (entry.is_directory())
                            cur->worlds.push_back({ entry.path().filename().wstring(), L"" });
                    }
                }
            }
            else if (key.find(L"存档名称+存档描述") == 0) {
                // 多行直到 '*'
                while (std::getline(in, line) && line != L"*") {
                    wstring name = line;
                    if (!std::getline(in, line) || line == L"*") break;
                    wstring desc = line;
                    cur->worlds.push_back({ name, desc });
                }
            }
            else if (key == L"备份路径") cur->backupPath = val;
            else if (key == L"压缩程序") cur->zipPath = val;
            else if (key == L"压缩格式") cur->zipFormat = val;
            else if (key == L"压缩等级") cur->zipLevel = std::stoi(val);
            else if (key == L"保留数量") cur->keepCount = std::stoi(val);
            else if (key == L"智能备份") cur->smartBackup = (val != L"0");
            else if (key == L"备份前还原") cur->restoreBefore = (val != L"0");
            else if (key == L"置顶窗口") cur->topMost = (val != L"0");
            else if (key == L"手动选择还原") cur->manualRestore = (val != L"0");
            else if (key == L"显示过程") cur->showProgress = (val != L"0");
        }
    }
}

// 当前正在编辑的配置引用
Config& cfg = configs[currentConfigIndex];

bool showSettings = false;

// 保存
static void SaveConfigs(const wstring& filename = L"config.ini") {
    wofstream out(filename, std::ios::binary);
    if (!out.is_open()) {
        MessageBoxW(nullptr, L"无法写入 config.ini！", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    out << L"[General]\n";
    out << L"当前使用配置编号=" << currentConfigIndex << L"\n\n";
    for (auto& kv : configs) {
        int idx = kv.first;
        Config& c = kv.second;
        out << L"[Config" << idx << L"]\n";
        out << L"存档路径=" << c.saveRoot << L"\n";
        out << L"存档名称+存档描述(一行名称一行描述)=\n";
        for (auto& p : c.worlds)
            out << p.first << L"\n" << p.second << L"\n";
        out << L"*\n";
        out << L"备份路径=" << c.backupPath << L"\n";
        out << L"压缩程序=" << c.zipPath << L"\n";
        out << L"压缩格式=" << c.zipFormat << L"\n";
        out << L"压缩等级=" << c.zipLevel << L"\n";
        out << L"保留数量=" << c.keepCount << L"\n";
        out << L"智能备份=" << (c.smartBackup ? 1 : 0) << L"\n";
        out << L"备份前还原=" << (c.restoreBefore ? 1 : 0) << L"\n";
        out << L"置顶窗口=" << (c.topMost ? 1 : 0) << L"\n";
        out << L"手动选择还原=" << (c.manualRestore ? 1 : 0) << L"\n";
        out << L"显示过程=" << (c.showProgress ? 1 : 0) << L"\n\n";
    }
    out.close();
}

// 主界面按钮触发设置窗口
void ShowMainUI() {
    if (ImGui::Button(u8"设置")) {
        showSettings = true;
    }
}

// 本地多字节编码（如GBK）转UTF-8
static std::string GbkToUtf8(const std::string& gbk)
{
    int lenW = MultiByteToWideChar(CP_ACP, 0, gbk.c_str(), -1, nullptr, 0);
    std::wstring wstr(lenW, 0);
    MultiByteToWideChar(CP_ACP, 0, gbk.c_str(), -1, &wstr[0], lenW);

    int lenU8 = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u8str(lenU8, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &u8str[0], lenU8, nullptr, nullptr);

    // 去掉末尾的\0
    if (!u8str.empty() && u8str.back() == '\0') u8str.pop_back();
    return u8str;
}

void ShowSettingsWindow() {
    if (!showSettings) return;
    ImGui::Begin(u8"设置", &showSettings, ImGuiWindowFlags_AlwaysAutoResize);

    // 存档根路径
    std::string rootBuf = wstring_to_utf8(cfg.saveRoot);
    char rootBufA[256];
    strncpy_s(rootBufA, rootBuf.c_str(), sizeof(rootBufA));
    ImGui::InputText(u8"存档根路径", rootBufA, 256);
    if (ImGui::Button(u8"选择")) {
        std::wstring sel = SelectFolderDialog();
        if (!sel.empty()) {
            std::string selu8 = wstring_to_utf8(sel);
            strncpy_s(rootBufA, selu8.c_str(), 256);
        }
    }
    cfg.saveRoot = utf8_to_wstring(rootBufA);

    // 自动扫描 worlds（可选刷新按钮）
    if (ImGui::Button(u8"扫描存档")) {
        cfg.worlds.clear();
        if (std::filesystem::exists(cfg.saveRoot))
            for (auto& e : std::filesystem::directory_iterator(cfg.saveRoot))
                if (e.is_directory())
                    cfg.worlds.push_back({ e.path().filename().wstring(), L"" });
    }

    // 每个存档的名称+描述编辑
    ImGui::Separator();
    ImGui::Text(u8"存档名称与描述：");
    for (size_t i = 0; i < cfg.worlds.size(); ++i) {
        ImGui::PushID(int(i));
        std::string nameA = wstring_to_utf8(cfg.worlds[i].first);
        std::string descA = wstring_to_utf8(cfg.worlds[i].second);
        char name[256], desc[512];
        strncpy_s(name, nameA.c_str(), sizeof(name));
        strncpy_s(desc, descA.c_str(), sizeof(desc));
        ImGui::InputText(u8"名称", name, 256);
        ImGui::InputText(u8"描述", desc, 512);
        cfg.worlds[i].first = utf8_to_wstring(name);
        cfg.worlds[i].second = utf8_to_wstring(desc);
        ImGui::PopID();
    }

    // 其他设置项
    std::string backupPathA = wstring_to_utf8(cfg.backupPath);
    char buf[256];
    strncpy_s(buf, backupPathA.c_str(), sizeof(buf));
    ImGui::InputText(u8"备份路径", buf, 256);
    if (ImGui::Button(u8"选择")) {
        std::wstring sel = SelectFolderDialog();
        if (!sel.empty()) {
            std::string selu8 = wstring_to_utf8(sel);
            strncpy_s(buf, selu8.c_str(), 256);
        }
    }
    cfg.backupPath = utf8_to_wstring(buf);

    std::string zipPathA = wstring_to_utf8(cfg.zipPath);
    strncpy_s(buf, zipPathA.c_str(), sizeof(buf));
    ImGui::InputText(u8"压缩程序", buf, 256);
    if (ImGui::Button(u8"选择")) {
        std::wstring sel = SelectFileDialog();
        if (!sel.empty()) {
            std::string selu8 = wstring_to_utf8(sel);
            strncpy_s(buf, selu8.c_str(), 256);
        }
    }
    cfg.zipPath = utf8_to_wstring(buf);

    std::string zipFormatA = wstring_to_utf8(cfg.zipFormat);
    char zipFormatBuf[16];
    strncpy_s(zipFormatBuf, zipFormatA.c_str(), sizeof(zipFormatBuf));
    ImGui::InputText(u8"压缩格式", zipFormatBuf, 16);
    cfg.zipFormat = utf8_to_wstring(zipFormatBuf);
    ImGui::SliderInt(u8"压缩等级", &cfg.zipLevel, 0, 9);
    ImGui::InputInt(u8"保留数量", &cfg.keepCount);

    ImGui::Checkbox(u8"智能备份", &cfg.smartBackup);
    ImGui::Checkbox(u8"备份前还原", &cfg.restoreBefore);
    ImGui::Checkbox(u8"置顶窗口", &cfg.topMost);
    ImGui::Checkbox(u8"手动选择还原", &cfg.manualRestore);
    ImGui::Checkbox(u8"显示过程", &cfg.showProgress);

    if (ImGui::Button(u8"保存并关闭")) {
        SaveConfigs();
        showSettings = false;
    }
    ImGui::End();
}

//文件存在判断
static bool Exists(wstring &files) {
    return std::filesystem::exists(files);
}

//自适应窗口
// Demonstrate creating a window which gets auto-resized according to its content.
static void ShowExampleAppAutoResize(bool* p_open)
{
    if (!ImGui::Begin("Example: Auto-resizing window", p_open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    //IMGUI_DEMO_MARKER("Examples/Auto-resizing window");

    static int lines = 10;
    ImGui::TextUnformatted(
        "Window will resize every-frame to the size of its content.\n"
        "Note that you probably don't want to query the window size to\n"
        "output your content because that would create a feedback loop.");
    ImGui::SliderInt("Number of lines", &lines, 1, 20);
    for (int i = 0; i < lines; i++)
        ImGui::Text("%*sThis is line %d", i * 4, "", i); // Pad with space to extend size horizontally
    ImGui::End();
}

// 控制台，演示版本——要改成监控台
struct Console
{
    char                  InputBuf[256];
    ImVector<char*>       Items;
    ImVector<const char*> Commands;
    ImVector<char*>       History;
    int                   HistoryPos;    // -1: new line, 0..History.Size-1 browsing history.
    ImGuiTextFilter       Filter;
    bool                  AutoScroll;
    bool                  ScrollToBottom;

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
        AddLog(u8"欢迎使用 MineBackup 状态监控台");
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
        for (int i = 0; i < Items.Size; i++)
            ImGui::MemFree(Items[i]);
        Items.clear();
    }

    //显示消息
    void    AddLog(const char* fmt, ...) IM_FMTARGS(2)
    {
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
        // Here we create a context menu only available from the title bar.
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem(u8"关闭"))
                *p_open = false;
            ImGui::EndPopup();
        }

        ImGui::TextWrapped(
            u8"在这个监控台中，你可以看到"
            u8"运行错误提示、存档备份状态、");
        ImGui::TextWrapped(u8"输入'HELP'来查看帮助");

        // TODO: display items starting from the bottom

        //if (ImGui::SmallButton("Add Debug Text")) { AddLog("%d some text", Items.Size); AddLog("some more text"); AddLog("display very important message here!"); }
        //ImGui::SameLine();
        /*if (ImGui::SmallButton("Add Debug Error")) { AddLog("[error] wrong"); }
        ImGui::SameLine();*/
        if (ImGui::SmallButton(u8"清空")) { ClearLog(); }
        ImGui::SameLine();
        bool copy_to_clipboard = ImGui::SmallButton(u8"复制");
        //static float t = 0.0f; if (ImGui::GetTime() - t > 0.02f) { t = ImGui::GetTime(); AddLog("Spam %f", t); }

        ImGui::Separator();

        // Options menu
        if (ImGui::BeginPopup(u8"选项"))
        {
            ImGui::Checkbox(u8"自动滚动", &AutoScroll);
            ImGui::EndPopup();
        }

        // Options, Filter
        ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_Tooltip);
        if (ImGui::Button(u8"选项"))
            ImGui::OpenPopup(u8"选项");
        ImGui::SameLine();
        Filter.Draw(u8"筛选器 (\"完成,提示\") (\"error\")", 180);
        ImGui::Separator();

        // Reserve enough left-over height for 1 separator + 1 input text
        const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_HorizontalScrollbar))
        {
            if (ImGui::BeginPopupContextWindow())
            {
                if (ImGui::Selectable("Clear")) ClearLog();
                ImGui::EndPopup();
            }

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
                else if (strncmp(item, "# ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
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
        if (ImGui::InputText("Input", InputBuf, IM_ARRAYSIZE(InputBuf), input_text_flags, &TextEditCallbackStub, (void*)this))
        {
            char* s = InputBuf;
            Strtrim(s);
            if (s[0])
                ExecCommand(s);
            strcpy_s(s, strlen(s)+1, "");//被要求从strcpy改成strcpy_s，这样中间要加个长度参数才不报错……
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
            AddLog("Unknown command: '%s'\n", command_line);
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
        //AddLog("cursor: %d, selection: %d-%d", data->CursorPos, data->SelectionStart, data->SelectionEnd);
        switch (data->EventFlag)
        {
        case ImGuiInputTextFlags_CallbackCompletion:
        {
            // Example of TEXT COMPLETION

            // Locate beginning of current word
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
                AddLog("No match for \"%.*s\"!\n", (int)(word_end - word_start), word_start);
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
                AddLog("Possible matches:\n");
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


//展示控制台窗口——需要在主函数中调用
static void ShowExampleAppConsole(bool* p_open)
{
    static Console console;
    console.Draw(u8"MineBackup 监控台", p_open);
}

// Main code
int main(int, char**)
{
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    //初始化

    // Create application window
    //ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"MineBackup - v1.5.0", WS_OVERLAPPEDWINDOW, 100, 100, 800, 500, nullptr, nullptr, wc.hInstance, nullptr);

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

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

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
    ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\msyh.ttc", 20.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    //IM_ASSERT(font != nullptr);

    // Our state
    bool show_demo_window = true;
    bool show_another_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    std::string fileName = "config.ini";
    bool isFirstRun = !std::filesystem::exists(fileName);
    static bool showConfigWizard = isFirstRun;
    static bool showMainApp = !isFirstRun;

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
        //if (done)
        //    break;

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
            static int page = 0;
            static std::vector<std::string> savePaths = { "" }; // 初始化时至少有一个
            static char backupPath[256] = "C:\\Users\\User\\Documents\\Minecraft备份";
            static char zipPath[256] = "C:\\Program Files\\7-Zip\\7z.exe";

            ImGui::Begin(u8"首次启动设置向导", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

            if (page == 0) {
                ImGui::Text(u8"欢迎使用 Minecraft 存档备份工具！");
                ImGui::Separator();
                ImGui::TextWrapped(u8"本工具用于帮助您自动备份 Minecraft 的存档文件。");
                if (ImGui::Button(u8"开始配置")) page++;
            }
            else if (page == 1) {
                ImGui::Text(u8"第1步：选择 Minecraft 存档路径");
                ImGui::Text(u8"请输入一个或多个 Minecraft 存档路径：");

                for (size_t i = 0; i < savePaths.size(); ++i) {
                    char buf[256];
                    strncpy_s(buf, savePaths[i].c_str(), sizeof(buf));
                    if (ImGui::InputText((u8"路径 " + std::to_string(i + 1)).c_str(), buf, sizeof(buf))) {
                        savePaths[i] = buf;
                    }
                }

                if (ImGui::Button(u8"添加路径")) {
                    savePaths.push_back("");
                }

                ImGui::Text(u8"当前输入了 %d 个路径", (int)savePaths.size());
                
                 
                if (ImGui::Button(u8"下一步")) page++;
            }
            else if (page == 2) {
                ImGui::Text(u8"第2步：选择备份文件夹路径");
                if (ImGui::Button(u8"选择备份路径")) {
                    std::wstring selected = SelectFolderDialog();
                    if (!selected.empty()) {
                        std::string selu8 = wstring_to_utf8(selected);
                        strncpy_s(backupPath, selu8.c_str(), sizeof(backupPath));
                    }
                }
                ImGui::InputText(u8"备份路径", backupPath, IM_ARRAYSIZE(backupPath));
                if (ImGui::Button(u8"下一步")) page++;
            }
            else if (page == 3) {
                ImGui::Text(u8"第3步：配置压缩程序");
                if (ImGui::Button(u8"选择 7z.exe")) {
                    std::wstring selected = SelectFileDialog(); // 支持选择 .exe 文件
                    if (!selected.empty()) {
                        std::string selu8 = wstring_to_utf8(selected);
                        strncpy_s(zipPath, selu8.c_str(), sizeof(zipPath));
                    }
                }
                ImGui::InputText(u8"7z 路径", zipPath, IM_ARRAYSIZE(zipPath));
                if (ImGui::Button(u8"完成配置")) {
                    SaveConfigs();
                    showConfigWizard = false;
                    showMainApp = true;
                }
            }

            ImGui::End();
        }
        if (showMainApp) {
            ImGui::Begin(u8"我的世界存档备份器");

            ImGui::Text(u8"欢迎回来！");
            ImGui::Text(u8"此处将显示备份管理界面。");
            ShowMainUI();         // 显示主界面上的“设置”按钮
            ShowSettingsWindow(); // 显示设置窗口
            if (ImGui::Button(u8"退出")) {
                done = true;
            }

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

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::wcin.get(); // Wait for user input before exiting
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
