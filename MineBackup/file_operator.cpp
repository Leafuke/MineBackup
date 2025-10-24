#include <string>
#include <filesystem>
#include <shobjidl.h>
#include <shlobj.h>
#include <Windows.h>
#include "resource.h"

using namespace std;
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
wstring SelectFolderDialog(HWND hwndOwner = NULL) {
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

    // 获取“文档”文件夹路径
    PWSTR documentsPath = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documentsPath);
    if (FAILED(hr)) {
        return false;
    }

    // 构造目标路径：文档\7z.exe
    wstring finalPath = documentsPath;
    CoTaskMemFree(documentsPath); // 释放 SHGetKnownFolderPath 分配的内存

    if (finalPath.back() != L'\\') finalPath += L'\\';
    finalPath += L"7z.exe";

    if (filesystem::exists(finalPath)) {
        extractedPath = finalPath;
        return true;
    }

    HANDLE hFile = CreateFileW(finalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
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

    /*wchar_t tempFile[MAX_PATH];
    if (!GetTempFileNameW(tempPath, L"7z", 0, tempFile)) return false;

    // 随机名称，其实没必要
    std::wstring finalPath = tempFile;
    finalPath += L".exe";
    MoveFileW(tempFile, finalPath.c_str());*/
}

bool ExtractFontToTempFile(wstring& extractedPath) {
    HRSRC hRes = FindResource(GetModuleHandle(NULL), MAKEINTRESOURCE(IDR_FONTS1), L"FONTS");
    if (!hRes) return false;

    HGLOBAL hData = LoadResource(GetModuleHandle(NULL), hRes);
    if (!hData) return false;

    DWORD dataSize = SizeofResource(GetModuleHandle(NULL), hRes);
    if (dataSize == 0) return false;

    LPVOID pData = LockResource(hData);
    if (!pData) return false;

    // 获取“文档”文件夹路径
    PWSTR documentsPath = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documentsPath);
    if (FAILED(hr)) {
        return false;
    }

    // 构造目标路径：文档\7z.exe
    wstring finalPath = documentsPath;
    CoTaskMemFree(documentsPath); // 释放 SHGetKnownFolderPath 分配的内存

    if (finalPath.back() != L'\\') finalPath += L'\\';
    finalPath += L"fontawesome-sp.otf";

    if (filesystem::exists(finalPath)) {
        extractedPath = finalPath;
        return true;
    }

    HANDLE hFile = CreateFileW(finalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
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

