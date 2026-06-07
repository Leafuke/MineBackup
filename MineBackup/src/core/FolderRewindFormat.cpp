#include "FolderRewindFormat.h"

#include "text_to_text.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <initializer_list>
#include <iomanip>
#include <iterator>
#include <sstream>

#ifdef _WIN32
#include <objbase.h>
#pragma comment(lib, "Ole32.lib")
#endif

using namespace std;

namespace FolderRewindFormat {
namespace {

wstring Trim(wstring value) {
    auto notSpace = [](wchar_t ch) { return !iswspace(ch); };
    value.erase(value.begin(), find_if(value.begin(), value.end(), notSpace));
    value.erase(find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

wstring ToLower(wstring value) {
    transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

bool IsControlCharacter(wchar_t ch) {
    return static_cast<unsigned int>(ch) < 0x20;
}

bool HasWindowsInvalidPathCharacter(wchar_t ch) {
    static const wstring invalid = L"\\/:*?\"<>|";
    return IsControlCharacter(ch) || invalid.find(ch) != wstring::npos;
}

bool IsWindowsReservedDeviceOrdinal(wchar_t ch) {
    return (ch >= L'1' && ch <= L'9')
        || ch == L'\x00B9'
        || ch == L'\x00B2'
        || ch == L'\x00B3';
}

bool IsWindowsReservedDeviceName(const wstring& value) {
    wstring stem = Trim(value);
    if (stem.empty()) return false;
    size_t dotPos = stem.find(L'.');
    if (dotPos != wstring::npos) stem = stem.substr(0, dotPos);
    while (!stem.empty() && (stem.back() == L'.' || iswspace(stem.back()))) stem.pop_back();
    stem = ToLower(stem);
    if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul"
        || stem == L"conin$" || stem == L"conout$") {
        return true;
    }
    if (stem.size() == 4 && (stem.rfind(L"com", 0) == 0 || stem.rfind(L"lpt", 0) == 0)
        && IsWindowsReservedDeviceOrdinal(stem[3])) {
        return true;
    }
    return false;
}

wstring RemapWindowsReservedDeviceName(wstring value) {
    if (!IsWindowsReservedDeviceName(value)) return value;
    size_t dotPos = value.find(L'.');
    if (dotPos == wstring::npos) return value + L"_";
    value.insert(dotPos, L"_");
    return value;
}

bool IsReservedFormatControlSegment(const wstring& value) {
    wstring segment = ToLower(Trim(value));
    return segment == ToLower(kMetadataRootDirName) || segment == ToLower(kCloudStateDirName);
}

bool IsReservedConfigRemoteSegment(const wstring& value) {
    wstring segment = ToLower(Trim(value));
    return IsReservedFormatControlSegment(value) || segment == ToLower(kCloudHistoryFileName);
}

wstring RemapReservedFormatControlSegment(wstring value) {
    if (IsReservedFormatControlSegment(value)) value += L"_";
    return value;
}

wstring RemapReservedConfigRemoteSegment(wstring value) {
    if (IsReservedConfigRemoteSegment(value)) value += L"_";
    return value;
}

wstring SanitizeStorageOrCloudSegment(wstring value) {
    return RemapReservedFormatControlSegment(SanitizePathSegment(value));
}

wstring SanitizeConfigRemoteSegment(wstring value) {
    return RemapReservedConfigRemoteSegment(SanitizePathSegment(value));
}

bool IsSupportedArchiveExtension(const wstring& extension) {
    static const wstring supportedExtensions[] = {
        L"7z", L"zip", L"tar", L"gz", L"gzip", L"xz", L"bz2", L"bzip2", L"zst", L"zstd",
    };
    wstring normalized = ToLower(extension);
    return find(begin(supportedExtensions), end(supportedExtensions), normalized) != end(supportedExtensions);
}

wstring NormalizeArchiveExtension(wstring format) {
    wstring extension = Trim(format);
    while (!extension.empty() && extension.front() == L'.') extension.erase(extension.begin());
    extension = SanitizePathSegment(extension);
    while (!extension.empty() && extension.front() == L'.') extension.erase(extension.begin());
    if (!IsSafeSinglePathSegment(extension) || !IsSupportedArchiveExtension(extension)) return L"7z";
    return ToLower(extension);
}

wstring MakeSafeRemoteSegment(wstring value, const wchar_t* fallback) {
    wstring segment = SanitizeStorageOrCloudSegment(value);
    if (!IsSafeSinglePathSegment(segment)) segment = SanitizeStorageOrCloudSegment(fallback);
    if (!IsSafeSinglePathSegment(segment)) return fallback;
    return segment;
}

wstring MakeSafeConfigRemoteSegment(wstring value) {
    wstring segment = SanitizeConfigRemoteSegment(value);
    if (!IsSafeSinglePathSegment(segment)) segment = SanitizeConfigRemoteSegment(L"DefaultConfig");
    if (!IsSafeSinglePathSegment(segment)) return L"DefaultConfig";
    return segment;
}

wstring MakeSafeFolderRemoteSegment(wstring value) {
    return MakeSafeRemoteSegment(value, L"Folder");
}

wstring MakeSafeArchiveRemoteFileName(wstring value) {
    return MakeSafeRemoteSegment(value, L"Backup.7z");
}

wstring MakeSafeRecordRemoteFileName(const wstring& archiveFileName) {
    return MakeSafeArchiveRemoteFileName(archiveFileName + L".json");
}

wstring NormalizeRemoteSegment(wstring value) {
    replace(value.begin(), value.end(), L'\\', L'/');
    while (!value.empty() && value.front() == L'/') value.erase(value.begin());
    while (!value.empty() && value.back() == L'/') value.pop_back();
    return value;
}

wstring FormatTime(tm value, const wchar_t* pattern) {
    wchar_t buffer[64] = {};
    wcsftime(buffer, size(buffer), pattern, &value);
    return buffer;
}

bool PathEqualsOrUnder(const filesystem::path& candidate, const filesystem::path& root) {
    error_code ec;
    filesystem::path normalizedRoot = filesystem::absolute(root, ec).lexically_normal();
    if (ec) return false;
    filesystem::path normalizedCandidate = filesystem::absolute(candidate, ec).lexically_normal();
    if (ec) return false;
    if (normalizedCandidate == normalizedRoot) return true;
    auto rootIt = normalizedRoot.begin();
    auto candidateIt = normalizedCandidate.begin();
    for (; rootIt != normalizedRoot.end() && candidateIt != normalizedCandidate.end(); ++rootIt, ++candidateIt) {
#ifdef _WIN32
        wstring lhs = rootIt->wstring();
        wstring rhs = candidateIt->wstring();
        transform(lhs.begin(), lhs.end(), lhs.begin(), ::towlower);
        transform(rhs.begin(), rhs.end(), rhs.begin(), ::towlower);
        if (lhs != rhs) return false;
#else
        if (*rootIt != *candidateIt) return false;
#endif
    }
    return rootIt == normalizedRoot.end();
}

} // namespace

wstring GenerateGuidString() {
#ifdef _WIN32
    GUID guid{};
    if (CoCreateGuid(&guid) == S_OK) {
        wchar_t buffer[40] = {};
        StringFromGUID2(guid, buffer, static_cast<int>(size(buffer)));
        wstring value = buffer;
        if (!value.empty() && value.front() == L'{') value.erase(value.begin());
        if (!value.empty() && value.back() == L'}') value.pop_back();
        transform(value.begin(), value.end(), value.begin(), ::towlower);
        return value;
    }
#endif
    auto now = chrono::steady_clock::now().time_since_epoch().count();
    wstringstream fallback;
    fallback << L"minebackup-" << now;
    return fallback.str();
}

wstring EnsureConfigId(wstring currentValue) {
    currentValue = Trim(currentValue);
    return currentValue.empty() ? GenerateGuidString() : currentValue;
}

wstring MakeLocalTimestampString() {
    time_t now = time(nullptr);
    tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif
    return FormatTime(localTime, L"%Y-%m-%d_%H-%M-%S");
}

wstring MakeUtcTimestampString() {
    time_t now = time(nullptr);
    tm utcTime{};
#ifdef _WIN32
    gmtime_s(&utcTime, &now);
#else
    gmtime_r(&now, &utcTime);
#endif
    return FormatTime(utcTime, L"%Y-%m-%dT%H:%M:%SZ");
}

wstring NormalizeRelativePath(filesystem::path relativePath) {
    wstring value = relativePath.lexically_normal().wstring();
    replace(value.begin(), value.end(), L'\\', L'/');
    while (!value.empty() && value.front() == L'/') value.erase(value.begin());
    return value;
}

wstring SanitizePathSegment(wstring value) {
    value = Trim(value);
    for (wchar_t& ch : value) {
        if (HasWindowsInvalidPathCharacter(ch) || ch == L'\0') ch = L'_';
    }
    while (!value.empty() && (value.back() == L'.' || iswspace(value.back()))) value.pop_back();
    return RemapWindowsReservedDeviceName(value);
}

bool IsSafeSinglePathSegment(const wstring& value) {
    if (value.empty() || value == L"." || value == L"..") return false;
    if (value.back() == L'.' || iswspace(value.back())) return false;
    if (IsWindowsReservedDeviceName(value)) return false;
    for (wchar_t ch : value) {
        if (ch == L'\0' || HasWindowsInvalidPathCharacter(ch)) return false;
    }
    filesystem::path path(value);
    if (path.is_absolute()) return false;
    return path.filename().wstring() == value;
}

bool TryResolveStoragePaths(const wstring& backupRoot, const wstring& folderName, const wstring& fallbackPath, StoragePaths& outPaths) {
    outPaths = StoragePaths{};
    if (backupRoot.empty()) return false;
    wstring candidate = SanitizeStorageOrCloudSegment(folderName);
    if (candidate.empty() && !fallbackPath.empty()) {
        candidate = SanitizeStorageOrCloudSegment(filesystem::path(fallbackPath).filename().wstring());
    }
    if (!IsSafeSinglePathSegment(candidate)) return false;
    filesystem::path root(backupRoot);
    filesystem::path metadataRoot = root / kMetadataRootDirName;
    filesystem::path backupSubDir = root / candidate;
    filesystem::path metadataDir = metadataRoot / candidate;
    if (!PathEqualsOrUnder(backupSubDir, root)) return false;
    if (!PathEqualsOrUnder(metadataDir, metadataRoot)) return false;
    outPaths.folderName = candidate;
    outPaths.backupSubDir = backupSubDir;
    outPaths.metadataDir = metadataDir;
    outPaths.recordsDir = metadataDir / kMetadataRecordsDirName;
    outPaths.statePath = metadataDir / kMetadataStateFileName;
    return true;
}

wstring SanitizeArchiveComment(const wstring& comment) {
    wstring value = SanitizePathSegment(comment);
    value.erase(remove(value.begin(), value.end(), L'['), value.end());
    value.erase(remove(value.begin(), value.end(), L']'), value.end());
    return Trim(value);
}

wstring GenerateArchiveFileName(const wstring& backupType, const wstring& folderName, const wstring& comment, const wstring& format) {
    wstring safeBackupType = SanitizePathSegment(backupType);
    if (safeBackupType.empty()) safeBackupType = L"Backup";
    wstring safeFolder = SanitizePathSegment(folderName);
    wstring safeComment = SanitizeArchiveComment(comment);
    wstring extension = NormalizeArchiveExtension(format);
    wstring commentPart = safeComment.empty() ? L"" : L" [" + safeComment + L"]";
    wstring fileName = L"[" + safeBackupType + L"][" + MakeLocalTimestampString() + L"]" + safeFolder + commentPart + L"." + extension;
    if (IsSafeSinglePathSegment(fileName)) return fileName;
    return L"[Backup][" + MakeLocalTimestampString() + L"].7z";
}

bool IsSmartBackupType(const wstring& typeOrFileName) {
    return _wcsicmp(typeOrFileName.c_str(), L"Smart") == 0 || typeOrFileName.find(L"[Smart]") != wstring::npos;
}

bool IsFullLikeBackupType(const wstring& typeOrFileName) {
    return _wcsicmp(typeOrFileName.c_str(), L"Full") == 0
        || _wcsicmp(typeOrFileName.c_str(), L"Overwrite") == 0
        || typeOrFileName.find(L"[Full]") != wstring::npos
        || typeOrFileName.find(L"[Overwrite]") != wstring::npos;
}

wstring AppendRemotePath(const wstring& root, initializer_list<wstring> segments) {
    wstring result = root;
    replace(result.begin(), result.end(), L'\\', L'/');
    while (!result.empty() && result.back() == L'/') result.pop_back();
    for (wstring segment : segments) {
        segment = NormalizeRemoteSegment(segment);
        if (segment.empty()) continue;
        if (!result.empty()) result += L"/";
        result += segment;
    }
    return result;
}

wstring BuildConfigCloudRoot(const Config& config) {
    wstring configName = MakeSafeConfigRemoteSegment(utf8_to_wstring(config.name));
    return AppendRemotePath(config.rcloneRemotePath, { configName });
}

wstring BuildArchiveRemotePath(const Config& config, const wstring& folderName, const wstring& archiveFileName) {
    return AppendRemotePath(BuildConfigCloudRoot(config), { MakeSafeFolderRemoteSegment(folderName), MakeSafeArchiveRemoteFileName(archiveFileName) });
}

wstring BuildMetadataStateRemotePath(const Config& config, const wstring& folderName) {
    return AppendRemotePath(BuildConfigCloudRoot(config), { MakeSafeFolderRemoteSegment(folderName), kMetadataRootDirName, kMetadataStateFileName });
}

wstring BuildMetadataRecordRemotePath(const Config& config, const wstring& folderName, const wstring& archiveFileName) {
    return AppendRemotePath(BuildConfigCloudRoot(config), { MakeSafeFolderRemoteSegment(folderName), kMetadataRootDirName, kMetadataRecordsDirName, MakeSafeRecordRemoteFileName(archiveFileName) });
}

wstring BuildActiveHistoryManifestRemotePath(const Config& config) {
    return AppendRemotePath(BuildConfigCloudRoot(config), { kCloudStateDirName, kCloudActiveHistoryFileName });
}

wstring BuildGlobalHistoryRemotePath(const Config& config) {
    return AppendRemotePath(config.rcloneRemotePath, { kCloudHistoryFileName });
}

} // namespace FolderRewindFormat
