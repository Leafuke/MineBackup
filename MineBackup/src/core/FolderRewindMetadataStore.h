#pragma once
#ifndef FOLDER_REWIND_METADATA_STORE_H
#define FOLDER_REWIND_METADATA_STORE_H

#include "FolderRewindFormat.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace FolderRewindMetadataStore {

struct LoadResult {
    FolderRewindFormat::MetadataState state;
    std::map<std::wstring, FolderRewindFormat::ChangeRecord> records;
    bool metadataExists = false;
    bool stateLoadFailed = false;
    bool recordLoadFailed = false;
    bool hasMissingRequestedRecords = false;
    bool stateFileExists = false;
    bool recordsDirExists = false;
    bool stateLoaded = false;
    bool stateMissing = false;
    bool stateMalformed = false;
    bool requestedRecordsMissing = false;
    bool requestedRecordsMalformed = false;
};

std::filesystem::path GetStatePath(const std::filesystem::path& metadataDir);
std::filesystem::path GetRecordsDir(const std::filesystem::path& metadataDir);
std::optional<std::filesystem::path> TryGetRecordPath(const std::filesystem::path& metadataDir, const std::wstring& archiveFileName);
bool LoadState(const std::filesystem::path& metadataDir, FolderRewindFormat::MetadataState& outState);
bool LoadRecord(const std::filesystem::path& metadataDir, const std::wstring& archiveFileName, FolderRewindFormat::ChangeRecord& outRecord);
LoadResult Load(const std::filesystem::path& metadataDir, const std::vector<std::wstring>& requestedArchiveFileNames = {});
bool SaveState(const std::filesystem::path& metadataDir, const FolderRewindFormat::MetadataState& state);
bool SaveRecord(const std::filesystem::path& metadataDir, const FolderRewindFormat::ChangeRecord& record);
bool Save(const std::filesystem::path& metadataDir, const FolderRewindFormat::MetadataState& state, const FolderRewindFormat::ChangeRecord& record);
bool DeleteRecord(const std::filesystem::path& metadataDir, const std::wstring& archiveFileName);
bool RewriteRecordArchiveName(const std::filesystem::path& metadataDir, const std::wstring& oldArchiveFileName, const std::wstring& newArchiveFileName);

} // namespace FolderRewindMetadataStore

#endif // FOLDER_REWIND_METADATA_STORE_H
