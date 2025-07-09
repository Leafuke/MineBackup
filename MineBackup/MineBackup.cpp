#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"
#include "resource.h"
#include "resource2.h"
#include <d3d11.h>
#include <tchar.h>
#pragma comment (lib,"d3d11.lib") 
//����Ҫ�ֶ����d3d11.lib���ļ����������ᱨ��
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
#include <atomic> // �����̰߳�ȫ�ı�־
#include <mutex>  // ���ڻ�����
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

// ������������
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// �����������ȫ�֣�
vector<wstring> savePaths = { L"" };
wchar_t backupPath[constant1] = L"";
wchar_t zipPath[constant1] = L"";
int compressLevel = 5;

// ÿ�����ÿ�Ľṹ ö�� ������ôд��ʵû��Ҫ...
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

// ȫ������
wstring Fontss = L"c:\\Windows\\Fonts\\msyh.ttc";
int currentConfigIndex = 1;
map<int, Config> configs;

// ����ȫ�ֱ�������
struct AutoBackupTask {
    thread worker;
    atomic<bool> stop_flag{ false }; // ԭ�Ӳ���ֵ�����ڰ�ȫ��֪ͨ�߳�ֹͣ
};

static map<int, AutoBackupTask> g_active_auto_backups; // key: worldIndex, value: task
static mutex g_task_mutex; // ר�����ڱ������� g_active_auto_backups �Ļ�����


bool Extract7zToTempFile(wstring& extractedPath) {
    // ����ģ����
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

    // ����Ϊ7z.exe
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

//ѡ���ļ�
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

//ѡ���ļ���
static wstring SelectFolderDialog(HWND hwndOwner = NULL) {
    IFileDialog* pfd;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
        IID_IFileDialog, reinterpret_cast<void**>(&pfd));

    if (SUCCEEDED(hr)) {
        DWORD options;
        pfd->GetOptions(&options);
        pfd->SetOptions(options | FOS_PICKFOLDERS); // ����Ϊѡ���ļ���
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

// ע����ѯ
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
        // ת��Ϊ system_clock::time_point
        auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
            ftime - filesystem::file_time_type::clock::now()
            + chrono::system_clock::now()
        );
        time_t cftime = chrono::system_clock::to_time_t(sctp);
        wchar_t buf[64];
        struct tm timeinfo;
        //wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtimeҪ���ɸ���ȫ��localtime
        if (localtime_s(&timeinfo, &cftime) == 0) { 
            wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
            return buf;
        }
        else {
            return L"δ֪";
        }
    }
    catch (...) {
        return L"δ֪";
    }
}

static wstring GetLastBackupTime(const wstring& backupDir) {
    time_t latest = 0;
    try {
        if (filesystem::exists(backupDir)) {
            for (const auto& entry : filesystem::directory_iterator(backupDir)) {
                if (entry.is_regular_file()) {
                    auto ftime = entry.last_write_time();
                    // ת��Ϊ system_clock::time_point
                    auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
                        ftime - filesystem::file_time_type::clock::now()
                        + chrono::system_clock::now()
                    );
                    time_t cftime = chrono::system_clock::to_time_t(sctp);
                    if (cftime > latest) latest = cftime;
                }
            }
        }
        if (latest == 0) return L"�ޱ���";
        wchar_t buf[64];
        struct tm timeinfo;
        //wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", localtime(&cftime));//localtimeҪ���ɸ���ȫ��localtime
        if (localtime_s(&timeinfo, &latest) == 0) {
            wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &timeinfo);
            return buf;
        }
        else {
            return L"δ֪";
        }
    }
    catch (...) {
        return L"δ֪";
    }
}

// ����������wstring <-> utf8 string��ʹ��WinAPI������C++17+�����ض��ֽڱ��루GBK��תUTF-8
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

    // ȥ��ĩβ��\0
    if (!u8str.empty() && u8str.back() == '\0') u8str.pop_back();
    return u8str;
}

// ����Ӧ�ú���
void ApplyTheme(ThemeType theme) {
    switch (theme) {
    case THEME_DARK: ImGui::StyleColorsDark(); break;
    case THEME_LIGHT: ImGui::StyleColorsLight(); break;
    case THEME_CLASSIC: ImGui::StyleColorsClassic(); break;
    }
}

// ��ȡ�����ļ�
static void LoadConfigs(const string& filename = "config.ini") {
    configs.clear();
    ifstream in(filename, ios::binary);
    if (!in.is_open()) return;
    string line1;
    wstring line, section;
    // cur��Ϊһ��ָ�룬ָ�� configs ���ȫ�� map<int, Config> �е�Ԫ�� Config
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
                if (pos != wstring::npos && line.find(L"��ǰʹ�����ñ��=", 0) == 0) {
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
            // ��ֵ
            auto pos = line.find(L'=');
            if (pos == wstring::npos) continue;
            wstring key = line.substr(0, pos);
            wstring val = line.substr(pos + 1);

            if (key == L"�浵·��") {
                cur->saveRoot = val;
                // �Զ�ɨ����Ŀ¼Ϊ������
                if (filesystem::exists(val)) {
                    for (auto& entry : filesystem::directory_iterator(val)) {
                        if (entry.is_directory())
                            cur->worlds.push_back({ entry.path().filename().wstring(), L"" });
                    }
                }
            }
            else if (key.find(L"�浵����+�浵����") == 0) {
                // ����ֱ�� '*'
                while (getline(in, line1) && line1 != "*") {
                    line = utf8_to_wstring(line1);
                    wstring name = line;
                    if (!getline(in, line1) || line1 == "*") break;
                    line = utf8_to_wstring(line1);
                    wstring desc = line;
                    cur->worlds.push_back({ name, desc });
                }
            }
            else if (key == L"����·��") cur->backupPath = val;
            else if (key == L"ѹ������") cur->zipPath = val;
            else if (key == L"ѹ����ʽ") cur->zipFormat = val;
            else if (key == L"ѹ���ȼ�") cur->zipLevel = stoi(val);
            else if (key == L"��������") cur->keepCount = stoi(val);
            else if (key == L"���ܱ���") cur->smartBackup = (val != L"0");
            else if (key == L"����ǰ��ԭ") cur->restoreBefore = (val != L"0");
            else if (key == L"�ö�����") cur->topMost = (val != L"0");
            else if (key == L"�ֶ�ѡ��ԭ") cur->manualRestore = (val != L"0");
            else if (key == L"��ʾ����") cur->showProgress = (val != L"0");
            else if (key == L"����") cur->theme = (ThemeType)stoi(val);
            else if (key == L"����������ʽ") cur->folderNameType = (BackupFolderNameType)stoi(val);
            else if (key == L"����") {
                cur->zipFonts = val;
                Fontss = val;
            }
            ApplyTheme(cur->theme);
        }
    }
}

bool showSettings = false;

// ����
static void SaveConfigs(const wstring& filename = L"config.ini") {
    wofstream out(filename, ios::binary);
    if (!out.is_open()) {
        MessageBoxW(nullptr, L"�޷�д�� config.ini��", L"����", MB_OK | MB_ICONERROR);
        return;
    }
    //out.imbue(locale("chs"));//�������������ANSI��
    out.imbue(locale(out.getloc(), new codecvt_byname<wchar_t, char, mbstate_t>("en_US.UTF-8")));
    //out.imbue(locale(out.getloc(), new codecvt_utf8<wchar_t>));//��UTF8תΪUTF������C++17Ҳ�����ˡ�������������define��
    out << L"[General]\n";
    out << L"��ǰʹ�����ñ��=" << currentConfigIndex << L"\n\n";
    for (auto& kv : configs) {
        int idx = kv.first;
        Config& c = kv.second;
        out << L"[Config" << idx << L"]\n";
        out << L"�浵·��=" << c.saveRoot << L"\n";
        out << L"�浵����+�浵����(һ������һ������)=\n";
        for (auto& p : c.worlds)
            out << p.first << L"\n" << p.second << L"\n";
        out << L"*\n";
        out << L"����·��=" << c.backupPath << L"\n";
        out << L"ѹ������=" << c.zipPath << L"\n";
        out << L"ѹ����ʽ=" << c.zipFormat << L"\n";
        out << L"ѹ���ȼ�=" << c.zipLevel << L"\n";
        out << L"��������=" << c.keepCount << L"\n";
        out << L"���ܱ���=" << (c.smartBackup ? 1 : 0) << L"\n";
        out << L"����ǰ��ԭ=" << (c.restoreBefore ? 1 : 0) << L"\n";
        out << L"�ö�����=" << (c.topMost ? 1 : 0) << L"\n";
        out << L"�ֶ�ѡ��ԭ=" << (c.manualRestore ? 1 : 0) << L"\n";
        out << L"��ʾ����=" << (c.showProgress ? 1 : 0) << L"\n";
        out << L"����=" << c.theme << L"\n";
        out << L"����=" << c.zipFonts << L"\n";
        out << L"����������ʽ=" << c.folderNameType << L"\n";
    }
}

// �����水ť�������ô���
void ShowMainUI() {
    if (ImGui::Button(u8"����")) {
        showSettings = true;
    }
}

//���ô���
void ShowSettingsWindow() {
    if (!showSettings) return;
    ImGui::Begin(u8"����", &showSettings, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SeparatorText(u8"���ù���");

    // ��ȡ��ǰѡ�е����ã�ȷ������UI����������ȷ��������
    // ���configsΪ��(���磬�״����е���������)���򴴽�һ����ʱ��
    if (configs.empty()) {
        configs[1] = Config();
        currentConfigIndex = 1;
    }
    Config& cfg = configs[currentConfigIndex];

    // 1. ����ѡ��������
    string current_config_label = u8"���� " + to_string(currentConfigIndex);
    if (ImGui::BeginCombo(u8"��ǰ����", current_config_label.c_str())) {
        for (auto const& [idx, val] : configs) {
            const bool is_selected = (currentConfigIndex == idx);
            string label = u8"���� " + to_string(idx);
            if (ImGui::Selectable(label.c_str(), is_selected)) {
                currentConfigIndex = idx;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // 2. ��Ӻ�ɾ����ť
    if (ImGui::Button(u8"���������")) {
        // �ҵ���������ID��Ȼ��+1��Ϊ��ID
        int new_index = configs.empty() ? 1 : configs.rbegin()->first + 1;
        configs[new_index] = Config(); // ����һ��Ĭ������
        currentConfigIndex = new_index; // �Զ��л����´���������

        //���ó�ʼֵ
        Config& cfg = configs[currentConfigIndex]; //�ܹؼ���
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
    if (ImGui::Button(u8"ɾ����ǰ����")) {
        if (configs.size() > 1) { // ���ٱ���һ������
            ImGui::OpenPopup(u8"ȷ��ɾ��");
        }
    }

    // ɾ��ȷ�ϵ������߼�
    if (ImGui::BeginPopupModal(u8"ȷ��ɾ��", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(u8"��ȷ��Ҫɾ�������� %d����\n�˲����޷�������\n", currentConfigIndex);
        ImGui::Separator();
        if (ImGui::Button(u8"ȷ��", ImVec2(120, 0))) {
            int old_index = currentConfigIndex;
            configs.erase(old_index);
            // �л���ʣ�µĵ�һ����������
            /*currentConfigIndex = configs.begin()->first;*/
            currentConfigIndex = 1; //���������׳���
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(u8"ȡ��", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SeparatorText(u8"��ǰ��������");

    // �浵��·��
    char rootBufA[constant1];
    strncpy_s(rootBufA, wstring_to_utf8(cfg.saveRoot).c_str(), sizeof(rootBufA));
    if (ImGui::Button(u8"ѡ���Ŵ浵���ļ���")) {
        wstring sel = SelectFolderDialog();
        if (!sel.empty()) {
            cfg.saveRoot = sel; // ֱ�Ӹ�ֵ����ǰ����
        }
    }
    ImGui::SameLine();
    if (ImGui::InputText(u8"�浵��·��", rootBufA, constant1)) {
        cfg.saveRoot = utf8_to_wstring(rootBufA);
    }


    // �Զ�ɨ�� worlds����ѡˢ�°�ť��
    if (ImGui::Button(u8"ɨ��浵�������Ϸ���·����")) {
        cfg.worlds.clear();
        if (filesystem::exists(cfg.saveRoot))
            for (auto& e : filesystem::directory_iterator(cfg.saveRoot))
                if (e.is_directory())
                    cfg.worlds.push_back({ e.path().filename().wstring(), L"" });
    }

    // ÿ���浵������+�����༭
    ImGui::Separator();
    ImGui::Text(u8"�浵������������");
    for (size_t i = 0; i < cfg.worlds.size(); ++i) {
        ImGui::PushID(int(i));
        char name[constant1], desc[constant2];
        strncpy_s(name, wstring_to_utf8(cfg.worlds[i].first).c_str(), sizeof(name));
        strncpy_s(desc, wstring_to_utf8(cfg.worlds[i].second).c_str(), sizeof(desc));

        if (ImGui::InputText(u8"����", name, constant1))
            cfg.worlds[i].first = utf8_to_wstring(name);
        if (ImGui::InputText(u8"����", desc, constant2))
            cfg.worlds[i].second = utf8_to_wstring(desc);

        ImGui::PopID();
    }

    // ����������
    char buf[constant1];
    strncpy_s(buf, wstring_to_utf8(cfg.backupPath).c_str(), sizeof(buf));
    if (ImGui::Button(u8"ѡ�񱸷ݴ��·��")) {
        wstring sel = SelectFolderDialog();
        if (!sel.empty()) {
            cfg.backupPath = sel;
        }
    }
    ImGui::SameLine();
    if (ImGui::InputText(u8"##����·��ֵ", buf, constant1)) { // ʹ��##���ر�ǩ�������ظ�
        cfg.backupPath = utf8_to_wstring(buf);
    }

    char zipBuf[constant1];
    strncpy_s(zipBuf, wstring_to_utf8(cfg.zipPath).c_str(), sizeof(zipBuf));
    //��Ѱ�ҵ������Ƿ����7z
    if (filesystem::exists("7z.exe") && cfg.zipPath.empty())
    {
        cfg.zipPath = L"7z.exe";
        ImGui::Text(u8"���Զ��ҵ�ѹ������");
    }
    else if(cfg.zipPath.empty())
    {
        string zipPath = GetRegistryValue("Software\\7-Zip", "Path") + "7z.exe";
        cfg.zipPath = utf8_to_wstring(zipPath);
        if(!zipPath.empty())
            ImGui::Text(u8"���Զ��ҵ�ѹ������");
    }
    if (ImGui::Button(u8"ѡ��ѹ������ 7z.exe")) {
        wstring sel = SelectFileDialog();
        if (!sel.empty()) {
            cfg.zipPath = sel;
        }
    }
    ImGui::SameLine();
    if (ImGui::InputText(u8"##ѹ������·��", zipBuf, constant1)) {
        cfg.zipPath = utf8_to_wstring(zipBuf);
    }

    // ѹ����ʽ��ѡ��ť
    static int format_choice = 0;//Ĭ��7z
    format_choice = (cfg.zipFormat == L"zip") ? 1 : 0;
    ImGui::Text(u8"ѹ����ʽ:"); ImGui::SameLine();
    if (ImGui::RadioButton(u8"7z", &format_choice, 0)) { cfg.zipFormat = L"7z"; } ImGui::SameLine();
    if (ImGui::RadioButton(u8"zip", &format_choice, 1)) { cfg.zipFormat = L"zip"; }

    ImGui::SliderInt(u8"ѹ���ȼ�", &cfg.zipLevel, 0, 9);
    ImGui::InputInt(u8"��������", &cfg.keepCount);
    ImGui::Checkbox(u8"���ܱ���", &cfg.smartBackup);
    ImGui::SameLine();
    ImGui::Checkbox(u8"����ǰ��ԭ", &cfg.restoreBefore);
    ImGui::SameLine();
    ImGui::Checkbox(u8"�ö�����", &cfg.topMost);
    ImGui::SameLine();
    ImGui::Checkbox(u8"�ֶ�ѡ��ԭ", &cfg.manualRestore);
    ImGui::SameLine();
    ImGui::Checkbox(u8"��ʾ����", &cfg.showProgress);

    ImGui::Separator();
    ImGui::Text(u8"����������ʽ:");
    int folder_name_choice = (int)cfg.folderNameType;
    if (ImGui::RadioButton(u8"��������", &folder_name_choice, NAME_BY_WORLD)) { cfg.folderNameType = NAME_BY_WORLD; } ImGui::SameLine();
    if (ImGui::RadioButton(u8"������", &folder_name_choice, NAME_BY_DESC)) { cfg.folderNameType = NAME_BY_DESC; }

    ImGui::Separator();
    ImGui::Text(u8"��������:");
    int theme_choice = (int)cfg.theme;
    if (ImGui::RadioButton(u8"��ɫ", &theme_choice, THEME_DARK)) { cfg.theme = THEME_DARK; ApplyTheme(cfg.theme); } ImGui::SameLine();
    if (ImGui::RadioButton(u8"��ɫ", &theme_choice, THEME_LIGHT)) { cfg.theme = THEME_LIGHT; ApplyTheme(cfg.theme); } ImGui::SameLine();
    if (ImGui::RadioButton(u8"����", &theme_choice, THEME_CLASSIC)) { cfg.theme = THEME_CLASSIC; ApplyTheme(cfg.theme); }
    
    // ͸��������
    static float window_alpha = ImGui::GetStyle().Alpha;
    if (ImGui::SliderFloat(u8"����͸����", &window_alpha, 0.2f, 1.0f, "%.2f")) {
        ImGui::GetStyle().Alpha = window_alpha;
    }

    ImGui::Text(u8"��������(�� C:\\Windows\\Fonts ·���£����ֶ���д):");
    char Fonts[constant1];
    strncpy_s(Fonts, wstring_to_utf8(cfg.zipFonts).c_str(), sizeof(Fonts));
    if (ImGui::Button(u8"ѡ��������·��")) {
        wstring sel = SelectFileDialog();
        if (!sel.empty()) {
            cfg.zipFonts = sel;
        }
    }
    ImGui::SameLine();
    if (ImGui::InputText(u8"##����·��ֵ", Fonts, constant1)) {
        cfg.zipFonts = utf8_to_wstring(Fonts);
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    if (ImGui::Button(u8"���沢�ر�", ImVec2(120, 0))) {
        SaveConfigs(); // ������������
        showSettings = false;
    }
    ImGui::End();
}

//�ļ������ж�
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
        Commands.push_back("CLASSIFY");
        AutoScroll = true;                  //�Զ�������ѽ
        ScrollToBottom = false;             //���ù�����������������
        AddLog(u8"��ӭʹ�� MineBackup ״̬���̨");
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

        ImGui::TextWrapped(
            u8"��������̨�У�����Կ���"
            u8"���д�����ʾ���浵����״̬��");
        ImGui::TextWrapped(u8"����'HELP'���鿴����");

        // TODO: display items starting from the bottom

        //if (ImGui::SmallButton("Add Debug Text")) { AddLog("%d some text", Items.Size); AddLog("some more text"); AddLog("display very important message here!"); }
        //ImGui::SameLine();
        /*if (ImGui::SmallButton("Add Debug Error")) { AddLog("[error] wrong"); }
        ImGui::SameLine();*/
        if (ImGui::SmallButton(u8"���")) { ClearLog(); }
        ImGui::SameLine();
        bool copy_to_clipboard = ImGui::SmallButton(u8"����");
        //static float t = 0.0f; if (ImGui::GetTime() - t > 0.02f) { t = ImGui::GetTime(); AddLog("Spam %f", t); }

        ImGui::Separator();

        // Options menu
        if (ImGui::BeginPopup(u8"ѡ��"))
        {
            ImGui::Checkbox(u8"�Զ�����", &AutoScroll);
            ImGui::EndPopup();
        }

        // Options, Filter
        ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_Tooltip);
        if (ImGui::Button(u8"ѡ��"))
            ImGui::OpenPopup(u8"ѡ��");
        ImGui::SameLine();
        Filter.Draw(u8"ɸѡ�� (\"���,��ʾ\") (\"error\")", 180);
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
                if (strstr(item, "[error]")) { color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); has_color = true; }
                else if (strncmp(item, "# ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f); has_color = true; }
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
        if (ImGui::InputText(u8"����", InputBuf, IM_ARRAYSIZE(InputBuf), input_text_flags, &TextEditCallbackStub, (void*)this))
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
                AddLog("�޷�ƥ�� \"%.*s\"!\n", (int)(word_end - word_start), word_start);
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
                AddLog("���ܵ�ƥ��:\n");
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

//չʾ����̨���ڡ�����Ҫ���������е���
static void ShowExampleAppConsole(bool* p_open)
{
    static Console console;
    console.Draw(u8"MineBackup ���̨", p_open);
}

// ���Ʊ����ļ��������������Զ�ɾ����ɵ�
void LimitBackupFiles(const std::wstring& folderPath, int limit, Console* console = nullptr)
{
    if (limit <= 0) return;
    namespace fs = std::filesystem;
    std::vector<fs::directory_entry> files;

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
        if (console) console->AddLog(u8"[error] ��������Ŀ¼ʧ��: %s", e.what());
        return;
    }

    // ���δ�������ƣ����账��
    if ((int)files.size() <= limit) return;

    // �����д��ʱ������������ɵ���ǰ��
    std::sort(files.begin(), files.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return fs::last_write_time(a) < fs::last_write_time(b);
        });

    // ɾ�����������ļ�
    int to_delete = (int)files.size() - limit;
    for (int i = 0; i < to_delete; ++i) {
        try {
            fs::remove(files[i]);
            if (console) console->AddLog(u8"[��ʾ] ���Զ�ɾ���ɱ���: %s", wstring_to_utf8(files[i].path().filename().wstring()).c_str());
        }
        catch (const fs::filesystem_error& e) {
            if (console) console->AddLog(u8"[error] ɾ�������ļ�ʧ��: %s", e.what());
        }
    }
}

//�ں�̨��Ĭִ��һ�������г�����7z.exe�������ȴ�����ɡ�
//����ʵ�ֱ��ݺͻ�ԭ���ܵĺ��ģ�������GUI���ٺͺڴ��ڵ�����
// ����:
//   - command: Ҫִ�е����������У����ַ�����
//   - console: ���̨��������ã����������־��Ϣ��
void RunCommandInBackground(wstring command, Console& console) {
    // CreateProcessW��Ҫһ����д��C-style�ַ������������ǽ�wstring���Ƶ�vector<wchar_t>
    vector<wchar_t> cmd_line(command.begin(), command.end());
    cmd_line.push_back(L'\0'); // ����ַ���������

    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // �����ӽ��̵Ĵ���

    // ��ʼ��������
    console.AddLog(u8"[��ʾ] ����ִ������: %s", wstring_to_utf8(command).c_str());

    if (!CreateProcessW(NULL, cmd_line.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        console.AddLog(u8"[error] ��������ʧ�ܣ�������: %d", GetLastError());
        return;
    }

    // �ȴ��ӽ���ִ�����
    WaitForSingleObject(pi.hProcess, INFINITE);

    // ����ӽ��̵��˳�����
    DWORD exit_code;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        if (exit_code == 0) {
            console.AddLog(u8"[�ɹ�] ����ִ�гɹ���������ɡ�");
        }
        else {
            console.AddLog(u8"[error] ����ִ��ʧ�ܣ��˳�����: %d", exit_code);
        }
    }
    else {
        console.AddLog(u8"[error] �޷���ȡ�����˳����롣");
    }

    // ������
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

// ִ�е�������ı��ݲ�����
// ����:
//   - config: ��ǰʹ�õ����á�
//   - world:  Ҫ���ݵ����磨����+��������
//   - console: ���̨��������ã����������־��Ϣ��
void DoBackup(const Config config, const pair<wstring, wstring> world, Console& console) {
    console.AddLog(u8"---------- ��ʼ���� ----------");
    console.AddLog(u8"׼����������: %s", wstring_to_utf8(world.first).c_str());

    // 1. ���7z.exe�Ƿ����
    if (!filesystem::exists(config.zipPath)) {
        console.AddLog(u8"[error] �Ҳ���ѹ������: %s", wstring_to_utf8(config.zipPath).c_str());
        console.AddLog(u8"���ڡ����á���ָ����ȷ�� 7z.exe ·����");
        return;
    }

    // 2. ׼��·��
    wstring sourcePath = config.saveRoot + L"\\" + world.first;
    wstring destinationFolder = config.backupPath + L"\\" + world.first;

    // 3. ��������Ŀ���ļ��У���������ڣ�
    try {
        filesystem::create_directories(destinationFolder);
        console.AddLog(u8"����Ŀ¼: %s", wstring_to_utf8(destinationFolder).c_str());
    }
    catch (const filesystem::filesystem_error& e) {
        console.AddLog(u8"[error] ��������Ŀ¼ʧ��: %s", e.what());
        return;
    }

    // 4. ���ɴ�ʱ������ļ���
    time_t now = time(0);
    tm ltm;
    localtime_s(&ltm, &now);
    wchar_t timeBuf[80];
    wcsftime(timeBuf, sizeof(timeBuf), L"%Y-%m-%d_%H-%M-%S", &ltm);

    // �����������ʹ��������Ϊ�ļ�����������������
    wstring archiveNameBase = world.second.empty() ? world.first : world.second;
    wstring archivePath = destinationFolder + L"\\" + L"[" + timeBuf + L"]" + archiveNameBase + L"." + config.zipFormat;

    // 5. ����7-Zip������
    // ��ʽ: "C:\7z.exe" a -t[��ʽ] -mx=[�ȼ�] "Ŀ��ѹ����·��" "Դ�ļ���·��\*"
    wstring command = L"\"" + config.zipPath + L"\" a -t" + config.zipFormat + L" -mx=" + to_wstring(config.zipLevel) +
        L" \"" + archivePath + L"\"" + L" \"" + sourcePath + L"\\*\"";

    // 6. �ں�̨�߳���ִ������
    RunCommandInBackground(command, console);
    console.AddLog(u8"---------- ���ݽ��� ----------");
    LimitBackupFiles(destinationFolder, config.keepCount, &console);
}

// ִ�е�������Ļ�ԭ������
// ����:
//   - config: ��ǰʹ�õ����á�
//   - worldName: Ҫ��ԭ����������
//   - backupFile: Ҫ���ڻ�ԭ�ı����ļ�����
//   - console: ���̨��������ã����������־��Ϣ��
void DoRestore(const Config config, const wstring& worldName, const wstring& backupFile, Console& console) {
    console.AddLog(u8"---------- ��ʼ��ԭ ----------");
    console.AddLog(u8"׼����ԭ����: %s", wstring_to_utf8(worldName).c_str());
    console.AddLog(u8"ʹ�ñ����ļ�: %s", wstring_to_utf8(backupFile).c_str());

    // 1. ���7z.exe�Ƿ����
    if (!filesystem::exists(config.zipPath)) {
        console.AddLog(u8"[error] �Ҳ���ѹ������: %s", wstring_to_utf8(config.zipPath).c_str());
        console.AddLog(u8"���ڡ����á���ָ����ȷ�� 7z.exe ·����");
        return;
    }

    // 2. ׼��·��
    wstring sourceArchive = config.backupPath + L"\\" + worldName + L"\\" + backupFile;
    wstring destinationFolder = config.saveRoot + L"\\" + worldName;

    // 3. ����7-Zip������
    // ��ʽ: "C:\7z.exe" x "Դѹ����·��" -o"Ŀ���ļ���·��" -y
    // 'x' ��ʾ��·����ѹ, '-o' ָ�����Ŀ¼, '-y' ��ʾ��������ʾ�ش��ǡ������縲���ļ���
    wstring command = L"\"" + config.zipPath + L"\" x \"" + sourceArchive + L"\" -o\"" + destinationFolder + L"\" -y";

    // 4. �ں�̨�߳���ִ������
    RunCommandInBackground(command, console);
    console.AddLog(u8"---------- ��ԭ���� ----------");
}

void AutoBackupThreadFunction(int worldIdx, int configIdx, int intervalMinutes, Console* console) {
    console->AddLog(u8"[�Զ�����] ��Ϊ���� #%d ��������� %d ���ӡ�", worldIdx, intervalMinutes);

    while (true) {
        // �ȴ�ָ����ʱ�䣬��ÿ����һ���Ƿ���Ҫֹͣ
        for (int i = 0; i < intervalMinutes * 60; ++i) {
            // ��ȫ�ؼ��ֹͣ��־
            if (g_active_auto_backups.at(worldIdx).stop_flag) {
                console->AddLog(u8"[�Զ�����] ���ֶ�ֹͣ���� #%d ������", worldIdx);
                return; // �����߳�
            }
            this_thread::sleep_for(chrono::seconds(1));
        }

        // ʱ�䵽�ˣ���ʼ����
        console->AddLog(u8"[�Զ�����] ����Ϊ���� #%d ִ�����б���...", worldIdx);
        // ֱ�ӵ����Ѿ����ڵ� DoBackup ����
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
    //����������ͼ��
    HICON hIcon = (HICON)LoadImage(
        GetModuleHandle(NULL),
        MAKEINTRESOURCE(IDI_ICON5),
        IMAGE_ICON,
        0, 0,
        LR_DEFAULTSIZE
    );

    if (hIcon) {
        // ���ô�ͼ�꣨������/Alt+Tab��
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        // ����Сͼ�꣨���ڱ�������
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
    ImGui::StyleColorsLight();//Ĭ����ɫ
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
            // �״�������ʹ�õľ�̬����
            static int page = 0;
            static char saveRootPath[constant1] = ""; // �޸�Ϊ�����浵��·��
            static char backupPath[constant1] = "";
            static char zipPath[constant1] = "C:\\Program Files\\7-Zip\\7z.exe"; // �ṩһ������Ĭ��ֵ

            ImGui::Begin(u8"�״�����������", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

            if (page == 0) {
                ImGui::Text(u8"��ӭʹ�� Minecraft �浵���ݹ��ߣ�");
                ImGui::Separator();
                ImGui::TextWrapped(u8"���򵼽��������������ĵ�һ�ױ������á�");
                ImGui::TextWrapped(u8"������ж����Ϸ��������浵��Ŀ¼��");
                ImGui::TextWrapped(u8"����������򵼺󣬽���������ġ����á�����Ӹ�����������÷�����");
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                if (ImGui::Button(u8"��ʼ����", ImVec2(120, 0))) {
                    page++;
                }
            }
            else if (page == 1) {
                ImGui::Text(u8"��1����ѡ�� Minecraft �浵��Ŀ¼");
                ImGui::TextWrapped(u8"��ѡ������ Minecraft ��Ϸ�浵�ļ���\"���ڵ�\"�ļ��С�");
                ImGui::TextWrapped(u8"ͨ������Ϸ��Ŀ¼�µ� 'saves' �ļ��С�");
                ImGui::Dummy(ImVec2(0.0f, 10.0f));

                // ��ӡ�ѡ���ļ��С���ť
                if (ImGui::Button(u8"ѡ���ļ���...")) {
                    wstring selected_folder = SelectFolderDialog();
                    if (!selected_folder.empty()) {
                        string sel_utf8 = wstring_to_utf8(selected_folder);
                        strncpy_s(saveRootPath, sel_utf8.c_str(), sizeof(saveRootPath));
                    }
                }
                ImGui::SameLine();
                ImGui::InputText(u8"�浵��·��", saveRootPath, IM_ARRAYSIZE(saveRootPath));

                ImGui::Dummy(ImVec2(0.0f, 20.0f));
                if (ImGui::Button(u8"��һ��", ImVec2(120, 0))) {
                    // ����֤·���ǿ�, ע�� exists ����·��������Ҫ��gbk��
                    if (strlen(saveRootPath) > 0 && filesystem::exists(utf8_to_wstring(saveRootPath))) {
                        page++;
                        errorShow = false;
                    }
                    else {
                        errorShow = true;
                    }
                }
                if (errorShow)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"·��Ϊ�ջ����ļ��в����ڣ�");
            }
            else if (page == 2) {
                ImGui::Text(u8"��2����ѡ�񱸷��ļ����·��");
                ImGui::TextWrapped(u8"��ѡ��һ�����ڴ�����д浵�����ļ����ļ��С�");
                ImGui::Dummy(ImVec2(0.0f, 10.0f));

                if (ImGui::Button(u8"ѡ���ļ���...")) {
                    wstring selected_folder = SelectFolderDialog();
                    if (!selected_folder.empty()) {
                        string sel_utf8 = wstring_to_utf8(selected_folder);
                        strncpy_s(backupPath, sel_utf8.c_str(), sizeof(backupPath));
                    }
                }
                ImGui::SameLine();
                ImGui::InputText(u8"���ݴ��·��", backupPath, IM_ARRAYSIZE(backupPath));

                ImGui::Dummy(ImVec2(0.0f, 20.0f));
                if (ImGui::Button(u8"��һ��", ImVec2(120, 0))) page--;
                ImGui::SameLine();
                if (ImGui::Button(u8"��һ��", ImVec2(120, 0))) {
                    if (strlen(backupPath) > 0 && filesystem::exists(utf8_to_wstring(backupPath))) {
                        page++;
                        errorShow = false;
                    }
                    else {
                        errorShow = true;
                    }
                }
                if (errorShow)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"·��Ϊ�ջ����ļ��в����ڣ�");
            }
            else if (page == 3) {
                ImGui::Text(u8"��3��������ѹ������ (7-Zip)");
                ImGui::TextWrapped(u8"��������Ҫ 7-Zip ������ѹ������ָ���������� 7z.exe ��λ�á�");
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                //��Ѱ�ҵ������Ƿ����7z
                if (filesystem::exists("7z.exe"))
                {
                    strncpy_s(zipPath, "7z.exe", sizeof(zipPath));
                    ImGui::Text(u8"���Զ��ҵ�ѹ������");
                }
                else
                {
                    static string zipTemp = GetRegistryValue("Software\\7-Zip", "Path") + "7z.exe";
                    strncpy_s(zipPath, zipTemp.c_str(), sizeof(zipPath));
                    if (strlen(zipPath) != 0)
                        ImGui::Text(u8"���Զ��ҵ�ѹ������");
                }
                if (ImGui::Button(u8"ѡ�� 7z.exe...")) {
                    wstring selected_file = SelectFileDialog();
                    if (!selected_file.empty()) {
                        string sel_utf8 = wstring_to_utf8(selected_file);
                        strncpy_s(zipPath, sel_utf8.c_str(), sizeof(zipPath));
                    }
                }
                ImGui::SameLine();
                ImGui::InputText(u8"7z.exe ·��", zipPath, IM_ARRAYSIZE(zipPath));

                ImGui::Dummy(ImVec2(0.0f, 20.0f));
                if (ImGui::Button(u8"��һ��", ImVec2(120, 0))) page--;
                ImGui::SameLine();
                if (ImGui::Button(u8"�������", ImVec2(120, 0))) {
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
                                    initialConfig.worlds.push_back({ entry.path().filename().wstring(), L"" }); // ����Ϊ�ļ�����������Ϊ��
                                }
                            }
                        }

                        // 3. ���ú����Ĭ��ֵ
                        initialConfig.zipFormat = L"7z";
                        initialConfig.zipLevel = 5;
                        initialConfig.keepCount = 10;
                        initialConfig.smartBackup = true;
                        initialConfig.restoreBefore = false;
                        initialConfig.topMost = false;
                        initialConfig.manualRestore = true;
                        initialConfig.showProgress = true;
                        initialConfig.zipFonts = L"c:\\Windows\\Fonts\\msyh.ttc";

                        // 4. ���浽�ļ����л�����Ӧ�ý���
                        SaveConfigs();
                        showConfigWizard = false;
                        showMainApp = true;
                    }
                }
            }

            ImGui::End();
        }
        if (showMainApp) {
            // ���ڸ����û����б���ѡ�������
            static int selectedWorldIndex = -1;
            // ���ڵ�����ԭ����
            static bool openRestorePopup = false;
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
            // --- ������ ---
            ImGui::Begin(u8"�ҵ�����浵������");

            // --- �����壺�����б�Ͳ��� ---
            ImGui::BeginChild(u8"������", ImVec2(ImGui::GetContentRegionAvail().x * 0.9f, 0), true);

            ImGui::SeparatorText(u8"�浵�б�");
            // ��ȡ��ǰ����
            Config& cfg = configs[currentConfigIndex];

            if (ImGui::BeginTable(u8"�����б�", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn(u8"��������", ImGuiTableColumnFlags_WidthStretch, 0.3f);
                ImGui::TableSetupColumn(u8"����/����", ImGuiTableColumnFlags_WidthStretch, 0.4f);
                ImGui::TableSetupColumn(u8"�����ʱ��|����ʱ��", ImGuiTableColumnFlags_WidthFixed, 160.0f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < cfg.worlds.size(); ++i) {
                    ImGui::PushID(i); // Ϊ��ǰѭ����������һ��Ψһ��ID������

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();

                    bool is_selected = (selectedWorldIndex == i);
                    // Ϊ�˱���Selectable�ı�ǩ��ͻ������ʹ�����ر�ǩ"##label"�ļ���
                    // ������ʹ��������Ϊ�գ�Ҳ����������
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

                    ImGui::PopID(); // ����ID������Ϊ��һ��ѭ����׼��
                }
                ImGui::EndTable();
            }

            ImGui::Separator();

            // --- ���Ĳ�����ť ---
            // ���û��ѡ���κ����磬����ð�ť
            bool no_world_selected = (selectedWorldIndex == -1);
            if (no_world_selected) {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button(u8"����ѡ�д浵", ImVec2(-1, 0))) {
                // ����һ����̨�߳���ִ�б��ݣ���ֹUI����
                thread backup_thread(DoBackup, cfg, cfg.worlds[selectedWorldIndex], ref(console));
                backup_thread.detach(); // �����̣߳������ں�̨��������
            }

            if (ImGui::Button(u8"�Զ�����ѡ�д浵", ImVec2(-1, 0))) {
                // ֻ��ѡ����������ܴ򿪵���
                if (selectedWorldIndex != -1) {
                    ImGui::OpenPopup(u8"�Զ���������");
                }
            }

            if (ImGui::Button(u8"��ԭѡ�д浵", ImVec2(-1, 0))) {
                openRestorePopup = true; // �򿪻�ԭѡ�񵯴�
                ImGui::OpenPopup(u8"ѡ�񱸷��ļ�");
            }

            if (no_world_selected) {
                ImGui::EndDisabled();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"�������Ϸ��б���ѡ��һ���浵");
            }

            if (ImGui::BeginPopupModal(u8"�Զ���������", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                bool is_task_running = false;
                {
                    // ��������Ƿ���������ʱ��Ҳ��Ҫ����
                    lock_guard<mutex> lock(g_task_mutex);
                    is_task_running = g_active_auto_backups.count(selectedWorldIndex);
                }

                if (is_task_running) {
                    ImGui::Text(u8"���� '%s' ���Զ��������������С�", wstring_to_utf8(cfg.worlds[selectedWorldIndex].first).c_str());
                    ImGui::Separator();
                    if (ImGui::Button(u8"ֹͣ�Զ�����", ImVec2(-1, 0))) {
                        lock_guard<mutex> lock(g_task_mutex);
                        // 1. ����ֹͣ��־
                        g_active_auto_backups.at(selectedWorldIndex).stop_flag = true;
                        // 2. �ȴ��߳̽���
                        if (g_active_auto_backups.at(selectedWorldIndex).worker.joinable()) {
                            g_active_auto_backups.at(selectedWorldIndex).worker.join();
                        }
                        // 3. �ӹ��������Ƴ�
                        g_active_auto_backups.erase(selectedWorldIndex);
                        ImGui::CloseCurrentPopup();
                    }
                }
                else {
                    ImGui::Text(u8"Ϊ���� '%s' �����Զ�����:", wstring_to_utf8(cfg.worlds[selectedWorldIndex].first).c_str());
                    ImGui::Separator();
                    static int interval = 15; // Ĭ��15����
                    ImGui::InputInt(u8"���ʱ�� (����)", &interval);
                    if (interval < 1) interval = 1; // ��С���1����

                    if (ImGui::Button(u8"��ʼ", ImVec2(120, 0))) {
                        lock_guard<mutex> lock(g_task_mutex);
                        auto& task = g_active_auto_backups[selectedWorldIndex];
                        task.stop_flag = false;
                        // ������̨�̣߳�ע�� console ��ͨ��ָ�봫�ݵ�
                        task.worker = thread(AutoBackupThreadFunction, selectedWorldIndex, currentConfigIndex, interval, &console);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(u8"ȡ��", ImVec2(120, 0))) {
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }

            // --- ��ԭ�ļ�ѡ�񵯴� ---
            if (ImGui::BeginPopupModal(u8"ѡ�񱸷��ļ�", &openRestorePopup, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text(u8"��Ϊ���硰%s��ѡ��һ�������ļ����л�ԭ:", wstring_to_utf8(cfg.worlds[selectedWorldIndex].first).c_str());

                wstring backupDir = cfg.backupPath + L"\\" + cfg.worlds[selectedWorldIndex].first;
                static int selectedBackupIndex = -1;
                vector<wstring> backupFiles;

                // ��������Ŀ¼���ҵ������ļ�
                if (filesystem::exists(backupDir)) {
                    for (const auto& entry : filesystem::directory_iterator(backupDir)) {
                        if (entry.is_regular_file()) {
                            backupFiles.push_back(entry.path().filename().wstring());
                        }
                    }
                }

                // ��ʾ�����ļ��б�
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

                // ȷ�ϻ�ԭ��ť
                bool no_backup_selected = (selectedBackupIndex == -1);
                if (no_backup_selected) ImGui::BeginDisabled();

                if (ImGui::Button(u8"ȷ�ϻ�ԭ", ImVec2(120, 0))) {
                    // ������̨�߳�ִ�л�ԭ
                    thread restore_thread(DoRestore, cfg, cfg.worlds[selectedWorldIndex].first, backupFiles[selectedBackupIndex], ref(console));
                    restore_thread.detach();
                    openRestorePopup = false; // �رյ���
                    ImGui::CloseCurrentPopup();
                }
                if (no_backup_selected) ImGui::EndDisabled();

                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ImGui::Button(u8"ȡ��", ImVec2(120, 0))) {
                    openRestorePopup = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            ImGui::EndChild();

            ImGui::SameLine();

            // --- �Ҳ���壺���̨������ ---
            ImGui::BeginChild(u8"�Ҳ����");

            // ��ʾ�������ϵġ����á��͡��˳�����ť
            if (ImGui::Button(u8"����")) showSettings = true;
            //ImGui::SameLine();
            if (ImGui::Button(u8"�˳�")) done = true;

            // ��ʾ���ô��ڣ����showSettingsΪtrue��
            ShowSettingsWindow();

            // ��ʾ���̨
            // ע�⣺���ǽ���̬��consoleʵ�����ݸ���
            console.Draw(u8"MineBackup ���̨", &showMainApp);

            ImGui::EndChild();


            ImGui::End();
        }

        // Rendering��Ⱦ����
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

    // ����
    lock_guard<mutex> lock(g_task_mutex);
    for (auto& pair : g_active_auto_backups) {
        pair.second.stop_flag = true; // ֪ͨ�߳�ֹͣ
        if (pair.second.worker.joinable()) {
            pair.second.worker.join(); // �ȴ��߳�ִ�����
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
