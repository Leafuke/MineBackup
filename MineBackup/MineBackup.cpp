// Dear ImGui: standalone example application for DirectX 11

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

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

#include <string>
#include <map>
using namespace std;
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
std::vector<std::string> savePaths = { "" };
char backupPath[256] = "C:\\Users\\User\\Documents\\Minecraft备份";
char zipPath[256] = "C:\\Program Files\\7-Zip\\7z.exe";
int compressLevel = 5;

// 每个配置块的结构
struct Config {
    std::string saveRoot;
    std::vector<std::pair<std::string, std::string>> worlds; // {name, desc}
    std::string backupPath;
    std::string zipPath;
    std::string zipFormat;
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
string SelectFileDialog(HWND hwndOwner = NULL) {
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
                std::wstring wpath(path);
                CoTaskMemFree(path);
                psi->Release();
                return std::string(wpath.begin(), wpath.end());
            }
        }
        pfd->Release();
    }
    return "";
}


//选择文件夹
static string SelectFolderDialog(HWND hwndOwner = NULL) {
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
                return string(wpath.begin(), wpath.end());
            }
        }
        pfd->Release();
    }
    return "";
}

// 读取配置文件
static void LoadConfigs(const std::string& filename = "config.ini") {
    configs.clear();
    std::ifstream in(filename);
    if (!in.is_open()) return;

    std::string line, section;
    Config* cur = nullptr;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            if (section == "General") { cur = nullptr; }
            else if (section.rfind("Config", 0) == 0) {
                int idx = std::stoi(section.substr(6));
                configs[idx] = Config();
                cur = &configs[idx];
            }
            continue;
        }
        if (section == "General") {
            auto pos = line.find('=');
            if (pos != std::string::npos && line.rfind("当前使用配置编号=", 0) == 0) {
                currentConfigIndex = std::stoi(line.substr(pos + 1));
            }
        }
        else if (cur) {
            // 键值
            auto pos = line.find('=');
            if (pos == std::string::npos) continue;
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);

            if (key == "存档路径") {
                cur->saveRoot = val;
                // 自动扫描子目录为世界名
                if (std::filesystem::exists(val)) {
                    for (auto& entry : std::filesystem::directory_iterator(val)) {
                        if (entry.is_directory())
                            cur->worlds.push_back({ entry.path().filename().string(), "" });
                    }
                }
            }
            else if (key.find("存档名称+存档描述") == 0) {
                // 多行直到 '*'
                while (std::getline(in, line) && line != "*") {
                    std::string name = line;
                    if (!std::getline(in, line) || line == "*") break;
                    std::string desc = line;
                    cur->worlds.push_back({ name, desc });
                }
            }
            else if (key == "备份路径") cur->backupPath = val;
            else if (key == "压缩程序") cur->zipPath = val;
            else if (key == "压缩格式") cur->zipFormat = val;
            else if (key == "压缩等级") cur->zipLevel = std::stoi(val);
            else if (key == "保留数量") cur->keepCount = std::stoi(val);
            else if (key == "智能备份") cur->smartBackup = (val != "0");
            else if (key == "备份前还原") cur->restoreBefore = (val != "0");
            else if (key == "置顶窗口") cur->topMost = (val != "0");
            else if (key == "手动选择还原") cur->manualRestore = (val != "0");
            else if (key == "显示过程") cur->showProgress = (val != "0");
        }
    }
}

// 当前正在编辑的配置引用
Config& cfg = configs[currentConfigIndex];

bool showSettings = false;

// 保存
static void SaveConfigs(const std::string& filename = "config.ini") {
    std::ofstream out(filename);
    if (!out.is_open()) {
        MessageBoxA(nullptr, u8"无法写入 config.ini！", "错误", MB_OK | MB_ICONERROR);
        return;
    }
    out << u8"[General]\n";
    out << u8"当前使用配置编号=" << currentConfigIndex << "\n\n";
    for (auto& kv : configs) {
        int idx = kv.first;
        Config& c = kv.second;
        out << u8"[Config" << idx << "]\n";
        out << u8"存档路径=" << c.saveRoot << "\n";
        out << u8"存档名称+存档描述(一行名称一行描述)=\n";
        for (auto& p : c.worlds)
            out << p.first << "\n" << p.second << "\n";
        out << u8"*\n";
        out << u8"备份路径=" << c.backupPath << "\n";
        out << u8"压缩程序=" << c.zipPath << "\n";
        out << u8"压缩格式=" << c.zipFormat << "\n";
        out << u8"压缩等级=" << c.zipLevel << "\n";
        out << u8"保留数量=" << c.keepCount << "\n";
        out << u8"智能备份=" << (c.smartBackup ? 1 : 0) << "\n";
        out << u8"备份前还原=" << (c.restoreBefore ? 1 : 0) << "\n";
        out << u8"置顶窗口=" << (c.topMost ? 1 : 0) << "\n";
        out << u8"手动选择还原=" << (c.manualRestore ? 1 : 0) << "\n";
        out << u8"显示过程=" << (c.showProgress ? 1 : 0) << "\n\n";
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
static string GbkToUtf8(const string& gbk)
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
    char rootBuf[256];
    strncpy_s(rootBuf, cfg.saveRoot.c_str(), sizeof(rootBuf));
    ImGui::InputText(u8"存档根路径", rootBuf, 256);
    if (ImGui::Button(u8"选择")) {
        std::string sel = SelectFolderDialog();
        if (!sel.empty()) strncpy_s(rootBuf, sel.c_str(), 256);
    }
    cfg.saveRoot = rootBuf;

    // 自动扫描 worlds（可选刷新按钮）
    if (ImGui::Button(u8"扫描存档")) {
        cfg.worlds.clear();
        if (std::filesystem::exists(cfg.saveRoot))
            for (auto& e : std::filesystem::directory_iterator(cfg.saveRoot))
                if (e.is_directory())
                    cfg.worlds.push_back({ e.path().filename().string(), "" });
    }

    // 每个存档的名称+描述编辑
    ImGui::Separator();
    ImGui::Text(u8"存档名称与描述：");
    for (size_t i = 0; i < cfg.worlds.size(); ++i) {
        ImGui::PushID(int(i));
        char name[256], desc[512];
        string nameTemp = "00000000000000000000000000000000", descTemp = "00000000000000000000000000000000"; //用迂回方式解决GBK和Uint_8问题
        strncpy_s(name, cfg.worlds[i].first.c_str(), sizeof(name));
        strncpy_s(desc, cfg.worlds[i].second.c_str(), sizeof(desc));
        
        for (int j = 0;; ++j) //sizeof(name)一直是256
        {
            if (name[j] == '\0')
                break;
            nameTemp[j] = name[j];
        }
        /*for (int i = 0; i < sizeof(desc); ++i)
            descTemp[i] = desc[i];*/
        //nameTemp = GbkToUtf8(nameTemp);
        //descTemp = GbkToUtf8(descTemp);
        for (int j = 0;; ++j) //sizeof(name)一直是256
        {
            if (name[j] == '\0')
                break;
            name[j] = nameTemp[j];
        }
        /*for (int i = 0; i < sizeof(desc); ++i)
            desc[i] = descTemp[i];*/
        ImGui::InputText(u8"名称", name, 256);
        ImGui::InputText(u8"描述", desc, 512);
        cfg.worlds[i].first = name;
        cfg.worlds[i].second = desc;
        ImGui::PopID();
    }

    // 其他设置项
    char buf[256];
    strncpy_s(buf, cfg.backupPath.c_str(), sizeof(buf));
    ImGui::InputText(u8"备份路径", buf, 256);
    if (ImGui::Button(u8"选择")) {
        std::string sel = SelectFolderDialog();
        if (!sel.empty()) strncpy_s(buf, sel.c_str(), 256);
    }
    cfg.backupPath = buf;

    strncpy_s(buf, cfg.zipPath.c_str(), sizeof(buf));
    ImGui::InputText(u8"压缩程序", buf, 256);
    if (ImGui::Button(u8"选择")) {
        std::string sel = SelectFileDialog();
        if (!sel.empty()) strncpy_s(buf, sel.c_str(), 256);
    }
    cfg.zipPath = buf;

    ImGui::InputText(u8"压缩格式", &cfg.zipFormat[0], 16);
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


// 设置界面窗口old
/*
void ShowSettingsWindow() {
    if (!showSettingsWindow) return;

    ImGui::Begin(u8"设置", &showSettingsWindow, ImGuiWindowFlags_AlwaysAutoResize);

    // 动态输入多个存档路径
    ImGui::Text(u8"多个存档路径：");
    for (size_t i = 0; i < savePaths.size(); ++i) {
        char buf[256];
        strncpy_s(buf, savePaths[i].c_str(), sizeof(buf));
        if (ImGui::InputText((u8"路径 " + std::to_string(i + 1)).c_str(), buf, sizeof(buf))) {
            savePaths[i] = buf;
        }
    }
    if (ImGui::Button(u8"添加存档路径")) {
        savePaths.push_back("");
    }

    // 其他设置项
    ImGui::InputText(u8"备份位置", backupPath, sizeof(backupPath));
    ImGui::InputText(u8"压缩软件路径", zipPath, sizeof(zipPath));
    ImGui::SliderInt(u8"压缩等级", &compressLevel, 0, 9);

    // 保存配置按钮
    if (ImGui::Button(u8"保存配置")) {
        SaveConfigs();
        MessageBoxA(nullptr, "配置已保存到 config.ini", "提示", MB_OK | MB_ICONINFORMATION);
    }

    ImGui::End();
}*/

//文件存在判断
static bool Exists(string &files) {
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
        //IMGUI_DEMO_MARKER("Examples/Console");???
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
    string fileName = "config.ini";
    bool isFirstRun = !Exists(fileName);
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
                    std::string selected = SelectFolderDialog();
                    if (!selected.empty())
                        strncpy_s(backupPath, selected.c_str(), sizeof(backupPath));
                }
                ImGui::InputText(u8"备份路径", backupPath, IM_ARRAYSIZE(backupPath));
                if (ImGui::Button(u8"下一步")) page++;
            }
            else if (page == 3) {
                ImGui::Text(u8"第3步：配置压缩程序");
                if (ImGui::Button(u8"选择 7z.exe")) {
                    std::string selected = SelectFileDialog(); // 支持选择 .exe 文件
                    if (!selected.empty())
                        strncpy_s(zipPath, selected.c_str(), sizeof(zipPath));
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

        // 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
        /*if (show_demo_window)
        {
            ShowExampleAppConsole(&show_demo_window);
            static int page = 0;
            static char savesPath[256] = "C:\\Users\\User\\AppData\\Roaming\\.minecraft\\saves";
            static char backupPath[256] = "C:\\Users\\User\\Documents\\Minecraft备份";
            static char zipPath[256] = "C:\\Program Files\\7-Zip\\7z.exe";

            ImGui::Begin(u8"首次启动设置向导", &show_demo_window, ImGuiWindowFlags_AlwaysAutoResize);

            if (page == 0) {
                ImGui::Text(u8"欢迎使用 Minecraft 存档备份工具！");
                ImGui::Separator();
                ImGui::TextWrapped(u8"本工具用于帮助您自动备份 Minecraft 的存档文件。");
                if (ImGui::Button(u8"开始配置")) page++;
            }

            else if (page == 1) {
                static char folderPath[256] = "";
                ImGui::Text(u8"第1步：选择 Minecraft 存档路径");
                if (ImGui::Button(u8"选择存档路径")) {
                    std::string selected = SelectFolderDialog();  // WinAPI 文件夹对话框
                    if (!selected.empty()) {
                        strncpy_s(folderPath, selected.c_str(), sizeof(folderPath));
                    }
                }
                ImGui::InputText(u8"存档路径", folderPath, IM_ARRAYSIZE(folderPath));

                if (ImGui::Button(u8"下一步")) page++;
            }

            else if (page == 2) {
                ImGui::Text(u8"第2步：选择备份文件夹路径");
                ImGui::InputText(u8"备份路径", backupPath, IM_ARRAYSIZE(backupPath));
                if (ImGui::Button(u8"下一步")) page++;
            }

            else if (page == 3) {
                ImGui::Text(u8"第3步：配置压缩程序");
                ImGui::InputText(u8"7z路径", zipPath, IM_ARRAYSIZE(zipPath));
                if (ImGui::Button(u8"完成配置")) {
                    // Save to config file...
                    page++;
                }
            }

            else if (page == 4) {
                ImGui::Text(u8"配置完成！");
                ImGui::Text(u8"您现在可以开始使用程序。");
                if (ImGui::Button(u8"关闭向导")) {}
            }

            ImGui::End();

            /*static bool window_open = true;
            if (window_open)
                ImGui::Begin(u8"小窗子", &window_open, ImGuiWindowFlags_MenuBar);
            else
                return 0;
            if (ImGui::Button(u8"关闭小窗子"))
            {
                aaa = 1;
                cout << "关闭小窗子" << endl;
            }
            if (aaa == 1)
            {
                ImGui::Text(u8"小窗子已关闭");
            }
            else
            {
                ImGui::Text(u8"小窗子未关闭");
            }
            static int aaa = 0; //如果用int就无法选择了
            static bool bbb = false; // 如果用bool就可以选择了
            ImGui::SmallButton(u8"小按钮"); // 小按钮
            ImGui::InvisibleButton(u8"不可见按钮", ImVec2(100, 50)); // 不可见按钮
            ImGui::ArrowButton(u8"箭头按钮", ImGuiDir_Right); // 箭头按钮
            ImGui::ProgressBar(0.5f, ImVec2(0.0f, 0.0f), u8"进度条"); // 进度条
            ImGui::BulletText(u8"这是一个项目符号文本"); // 项目符号文本
            ImGui::TextLinkOpenURL(u8"这是一个链接", "https://www.dearimgui.com"); // 链接文本
            ImGui::RadioButton(u8"选项1", &aaa, 0);
            ImGui::RadioButton(u8"选项2", &aaa, 1);
            ImGui::RadioButton(u8"选项3", &aaa, 2);
            ImGui::Checkbox(u8"选项4", &bbb); // 复选框
            if (bbb)
            {
                ImGui::Text(u8"你选择了选项4");
            }
            else
            {
                ImGui::Text(u8"你没有选择选项4");
            }
            switch (aaa)
            {
            case 0: {
                ImGui::Text(u8"你选择了选项1");
                break;
                }
            case 1: {
                ImGui::Text(u8"你选择了选项2");
                break;
            }
            default:
                break;
            }
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), u8"这是红色文字");
            ImGui::Text("Hello, world!"); // Display some text (you can use a format strings too)
            ImGui::Text("This is a simple example of Dear ImGui with DirectX11.");
            ImGui::Text("You can use this as a starting point for your own applications.");
            ImGui::Text("Press ESC to close the application.");
            ImGui::End();
        }
        /*    ImGui::ShowDemoWindow(&show_demo_window);

        // 2. Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.
        {
            static float f = 0.0f;
            static int counter = 0;

            ImGui::Begin("Hello, world!");                          // Create a window called "Hello, world!" and append into it.

            ImGui::Text("This is some useful text.");               // Display some text (you can use a format strings too)
            ImGui::Checkbox("Demo Window", &show_demo_window);      // Edit bools storing our window open/close state
            ImGui::Checkbox("Another Window", &show_another_window);

            ImGui::SliderFloat("float", &f, 0.0f, 1.0f);            // Edit 1 float using a slider from 0.0f to 1.0f
            ImGui::ColorEdit3("clear color", (float*)&clear_color); // Edit 3 floats representing a color

            if (ImGui::Button("Button"))                            // Buttons return true when clicked (most widgets return true when edited/activated)
                counter++;
            ImGui::SameLine();
            ImGui::Text("counter = %d", counter);

            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
            ImGui::End();
        }

        // 3. Show another simple window.
        if (show_another_window)
        {
            ImGui::Begin("Another Window", &show_another_window);   // Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
            ImGui::Text("Hello from another window!");
            if (ImGui::Button("Close Me"))
                show_another_window = false;
            ImGui::End();
        }*/
        
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
    std::cin.get(); // Wait for user input before exiting
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
