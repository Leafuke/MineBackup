#include <string>
#include <filesystem>
#include <shobjidl.h>
#include <Windows.h>
#include "resource.h"

using namespace std;
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
wstring SelectFolderDialog(HWND hwndOwner = NULL) {
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

    wstring finalPath = tempPath;
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

    // ������ƣ���ʵû��Ҫ
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

    wchar_t tempPath[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempPath)) return false;

    wstring finalPath = tempPath;
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

bool IsFileLocked(const wstring& path) {
    if (!filesystem::exists(path)) {
        return false;
    }
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    //HANDLE hFile = CreateFile(filePath.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_SHARE_READ, NULL)
    if (hFile == INVALID_HANDLE_VALUE) {
        return true;
        if (GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_LOCK_VIOLATION) {
            return true;
        }
    }
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
    }
    return false;
}