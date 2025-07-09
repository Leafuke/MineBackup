#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"
#include "resource.h"
#include "resource2.h"
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
#include <codecvt>
#include <vector>
#include <fcntl.h>
#include <io.h>
#include <thread>
#include <string>
#include <map>
#include <atomic> // 用于线程安全的标志
#include <mutex>  // 用于互斥锁
#define constant1 256
#define constant2 512
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

// 每个配置块的结构 枚举 但是这么写其实没必要...
enum ThemeType { THEME_DARK = 0, THEME_LIGHT = 1, THEME_CLASSIC = 2 };
enum BackupFolderNameType { NAME_BY_WORLD = 0, NAME_BY_DESC = 1 };

struct Config {
    wstring saveRoot;
    std::vector<std::pair<wstring, wstring>> worlds; // {name, desc}
    wstring backupPath;
    wstring zipPath;
    wstring zipFormat;
    wstring zipFonts;
    int zipLevel;
    int keepCount;
    bool smartBackup;
    bool restoreBefore;
    bool topMost;
    bool manualRestore;
    bool showProgress;
    ThemeType theme = THEME_LIGHT;
    BackupFolderNameType folderNameType = NAME_BY_WORLD;
};

// 全部配置
wstring Fontss = L"c:\\Windows\\Fonts\\msyh.ttc";
int currentConfigIndex = 1;
map<int, Config> configs;

// 放在全局变量区域
struct AutoBackupTask {
    thread worker;
    atomic<bool> stop_flag{ false }; // 原子布尔值，用于安全地通知线程停止
};

static map<int, AutoBackupTask> g_active_auto_backups; // key: worldIndex, value: task
static mutex g_task_mutex; // 专门用于保护上面 g_active_auto_backups 的互斥锁


bool Extract7zToTempFile(wstring& extractedPath) {
    // 用主模块句柄
    HRSRC hRes = FindResource(GetModuleHandle(NULL), MAKEINTRESOURCE(IDR_EXE1), L"EXE");
    if (!hRes) return false;

    HGLOBAL hData = LoadResource(GetModuleHandle(NULL), hRes);
    if (!hData) return false;

    DWORD dataSize = SizeofResource(GetModuleHandle(NULL), hRes);
    if (dataSize == 0) return false;

    LPVOID pData = LockResource(hData);
    if (!pData) return false;

    wchar_t tempPath[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempPath)) return false;

    wchar_t tempFile[MAX_PATH];
    if (!GetTempFileNameW(tempPath, L"7z", 0, tempFile)) return false;

    // 改名为7z.exe
    std::wstring finalPath = tempFile;
    finalPath += L".exe";
    MoveFileW(tempFile, finalPath.c_str());

    HANDLE hFile = CreateFileW(finalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten;
    BOOL ok = WriteFile(hFile, pData, dataSize, &bytesWritten, nullptr);
    CloseHandle(hFile);
    if (!ok || bytesWritten != dataSize) {
        DeleteFileW(finalPath.c_str());
        return false;
    }

    extractedPath = finalPath;
    return true;
}

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

// 注册表查询
string GetRegistryValue(const string& keyPath, const string& valueName)
{
    HKEY hKey;
    string valueData;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buffer[1024];
        DWORD dataSize = sizeof(buffer);
        if (RegGetValueA(hKey, NULL, valueName.c_str(), RRF_RT_ANY, NULL, buffer, &dataSize) == ERROR_SUCCESS) {
            valueData = buffer;
        }
        RegCloseKey(hKey);
    }
    else
        return "";
    return valueData;
}

static wstring GetLastOpenTime(const wstring& worldPath) {
    try {
        auto ftime = filesystem::last_write_time(worldPath);
        // 转换为 system_clock::time_point
        auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
            ftime - filesystem::file_time_type::clock::now()
            + chrono::system_clock::now()
        );
        time_t cftime = chrono::system_clock::to_time_t(sctp);
        wchar_t buf[64];
        struct tm timeinfo;
        //wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtime要换成更安全的localtime
        if (localtime_s(&timeinfo, &cftime) == 0) { 
            wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
            return buf;
        }
        else {
            return L"未知";
        }
    }
    catch (...) {
        return L"未知";
    }
}

static wstring GetLastBackupTime(const wstring& backupDir) {
    time_t latest = 0;
    try {
        if (filesystem::exists(backupDir)) {
            for (const auto& entry : filesystem::directory_iterator(backupDir)) {
                if (entry.is_regular_file()) {
                    auto ftime = entry.last_write_time();
                    // 转换为 system_clock::time_point
                    auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
                        ftime - filesystem::file_time_type::clock::now()
                        + chrono::system_clock::now()
                    );
                    time_t cftime = chrono::system_clock::to_time_t(sctp);
                    if (cftime > latest) latest = cftime;
                }
            }
        }
        if (latest == 0) return L"无备份";
        wchar_t buf[64];
        struct tm timeinfo;
        //wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtime要换成更安全的localtime
        if (localtime_s(&timeinfo, &latest) == 0) {
            wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
            return buf;
        }
        else {
            return L"未知";
        }
    }
    catch (...) {
        return L"未知";
    }
}

// 辅助函数：wstring <-> utf8 string（使用WinAPI，兼容C++17+）本地多字节编码（GBK）转UTF-8
static string wstring_to_utf8(const wstring& wstr) {
    if (wstr.empty()) return string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
    string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}
static wstring utf8_to_wstring(const string& str) {
    if (str.empty()) return wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), NULL, 0);
    wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}
static string GbkToUtf8(const string& gbk)
{
    int lenW = MultiByteToWideChar(CP_ACP, 0, gbk.c_str(), -1, nullptr, 0);
    wstring wstr(lenW, 0);
    MultiByteToWideChar(CP_ACP, 0, gbk.c_str(), -1, &wstr[0], lenW);

    int lenU8 = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    string u8str(lenU8, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &u8str[0], lenU8, nullptr, nullptr);

    // 去掉末尾的\0
    if (!u8str.empty() && u8str.back() == '\0') u8str.pop_back();
    return u8str;
}

// 主题应用函数
void ApplyTheme(ThemeType theme) {
    switch (theme) {
    case THEME_DARK: ImGui::StyleColorsDark(); break;
    case THEME_LIGHT: ImGui::StyleColorsLight(); break;
    case THEME_CLASSIC: ImGui::StyleColorsClassic(); break;
    }
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
        if (line.empty()) continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = line.substr(1, line.size() - 2);
            if (section == L"General") {
                cur = nullptr;
                getline(in, line1);
                line = utf8_to_wstring(line1);
                auto pos = line.find(L'=');
                wcout << line;
                if (pos != wstring::npos && line.find(L"当前使用配置编号=", 0) == 0) {
                    currentConfigIndex = stoi(line.substr(pos + 1));
                }
            }
            else if (section.find(L"Config", 0) == 0) {
                int idx = stoi(section.substr(6));
                configs[idx] = Config();
                cur = &configs[idx];
            }
        }
        if (cur) {
            // 键值
            auto pos = line.find(L'=');
            if (pos == wstring::npos) continue;
            wstring key = line.substr(0, pos);
            wstring val = line.substr(pos + 1);

            if (key == L"存档路径") {
                cur->saveRoot = val;
                // 自动扫描子目录为世界名
                if (filesystem::exists(val)) {
                    for (auto& entry : filesystem::directory_iterator(val)) {
                        if (entry.is_directory())
                            cur->worlds.push_back({ entry.path().filename().wstring(), L"" });
                    }
                }
            }
            else if (key.find(L"存档名称+存档描述") == 0) {
                // 多行直到 '*'
                while (getline(in, line1) && line1 != "*") {
                    line = utf8_to_wstring(line1);
                    wstring name = line;
                    if (!getline(in, line1) || line1 == "*") break;
                    line = utf8_to_wstring(line1);
                    wstring desc = line;
                    cur->worlds.push_back({ name, desc });
                }
            }
            else if (key == L"备份路径") cur->backupPath = val;
            else if (key == L"压缩程序") cur->zipPath = val;
            else if (key == L"压缩格式") cur->zipFormat = val;
            else if (key == L"压缩等级") cur->zipLevel = stoi(val);
            else if (key == L"保留数量") cur->keepCount = stoi(val);
            else if (key == L"智能备份") cur->smartBackup = (val != L"0");
            else if (key == L"备份前还原") cur->restoreBefore = (val != L"0");
            else if (key == L"置顶窗口") cur->topMost = (val != L"0");
            else if (key == L"手动选择还原") cur->manualRestore = (val != L"0");
            else if (key == L"显示过程") cur->showProgress = (val != L"0");
            else if (key == L"主题") cur->theme = (ThemeType)stoi(val);
            else if (key == L"备份命名方式") cur->folderNameType = (BackupFolderNameType)stoi(val);
            else if (key == L"字体") {
                cur->zipFonts = val;
                Fontss = val;
            }
            ApplyTheme(cur->theme);
        }
    }
}

bool showSettings = false;

// 保存
static void SaveConfigs(const wstring& filename = L"config.ini") {
    wofstream out(filename, ios::binary);
    if (!out.is_open()) {
        MessageBoxW(nullptr, L"无法写入 config.ini！", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    //out.imbue(locale("chs"));//不能用这个，变ANSI啦
    out.imbue(locale(out.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
    //out.imbue(locale(out.getloc(), new codecvt_utf8<wchar_t>));//将UTF8转为UTF，现在C++17也不对了……但是我们有define！
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
        out << L"显示过程=" << (c.showProgress ? 1 : 0) << L"\n";
        out << L"主题=" << c.theme << L"\n";
        out << L"字体=" << c.zipFonts << L"\n";
        out << L"备份命名方式=" << c.folderNameType << L"\n";
    }
}

// 主界面按钮触发设置窗口
void ShowMainUI() {
    if (ImGui::Button(u8"设置")) {
        showSettings = true;
    }
}

//设置窗口
void ShowSettingsWindow() {
    if (!showSettings) return;
    ImGui::Begin(u8"设置", &showSettings, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SeparatorText(u8"配置管理");

    // 获取当前选中的配置，确保后续UI都作用于正确的配置上
    // 如果configs为空(例如，首次运行但跳过了向导)，则创建一个临时的
    if (configs.empty()) {
        configs[1] = Config();
        currentConfigIndex = 1;
    }
    Config& cfg = configs[currentConfigIndex];

    // 1. 配置选择下拉框
    string current_config_label = u8"配置 " + to_string(currentConfigIndex);
    if (ImGui::BeginCombo(u8"当前配置", current_config_label.c_str())) {
        for (auto const& [idx, val] : configs) {
            const bool is_selected = (currentConfigIndex == idx);
            string label = u8"配置 " + to_string(idx);
            if (ImGui::Selectable(label.c_str(), is_selected)) {
                currentConfigIndex = idx;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // 2. 添加和删除按钮
    if (ImGui::Button(u8"添加新配置")) {
        // 找到最大的现有ID，然后+1作为新ID
        int new_index = configs.empty() ? 1 : configs.rbegin()->first + 1;
        configs[new_index] = Config(); // 创建一个默认配置
        currentConfigIndex = new_index; // 自动切换到新创建的配置

        //设置初始值
        Config& cfg = configs[currentConfigIndex]; //很关键！
        cfg.zipFormat = L"7z";
        cfg.zipLevel = 5;
        cfg.keepCount = 10;
        cfg.smartBackup = true;
        cfg.restoreBefore = false;
        cfg.topMost = false;
        cfg.manualRestore = true;
        cfg.showProgress = true;
        cfg.zipFonts = L"c:\\Windows\\Fonts\\msyh.ttc";
    }
    ImGui::SameLine();
    if (ImGui::Button(u8"删除当前配置")) {
        if (configs.size() > 1) { // 至少保留一个配置
            ImGui::OpenPopup(u8"确认删除");
        }
    }

    // 删除确认弹窗的逻辑
    if (ImGui::BeginPopupModal(u8"确认删除", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(u8"您确定要删除“配置 %d”吗？\n此操作无法撤销！\n", currentConfigIndex);
        ImGui::Separator();
        if (ImGui::Button(u8"确定", ImVec2(120, 0))) {
            int old_index = currentConfigIndex;
            configs.erase(old_index);
            // 切换到剩下的第一个可用配置
            /*currentConfigIndex = configs.begin()->first;*/
            currentConfigIndex = 1; //这样不容易出错
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(u8"取消", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SeparatorText(u8"当前配置详情");

    // 存档根路径
    char rootBufA[constant1];
    strncpy_s(rootBufA, wstring_to_utf8(cfg.saveRoot).c_str(), sizeof(rootBufA));
    if (ImGui::Button(u8"选择存放存档的文件夹")) {
        wstring sel = SelectFolderDialog();
        if (!sel.empty()) {
            cfg.saveRoot = sel; // 直接赋值给当前配置
        }
    }
    ImGui::SameLine();
    if (ImGui::InputText(u8"存档根路径", rootBufA, constant1)) {
        cfg.saveRoot = utf8_to_wstring(rootBufA);
    }


    // 自动扫描 worlds（可选刷新按钮）
    if (ImGui::Button(u8"扫描存档（根据上方根路径）")) {
        cfg.worlds.clear();
        if (filesystem::exists(cfg.saveRoot))
            for (auto& e : filesystem::directory_iterator(cfg.saveRoot))
                if (e.is_directory())
                    cfg.worlds.push_back({ e.path().filename().wstring(), L"" });
    }

    // 每个存档的名称+描述编辑
    ImGui::Separator();
    ImGui::Text(u8"存档名称与描述：");
    for (size_t i = 0; i < cfg.worlds.size(); ++i) {
        ImGui::PushID(int(i));
        char name[constant1], desc[constant2];
        strncpy_s(name, wstring_to_utf8(cfg.worlds[i].first).c_str(), sizeof(name));
        strncpy_s(desc, wstring_to_utf8(cfg.worlds[i].second).c_str(), sizeof(desc));

        if (ImGui::InputText(u8"名称", name, constant1))
            cfg.worlds[i].first = utf8_to_wstring(name);
        if (ImGui::InputText(u8"描述", desc, constant2))
            cfg.worlds[i].second = utf8_to_wstring(desc);

        ImGui::PopID();
    }

    // 其他设置项
    char buf[constant1];
    strncpy_s(buf, wstring_to_utf8(cfg.backupPath).c_str(), sizeof(buf));
    if (ImGui::Button(u8"选择备份存放路径")) {
        wstring sel = SelectFolderDialog();
        if (!sel.empty()) {
            cfg.backupPath = sel;
        }
    }
    ImGui::SameLine();
    if (ImGui::InputText(u8"##备份路径值", buf, constant1)) { // 使用##隐藏标签，避免重复
        cfg.backupPath = utf8_to_wstring(buf);
    }

    char zipBuf[constant1];
    strncpy_s(zipBuf, wstring_to_utf8(cfg.zipPath).c_str(), sizeof(zipBuf));
    //先寻找电脑上是否存在7z
    if (filesystem::exists("7z.exe") && cfg.zipPath.empty())
    {
        cfg.zipPath = L"7z.exe";
        ImGui::Text(u8"已自动找到压缩程序");
    }
    else if(cfg.zipPath.empty())
    {
        string zipPath = GetRegistryValue("Software\\7-Zip", "Path") + "7z.exe";
        cfg.zipPath = utf8_to_wstring(zipPath);
        if(!zipPath.empty())
            ImGui::Text(u8"已自动找到压缩程序");
    }
    if (ImGui::Button(u8"选择压缩程序 7z.exe")) {
        wstring sel = SelectFileDialog();
        if (!sel.empty()) {
            cfg.zipPath = sel;
        }
    }
    ImGui::SameLine();
    if (ImGui::InputText(u8"##压缩程序路径", zipBuf, constant1)) {
        cfg.zipPath = utf8_to_wstring(zipBuf);
    }

    // 压缩格式单选按钮
    static int format_choice = 0;//默认7z
    format_choice = (cfg.zipFormat == L"zip") ? 1 : 0;
    ImGui::Text(u8"压缩格式:"); ImGui::SameLine();
    if (ImGui::RadioButton(u8"7z", &format_choice, 0)) { cfg.zipFormat = L"7z"; } ImGui::SameLine();
    if (ImGui::RadioButton(u8"zip", &format_choice, 1)) { cfg.zipFormat = L"zip"; }

    ImGui::SliderInt(u8"压缩等级", &cfg.zipLevel, 0, 9);
    ImGui::InputInt(u8"保留数量", &cfg.keepCount);
    ImGui::Checkbox(u8"智能备份", &cfg.smartBackup);
    ImGui::SameLine();
    ImGui::Checkbox(u8"备份前还原", &cfg.restoreBefore);
    ImGui::SameLine();
    ImGui::Checkbox(u8"置顶窗口", &cfg.topMost);
    ImGui::SameLine();
    ImGui::Checkbox(u8"手动选择还原", &cfg.manualRestore);
    ImGui::SameLine();
    ImGui::Checkbox(u8"显示过程", &cfg.showProgress);

    ImGui::Separator();
    ImGui::Text(u8"备份命名方式:");
    int folder_name_choice = (int)cfg.folderNameType;
    if (ImGui::RadioButton(u8"按世界名", &folder_name_choice, NAME_BY_WORLD)) { cfg.folderNameType = NAME_BY_WORLD; } ImGui::SameLine();
    if (ImGui::RadioButton(u8"按描述", &folder_name_choice, NAME_BY_DESC)) { cfg.folderNameType = NAME_BY_DESC; }

    ImGui::Separator();
    ImGui::Text(u8"主题设置:");
    int theme_choice = (int)cfg.theme;
    if (ImGui::RadioButton(u8"暗色", &theme_choice, THEME_DARK)) { cfg.theme = THEME_DARK; ApplyTheme(cfg.theme); } ImGui::SameLine();
    if (ImGui::RadioButton(u8"亮色", &theme_choice, THEME_LIGHT)) { cfg.theme = THEME_LIGHT; ApplyTheme(cfg.theme); } ImGui::SameLine();
    if (ImGui::RadioButton(u8"经典", &theme_choice, THEME_CLASSIC)) { cfg.theme = THEME_CLASSIC; ApplyTheme(cfg.theme); }
    
    // 透明度设置
    static float window_alpha = ImGui::GetStyle().Alpha;
    if (ImGui::SliderFloat(u8"窗口透明度", &window_alpha, 0.2f, 1.0f, "%.2f")) {
        ImGui::GetStyle().Alpha = window_alpha;
    }

    ImGui::Text(u8"字体设置(在 C:\\Windows\\Fonts 路径下，需手动填写):");
    char Fonts[constant1];
    strncpy_s(Fonts, wstring_to_utf8(cfg.zipFonts).c_str(), sizeof(Fonts));
    if (ImGui::Button(u8"选择字体存放路径")) {
        wstring sel = SelectFileDialog();
        if (!sel.empty()) {
            cfg.zipFonts = sel;
        }
    }
    ImGui::SameLine();
    if (ImGui::InputText(u8"##字体路径值", Fonts, constant1)) {
        cfg.zipFonts = utf8_to_wstring(Fonts);
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    if (ImGui::Button(u8"保存并关闭", ImVec2(120, 0))) {
        SaveConfigs(); // 保存所有配置
        showSettings = false;
    }
    ImGui::End();
}

//文件存在判断
static bool Exists(wstring& files) {
    return filesystem::exists(files);
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
                else if (strncmp(item, "# ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
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
        if (ImGui::InputText(u8"输入", InputBuf, IM_ARRAYSIZE(InputBuf), input_text_flags, &TextEditCallbackStub, (void*)this))
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
                AddLog("无法匹配 \"%.*s\"!\n", (int)(word_end - word_start), word_start);
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
                AddLog("可能的匹配:\n");
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

// 限制备份文件数量，超出则自动删除最旧的
void LimitBackupFiles(const std::wstring& folderPath, int limit, Console* console = nullptr)
{
    if (limit <= 0) return;
    namespace fs = std::filesystem;
    std::vector<fs::directory_entry> files;

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
        if (console) console->AddLog(u8"[error] 遍历备份目录失败: %s", e.what());
        return;
    }

    // 如果未超出限制，无需处理
    if ((int)files.size() <= limit) return;

    // 按最后写入时间升序排序（最旧的在前）
    std::sort(files.begin(), files.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return fs::last_write_time(a) < fs::last_write_time(b);
        });

    // 删除多余的最旧文件
    int to_delete = (int)files.size() - limit;
    for (int i = 0; i < to_delete; ++i) {
        try {
            fs::remove(files[i]);
            if (console) console->AddLog(u8"[提示] 已自动删除旧备份: %s", wstring_to_utf8(files[i].path().filename().wstring()).c_str());
        }
        catch (const fs::filesystem_error& e) {
            if (console) console->AddLog(u8"[error] 删除备份文件失败: %s", e.what());
        }
    }
}

//在后台静默执行一个命令行程序（如7z.exe），并等待其完成。
//这是实现备份和还原功能的核心，避免了GUI卡顿和黑窗口弹出。
// 参数:
//   - command: 要执行的完整命令行（宽字符）。
//   - console: 监控台对象的引用，用于输出日志信息。
void RunCommandInBackground(wstring command, Console& console) {
    // CreateProcessW需要一个可写的C-style字符串，所以我们将wstring复制到vector<wchar_t>
    vector<wchar_t> cmd_line(command.begin(), command.end());
    cmd_line.push_back(L'\0'); // 添加字符串结束符

    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // 隐藏子进程的窗口

    // 开始创建进程
    console.AddLog(u8"[提示] 正在执行命令: %s", wstring_to_utf8(command).c_str());

    if (!CreateProcessW(NULL, cmd_line.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        console.AddLog(u8"[error] 创建进程失败！错误码: %d", GetLastError());
        return;
    }

    // 等待子进程执行完毕
    WaitForSingleObject(pi.hProcess, INFINITE);

    // 检查子进程的退出代码
    DWORD exit_code;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        if (exit_code == 0) {
            console.AddLog(u8"[成功] 命令执行成功，操作完成。");
        }
        else {
            console.AddLog(u8"[error] 命令执行失败，退出代码: %d", exit_code);
        }
    }
    else {
        console.AddLog(u8"[error] 无法获取进程退出代码。");
    }

    // 清理句柄
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

// 执行单个世界的备份操作。
// 参数:
//   - config: 当前使用的配置。
//   - world:  要备份的世界（名称+描述）。
//   - console: 监控台对象的引用，用于输出日志信息。
void DoBackup(const Config config, const pair<wstring, wstring> world, Console& console) {
    console.AddLog(u8"---------- 开始备份 ----------");
    console.AddLog(u8"准备备份世界: %s", wstring_to_utf8(world.first).c_str());

    // 1. 检查7z.exe是否存在
    if (!filesystem::exists(config.zipPath)) {
        console.AddLog(u8"[error] 找不到压缩程序: %s", wstring_to_utf8(config.zipPath).c_str());
        console.AddLog(u8"请在“设置”中指定正确的 7z.exe 路径。");
        return;
    }

    // 2. 准备路径
    wstring sourcePath = config.saveRoot + L"\\" + world.first;
    wstring destinationFolder = config.backupPath + L"\\" + world.first;

    // 3. 创建备份目标文件夹（如果不存在）
    try {
        filesystem::create_directories(destinationFolder);
        console.AddLog(u8"备份目录: %s", wstring_to_utf8(destinationFolder).c_str());
    }
    catch (const filesystem::filesystem_error& e) {
        console.AddLog(u8"[error] 创建备份目录失败: %s", e.what());
        return;
    }

    // 4. 生成带时间戳的文件名
    time_t now = time(0);
    tm ltm;
    localtime_s(&ltm, &now);
    wchar_t timeBuf[80];
    wcsftime(timeBuf, sizeof(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);

    // 如果有描述，使用描述作为文件名；否则用世界名
    wstring archiveNameBase = world.second.empty() ? world.first : world.second;
    wstring archivePath = destinationFolder + L"\\" + L"[" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;

    // 5. 构建7-Zip命令行
    // 格式: "C:\7z.exe" a -t[格式] -mx=[等级] "目标压缩包路径" "源文件夹路径\*"
    wstring command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
        L" \"" + archivePath + L"\"" + L" \"" + sourcePath + L"\\*\"";

    // 6. 在后台线程中执行命令
    RunCommandInBackground(command, console);
    console.AddLog(u8"---------- 备份结束 ----------");
    LimitBackupFiles(destinationFolder, config.keepCount, &console);
}

// 执行单个世界的还原操作。
// 参数:
//   - config: 当前使用的配置。
//   - worldName: 要还原的世界名。
//   - backupFile: 要用于还原的备份文件名。
//   - console: 监控台对象的引用，用于输出日志信息。
void DoRestore(const Config config, const wstring& worldName, const wstring& backupFile, Console& console) {
    console.AddLog(u8"---------- 开始还原 ----------");
    console.AddLog(u8"准备还原世界: %s", wstring_to_utf8(worldName).c_str());
    console.AddLog(u8"使用备份文件: %s", wstring_to_utf8(backupFile).c_str());

    // 1. 检查7z.exe是否存在
    if (!filesystem::exists(config.zipPath)) {
        console.AddLog(u8"[error] 找不到压缩程序: %s", wstring_to_utf8(config.zipPath).c_str());
        console.AddLog(u8"请在“设置”中指定正确的 7z.exe 路径。");
        return;
    }

    // 2. 准备路径
    wstring sourceArchive = config.backupPath + L"\\" + worldName + L"\\" + backupFile;
    wstring destinationFolder = config.saveRoot + L"\\" + worldName;

    // 3. 构建7-Zip命令行
    // 格式: "C:\7z.exe" x "源压缩包路径" -o"目标文件夹路径" -y
    // 'x' 表示带路径解压, '-o' 指定输出目录, '-y' 表示对所有提示回答“是”（例如覆盖文件）
    wstring command = L"\"" + config.zipPath + L"\" x \"" + sourceArchive + L"\" -o\"" + destinationFolder + L"\" -y";

    // 4. 在后台线程中执行命令
    RunCommandInBackground(command, console);
    console.AddLog(u8"---------- 还原结束 ----------");
}

void AutoBackupThreadFunction(int worldIdx, int configIdx, int intervalMinutes, Console* console) {
    console->AddLog(u8"[自动备份] 已为世界 #%d 启动，间隔 %d 分钟。", worldIdx, intervalMinutes);

    while (true) {
        // 等待指定的时间，但每秒检查一次是否需要停止
        for (int i = 0; i < intervalMinutes * 60; ++i) {
            // 安全地检查停止标志
            if (g_active_auto_backups.at(worldIdx).stop_flag) {
                console->AddLog(u8"[自动备份] 已手动停止世界 #%d 的任务。", worldIdx);
                return; // 结束线程
            }
            this_thread::sleep_for(chrono::seconds(1));
        }

        // 时间到了，开始备份
        console->AddLog(u8"[自动备份] 正在为世界 #%d 执行例行备份...", worldIdx);
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
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"MineBackup - v1.5.0", WS_OVERLAPPEDWINDOW, 100, 100, 1000, 800, nullptr, nullptr, wc.hInstance, nullptr);

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
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    string fileName = "config.ini";
    bool isFirstRun = !filesystem::exists(fileName);
    static bool showConfigWizard = isFirstRun;
    static bool showMainApp = !isFirstRun;
    ImGui::StyleColorsLight();//默认亮色
    LoadConfigs(fileName);
    ImFont* font = io.Fonts->AddFontFromFileTTF(wstring_to_utf8(Fontss).c_str(), 20.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
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
            // 首次启动向导使用的静态变量
            static int page = 0;
            static char saveRootPath[constant1] = ""; // 修改为单个存档根路径
            static char backupPath[constant1] = "";
            static char zipPath[constant1] = "C:\\Program Files\\7-Zip\\7z.exe"; // 提供一个常用默认值

            ImGui::Begin(u8"首次启动设置向导", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

            if (page == 0) {
                ImGui::Text(u8"欢迎使用 Minecraft 存档备份工具！");
                ImGui::Separator();
                ImGui::TextWrapped(u8"本向导将帮助您设置您的第一套备份配置。");
                ImGui::TextWrapped(u8"如果您有多个游戏启动器或存档根目录，");
                ImGui::TextWrapped(u8"可以在完成向导后，进入主界面的“设置”中添加更多独立的配置方案。");
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                if (ImGui::Button(u8"开始配置", ImVec2(120, 0))) {
                    page++;
                }
            }
            else if (page == 1) {
                ImGui::Text(u8"第1步：选择 Minecraft 存档根目录");
                ImGui::TextWrapped(u8"请选择您的 Minecraft 游戏存档文件夹\"所在的\"文件夹。");
                ImGui::TextWrapped(u8"通常是游戏根目录下的 'saves' 文件夹。");
                ImGui::Dummy(ImVec2(0.0f, 10.0f));

                // 添加“选择文件夹”按钮
                if (ImGui::Button(u8"选择文件夹...")) {
                    wstring selected_folder = SelectFolderDialog();
                    if (!selected_folder.empty()) {
                        string sel_utf8 = wstring_to_utf8(selected_folder);
                        strncpy_s(saveRootPath, sel_utf8.c_str(), sizeof(saveRootPath));
                    }
                }
                ImGui::SameLine();
                ImGui::InputText(u8"存档根路径", saveRootPath, IM_ARRAYSIZE(saveRootPath));

                ImGui::Dummy(ImVec2(0.0f, 20.0f));
                if (ImGui::Button(u8"下一步", ImVec2(120, 0))) {
                    // 简单验证路径非空, 注意 exists 检查的路径有中文要是gbk的
                    if (strlen(saveRootPath) > 0 && filesystem::exists(utf8_to_wstring(saveRootPath))) {
                        page++;
                        errorShow = false;
                    }
                    else {
                        errorShow = true;
                    }
                }
                if (errorShow)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"路径为空或者文件夹不存在！");
            }
            else if (page == 2) {
                ImGui::Text(u8"第2步：选择备份文件存放路径");
                ImGui::TextWrapped(u8"请选择一个用于存放所有存档备份文件的文件夹。");
                ImGui::Dummy(ImVec2(0.0f, 10.0f));

                if (ImGui::Button(u8"选择文件夹...")) {
                    wstring selected_folder = SelectFolderDialog();
                    if (!selected_folder.empty()) {
                        string sel_utf8 = wstring_to_utf8(selected_folder);
                        strncpy_s(backupPath, sel_utf8.c_str(), sizeof(backupPath));
                    }
                }
                ImGui::SameLine();
                ImGui::InputText(u8"备份存放路径", backupPath, IM_ARRAYSIZE(backupPath));

                ImGui::Dummy(ImVec2(0.0f, 20.0f));
                if (ImGui::Button(u8"上一步", ImVec2(120, 0))) page--;
                ImGui::SameLine();
                if (ImGui::Button(u8"下一步", ImVec2(120, 0))) {
                    if (strlen(backupPath) > 0 && filesystem::exists(utf8_to_wstring(backupPath))) {
                        page++;
                        errorShow = false;
                    }
                    else {
                        errorShow = true;
                    }
                }
                if (errorShow)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"路径为空或者文件夹不存在！");
            }
            else if (page == 3) {
                ImGui::Text(u8"第3步：配置压缩程序 (7-Zip)");
                ImGui::TextWrapped(u8"本工具需要 7-Zip 来进行压缩。请指定其主程序 7z.exe 的位置。");
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                //先寻找电脑上是否存在7z
                if (filesystem::exists("7z.exe"))
                {
                    strncpy_s(zipPath, "7z.exe", sizeof(zipPath));
                    ImGui::Text(u8"已自动找到压缩程序");
                }
                else
                {
                    static string zipTemp = GetRegistryValue("Software\\7-Zip", "Path") + "7z.exe";
                    strncpy_s(zipPath, zipTemp.c_str(), sizeof(zipPath));
                    if (strlen(zipPath) != 0)
                        ImGui::Text(u8"已自动找到压缩程序");
                }
                if (ImGui::Button(u8"选择 7z.exe...")) {
                    wstring selected_file = SelectFileDialog();
                    if (!selected_file.empty()) {
                        string sel_utf8 = wstring_to_utf8(selected_file);
                        strncpy_s(zipPath, sel_utf8.c_str(), sizeof(zipPath));
                    }
                }
                ImGui::SameLine();
                ImGui::InputText(u8"7z.exe 路径", zipPath, IM_ARRAYSIZE(zipPath));

                ImGui::Dummy(ImVec2(0.0f, 20.0f));
                if (ImGui::Button(u8"上一步", ImVec2(120, 0))) page--;
                ImGui::SameLine();
                if (ImGui::Button(u8"完成配置", ImVec2(120, 0))) {
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
                        initialConfig.smartBackup = true;
                        initialConfig.restoreBefore = false;
                        initialConfig.topMost = false;
                        initialConfig.manualRestore = true;
                        initialConfig.showProgress = true;
                        initialConfig.zipFonts = L"c:\\Windows\\Fonts\\msyh.ttc";

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
            // --- 主窗口 ---
            ImGui::Begin(u8"我的世界存档备份器");

            // --- 左侧面板：世界列表和操作 ---
            ImGui::BeginChild(u8"左侧面板", ImVec2(ImGui::GetContentRegionAvail().x * 0.9f, 0), true);

            ImGui::SeparatorText(u8"存档列表");
            // 获取当前配置
            Config& cfg = configs[currentConfigIndex];

            if (ImGui::BeginTable(u8"世界列表", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn(u8"世界名称", ImGuiTableColumnFlags_WidthStretch, 0.3f);
                ImGui::TableSetupColumn(u8"描述/别名", ImGuiTableColumnFlags_WidthStretch, 0.4f);
                ImGui::TableSetupColumn(u8"最近打开时间|备份时间", ImGuiTableColumnFlags_WidthFixed, 160.0f);
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

            if (ImGui::Button(u8"备份选中存档", ImVec2(-1, 0))) {
                // 创建一个后台线程来执行备份，防止UI卡死
                thread backup_thread(DoBackup, cfg, cfg.worlds[selectedWorldIndex], ref(console));
                backup_thread.detach(); // 分离线程，让它在后台独立运行
            }

            if (ImGui::Button(u8"自动备份选中存档", ImVec2(-1, 0))) {
                // 只有选中了世界才能打开弹窗
                if (selectedWorldIndex != -1) {
                    ImGui::OpenPopup(u8"自动备份设置");
                }
            }

            if (ImGui::Button(u8"还原选中存档", ImVec2(-1, 0))) {
                openRestorePopup = true; // 打开还原选择弹窗
                ImGui::OpenPopup(u8"选择备份文件");
            }

            if (no_world_selected) {
                ImGui::EndDisabled();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"请先在上方列表中选择一个存档");
            }

            if (ImGui::BeginPopupModal(u8"自动备份设置", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                bool is_task_running = false;
                {
                    // 检查任务是否正在运行时，也需要加锁
                    lock_guard<mutex> lock(g_task_mutex);
                    is_task_running = g_active_auto_backups.count(selectedWorldIndex);
                }

                if (is_task_running) {
                    ImGui::Text(u8"世界 '%s' 的自动备份正在运行中。", wstring_to_utf8(cfg.worlds[selectedWorldIndex].first).c_str());
                    ImGui::Separator();
                    if (ImGui::Button(u8"停止自动备份", ImVec2(-1, 0))) {
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
                    ImGui::Text(u8"为世界 '%s' 设置自动备份:", wstring_to_utf8(cfg.worlds[selectedWorldIndex].first).c_str());
                    ImGui::Separator();
                    static int interval = 15; // 默认15分钟
                    ImGui::InputInt(u8"间隔时间 (分钟)", &interval);
                    if (interval < 1) interval = 1; // 最小间隔1分钟

                    if (ImGui::Button(u8"开始", ImVec2(120, 0))) {
                        lock_guard<mutex> lock(g_task_mutex);
                        auto& task = g_active_auto_backups[selectedWorldIndex];
                        task.stop_flag = false;
                        // 启动后台线程，注意 console 是通过指针传递的
                        task.worker = thread(AutoBackupThreadFunction, selectedWorldIndex, currentConfigIndex, interval, &console);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(u8"取消", ImVec2(120, 0))) {
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }

            // --- 还原文件选择弹窗 ---
            if (ImGui::BeginPopupModal(u8"选择备份文件", &openRestorePopup, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text(u8"请为世界“%s”选择一个备份文件进行还原:", wstring_to_utf8(cfg.worlds[selectedWorldIndex].first).c_str());

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

                if (ImGui::Button(u8"确认还原", ImVec2(120, 0))) {
                    // 创建后台线程执行还原
                    thread restore_thread(DoRestore, cfg, cfg.worlds[selectedWorldIndex].first, backupFiles[selectedBackupIndex], ref(console));
                    restore_thread.detach();
                    openRestorePopup = false; // 关闭弹窗
                    ImGui::CloseCurrentPopup();
                }
                if (no_backup_selected) ImGui::EndDisabled();

                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ImGui::Button(u8"取消", ImVec2(120, 0))) {
                    openRestorePopup = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            ImGui::EndChild();

            ImGui::SameLine();

            // --- 右侧面板：监控台和其他 ---
            ImGui::BeginChild(u8"右侧面板");

            // 显示主界面上的“设置”和“退出”按钮
            if (ImGui::Button(u8"设置")) showSettings = true;
            //ImGui::SameLine();
            if (ImGui::Button(u8"退出")) done = true;

            // 显示设置窗口（如果showSettings为true）
            ShowSettingsWindow();

            // 显示监控台
            // 注意：我们将静态的console实例传递给它
            console.Draw(u8"MineBackup 监控台", &showMainApp);

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
    wcin.get(); // Wait for user input before exiting
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
