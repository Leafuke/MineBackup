#pragma once
#ifndef FOLDER_REWIND_FORMAT_H
#define FOLDER_REWIND_FORMAT_H

#include "DataModels.h"

#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace FolderRewindFormat {

constexpr const wchar_t* kMetadataRootDirName = L"_metadata";
constexpr const wchar_t* kMetadataStateFileName = L"state.json";
constexpr const wchar_t* kMetadataRecordsDirName = L"records";
constexpr const wchar_t* kCloudStateDirName = L"_folderrewind";
constexpr const wchar_t* kCloudActiveHistoryFileName = L"active-history.json";
constexpr const wchar_t* kCloudHistoryFileName = L"history.json";
constexpr const wchar_t* kInternalRestoreMarkerDirectoryName = L"__FolderRewind_Internal";
constexpr const wchar_t* kInternalRestoreMarkerFileName = L"__DeletedOnly.marker";

struct StoragePaths {
    std::wstring folderName;
    std::filesystem::path backupSubDir;
    std::filesystem::path metadataDir;
    std::filesystem::path recordsDir;
    std::filesystem::path statePath;
};

struct FileState {
    uintmax_t size = 0;
    std::wstring lastWriteTimeUtc;
    std::wstring hash;
};

struct MetadataState {
    std::wstring version = L"3.0";
    std::wstring lastBackupTime;
    std::wstring lastBackupFileName;
    std::wstring basedOnFullBackup;
    std::map<std::wstring, FileState> fileStates;
};

struct ChangeRecord {
    std::wstring archiveFileName;
    std::wstring backupType;
    std::wstring basedOnFullBackup;
    std::wstring previousBackupFileName;
    std::wstring createdAtUtc;
    std::vector<std::wstring> addedFiles;
    std::vector<std::wstring> modifiedFiles;
    std::vector<std::wstring> deletedFiles;
    // Complete snapshots belong to Full/Overwrite checkpoints. Smart records
    // keep this field empty and reconstruct state from their deltas.
    std::vector<std::wstring> fullFileList;
};

struct ActiveHistoryEntry {
    std::wstring folderPath;
    std::wstring folderName;
    std::wstring fileName;
    std::wstring timestamp;
};

struct ActiveHistoryManifest {
    std::wstring configId;
    std::wstring configName;
    std::wstring updatedAtUtc;
    std::vector<ActiveHistoryEntry> entries;
};

std::wstring GenerateGuidString();
std::wstring EnsureConfigId(std::wstring currentValue);
std::wstring MakeLocalTimestampString();
std::wstring MakeLocalHistoryTimestampString();
std::wstring FormatFileTimeUtc(std::filesystem::file_time_type fileTime);
std::wstring MakeUtcTimestampString();
std::wstring NormalizeRelativePath(std::filesystem::path relativePath);
std::wstring SanitizePathSegment(std::wstring value);
bool IsSafeSinglePathSegment(const std::wstring& value);
bool TryResolveStoragePaths(const std::wstring& backupRoot, const std::wstring& folderName, const std::wstring& fallbackPath, StoragePaths& outPaths);
std::wstring SanitizeArchiveComment(const std::wstring& comment);
std::wstring GenerateArchiveFileName(const std::wstring& backupType, const std::wstring& folderName, const std::wstring& comment, const std::wstring& format);
bool IsSmartBackupType(const std::wstring& typeOrFileName);
bool IsFullLikeBackupType(const std::wstring& typeOrFileName);
std::wstring AppendRemotePath(const std::wstring& root, std::initializer_list<std::wstring> segments);
std::wstring BuildConfigCloudRoot(const Config& config);
std::wstring BuildArchiveRemotePath(const Config& config, const std::wstring& folderName, const std::wstring& archiveFileName);
std::wstring BuildMetadataStateRemotePath(const Config& config, const std::wstring& folderName);
std::wstring BuildMetadataRecordRemotePath(const Config& config, const std::wstring& folderName, const std::wstring& archiveFileName);
std::wstring BuildActiveHistoryManifestRemotePath(const Config& config);
std::wstring BuildGlobalHistoryRemotePath(const Config& config);

} // namespace FolderRewindFormat

#endif // FOLDER_REWIND_FORMAT_H
