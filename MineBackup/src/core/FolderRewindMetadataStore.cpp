#include "FolderRewindMetadataStore.h"

#include "AtomicFileWriter.h"
#include "json.hpp"
#include "PlatformCompat.h"
#include "text_to_text.h"

#include <algorithm>
#include <exception>
#include <fstream>

using namespace std;

namespace FolderRewindMetadataStore {
namespace {

SaveResult Failure(const wstring& error) {
    return {false, error};
}

wstring SystemErrorText(const wchar_t* operation, const error_code& error) {
    wstring text(operation);
    if (!error) return text;
    text += L" (native=" + to_wstring(error.value()) + L"): ";
    text += utf8_to_wstring(error.message());
    return text;
}

SaveResult ExceptionFailure(const wchar_t* operation, const exception& error) {
    return Failure(wstring(operation) + L": " + utf8_to_wstring(error.what()));
}

SaveResult AtomicWriteJson(const filesystem::path& path, const nlohmann::json& value) {
    try {
        const auto write = AtomicFileWriter::WriteText(path, value.dump(2));
        if (!write.success) return Failure(write.error);
        return {true, {}};
    }
    catch (const exception& error) {
        return ExceptionFailure(L"Could not serialize metadata JSON", error);
    }
    catch (...) {
        return Failure(L"Could not serialize metadata JSON: unknown exception.");
    }
}

wstring JsonString(const nlohmann::json& item, const char* key) {
    const auto it = item.find(key);
    if (it == item.end() || !it->is_string()) return L"";
    return utf8_to_wstring(it->get<string>());
}

bool HasSafeRawRelativeSegments(const wstring& value) {
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find_first_of(L"\\/", start);
        const wstring segment = value.substr(start, end == wstring::npos ? wstring::npos : end - start);
        if (segment.empty() || segment == L"." || segment == L"..") return false;
        if (end == wstring::npos) break;
        start = end + 1;
    }
    return true;
}

bool TryNormalizeRelativeEntry(const wstring& value, wstring& outValue) {
    outValue.clear();
    if (value.empty() || !HasSafeRawRelativeSegments(value)) return false;

    filesystem::path raw(value);
    if (raw.empty() || raw.is_absolute() || raw.has_root_name() || raw.has_root_directory()) return false;

    for (const auto& part : raw) {
        const wstring segment = part.wstring();
        if (segment.empty() || segment == L"." || segment == L"..") return false;
    }

    const wstring normalized = FolderRewindFormat::NormalizeRelativePath(raw);
    if (normalized.empty() || normalized == L"." || normalized == L"..") return false;

    filesystem::path normalizedPath(normalized);
    if (normalizedPath.is_absolute() || normalizedPath.has_root_name() || normalizedPath.has_root_directory()) return false;

    for (const auto& part : normalizedPath) {
        const wstring segment = part.wstring();
        if (segment.empty() || segment == L"." || segment == L"..") return false;
        if (!FolderRewindFormat::IsSafeSinglePathSegment(segment)) return false;
    }

    outValue = normalized;
    return true;
}

bool IsSafeArchiveReference(const wstring& value, bool allowEmpty) {
    if (value.empty()) return allowEmpty;
    return FolderRewindFormat::IsSafeSinglePathSegment(value);
}

nlohmann::json SerializeFileState(const FolderRewindFormat::FileState& state) {
    nlohmann::json item;
    item["Size"] = state.size;
    item["LastWriteTimeUtc"] = wstring_to_utf8(state.lastWriteTimeUtc);
    item["Hash"] = wstring_to_utf8(state.hash);
    return item;
}

bool TryParseFileState(const nlohmann::json& item, FolderRewindFormat::FileState& outState) {
    if (!item.is_object()) return false;

    FolderRewindFormat::FileState state;
    const auto sizeIt = item.find("Size");
    if (sizeIt == item.end()) return false;
    if (sizeIt->is_number_unsigned()) {
        state.size = sizeIt->get<uintmax_t>();
    }
    else if (sizeIt->is_number_integer()) {
        const auto signedSize = sizeIt->get<int64_t>();
        if (signedSize < 0) return false;
        state.size = static_cast<uintmax_t>(signedSize);
    }
    else {
        return false;
    }
    state.lastWriteTimeUtc = JsonString(item, "LastWriteTimeUtc");
    if (state.lastWriteTimeUtc.empty()) return false;
    state.hash = JsonString(item, "Hash");

    outState = std::move(state);
    return true;
}

bool TryLoadStringArray(const nlohmann::json& parent, const char* key, vector<wstring>& out) {
    out.clear();

    const auto it = parent.find(key);
    if (it == parent.end()) return true;
    if (!it->is_array()) return false;

    for (const auto& item : *it) {
        if (!item.is_string()) return false;

        wstring normalized;
        if (!TryNormalizeRelativeEntry(utf8_to_wstring(item.get<string>()), normalized)) return false;
        out.push_back(normalized);
    }
    sort(out.begin(), out.end());
    return adjacent_find(out.begin(), out.end()) == out.end();
}

bool WriteStringArray(nlohmann::json& parent, const char* key, const vector<wstring>& values) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& value : values) {
        wstring normalized;
        if (!TryNormalizeRelativeEntry(value, normalized)) return false;
        array.push_back(wstring_to_utf8(normalized));
    }
    parent[key] = std::move(array);
    return true;
}

bool TrySerializeRecord(const FolderRewindFormat::ChangeRecord& record, nlohmann::json& root) {
    if (!IsSafeArchiveReference(record.archiveFileName, false)
        || !IsSafeArchiveReference(record.basedOnFullBackup, true)
        || !IsSafeArchiveReference(record.previousBackupFileName, true)) {
        return false;
    }

    root = nlohmann::json::object();
    root["ArchiveFileName"] = wstring_to_utf8(record.archiveFileName);
    root["BackupType"] = wstring_to_utf8(record.backupType);
    root["BasedOnFullBackup"] = wstring_to_utf8(record.basedOnFullBackup);
    root["PreviousBackupFileName"] = wstring_to_utf8(record.previousBackupFileName);
    root["CreatedAtUtc"] = wstring_to_utf8(record.createdAtUtc);
    return WriteStringArray(root, "AddedFiles", record.addedFiles)
        && WriteStringArray(root, "ModifiedFiles", record.modifiedFiles)
        && WriteStringArray(root, "DeletedFiles", record.deletedFiles)
        && WriteStringArray(root, "FullFileList", record.fullFileList);
}

bool TryParseRecord(const nlohmann::json& root, const wstring& fallbackArchiveFileName, FolderRewindFormat::ChangeRecord& outRecord) {
    if (!root.is_object()) return false;

    const auto archiveFileNameIt = root.find("ArchiveFileName");
    if (archiveFileNameIt == root.end() || !archiveFileNameIt->is_string()) return false;

    FolderRewindFormat::ChangeRecord record;
    record.archiveFileName = utf8_to_wstring(archiveFileNameIt->get<string>());
    if (!IsSafeArchiveReference(record.archiveFileName, false)) return false;
    if (!fallbackArchiveFileName.empty() && _wcsicmp(record.archiveFileName.c_str(), fallbackArchiveFileName.c_str()) != 0) return false;

    record.backupType = JsonString(root, "BackupType");
    record.basedOnFullBackup = JsonString(root, "BasedOnFullBackup");
    record.previousBackupFileName = JsonString(root, "PreviousBackupFileName");
    if (!IsSafeArchiveReference(record.basedOnFullBackup, true)
        || !IsSafeArchiveReference(record.previousBackupFileName, true)) {
        return false;
    }

    record.createdAtUtc = JsonString(root, "CreatedAtUtc");
    if (!TryLoadStringArray(root, "AddedFiles", record.addedFiles)
        || !TryLoadStringArray(root, "ModifiedFiles", record.modifiedFiles)
        || !TryLoadStringArray(root, "DeletedFiles", record.deletedFiles)
        || !TryLoadStringArray(root, "FullFileList", record.fullFileList)) {
        return false;
    }

    outRecord = std::move(record);
    return true;
}

} // namespace

filesystem::path GetStatePath(const filesystem::path& metadataDir) {
    try {
        return metadataDir / FolderRewindFormat::kMetadataStateFileName;
    }
    catch (...) {
        return {};
    }
}

filesystem::path GetRecordsDir(const filesystem::path& metadataDir) {
    try {
        return metadataDir / FolderRewindFormat::kMetadataRecordsDirName;
    }
    catch (...) {
        return {};
    }
}

optional<filesystem::path> TryGetRecordPath(const filesystem::path& metadataDir, const wstring& archiveFileName) {
    try {
        if (!FolderRewindFormat::IsSafeSinglePathSegment(archiveFileName)) return nullopt;
        return GetRecordsDir(metadataDir) / (archiveFileName + L".json");
    }
    catch (...) {
        return nullopt;
    }
}

bool LoadState(const filesystem::path& metadataDir, FolderRewindFormat::MetadataState& outState) {
    try {
        const filesystem::path statePath = GetStatePath(metadataDir);
        if (statePath.empty() || !filesystem::exists(statePath)) return false;

        ifstream in(statePath, ios::binary);
        if (!in.is_open()) return false;

        const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
        if (root.is_discarded() || !root.is_object()) return false;

        FolderRewindFormat::MetadataState state;
        state.version = JsonString(root, "Version");
        if (state.version.empty()) state.version = L"3.0";
        state.lastBackupTime = JsonString(root, "LastBackupTime");
        state.lastBackupFileName = JsonString(root, "LastBackupFileName");
        state.basedOnFullBackup = JsonString(root, "BasedOnFullBackup");
        if (!IsSafeArchiveReference(state.lastBackupFileName, true)
            || !IsSafeArchiveReference(state.basedOnFullBackup, true)) {
            return false;
        }

        const auto fileStatesIt = root.find("FileStates");
        if (fileStatesIt == root.end() || !fileStatesIt->is_object()) return false;

        for (const auto& item : fileStatesIt->items()) {
            wstring relativePath;
            if (!TryNormalizeRelativeEntry(utf8_to_wstring(item.key()), relativePath)) return false;

            FolderRewindFormat::FileState fileState;
            if (!TryParseFileState(item.value(), fileState)) return false;
            state.fileStates[relativePath] = std::move(fileState);
        }

        outState = std::move(state);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool LoadRecord(const filesystem::path& metadataDir, const wstring& archiveFileName, FolderRewindFormat::ChangeRecord& outRecord) {
    try {
        const auto recordPath = TryGetRecordPath(metadataDir, archiveFileName);
        if (!recordPath || !filesystem::exists(*recordPath)) return false;

        ifstream in(*recordPath, ios::binary);
        if (!in.is_open()) return false;

        const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
        if (root.is_discarded()) return false;
        return TryParseRecord(root, archiveFileName, outRecord);
    }
    catch (...) {
        return false;
    }
}

LoadResult Load(const filesystem::path& metadataDir, const vector<wstring>& requestedArchiveFileNames) {
    LoadResult result;

    try {
        error_code ec;
        result.metadataExists = filesystem::exists(metadataDir, ec);
        if (ec) {
            result.metadataExists = false;
            result.stateLoadFailed = true;
            result.stateMalformed = true;
            result.recordLoadFailed = true;
            if (!requestedArchiveFileNames.empty()) result.requestedRecordsMalformed = true;
            return result;
        }

        const filesystem::path statePath = GetStatePath(metadataDir);
        if (!statePath.empty()) {
            result.stateFileExists = filesystem::exists(statePath, ec);
            if (ec) {
                result.stateFileExists = false;
                result.stateLoadFailed = true;
                result.stateMalformed = true;
                ec.clear();
            }
            else {
                result.stateMissing = !result.stateFileExists;
            }
        }
        else {
            result.stateLoadFailed = true;
            result.stateMalformed = true;
        }

        if (result.stateFileExists) {
            result.stateLoaded = LoadState(metadataDir, result.state);
            result.stateLoadFailed = !result.stateLoaded;
            result.stateMalformed = !result.stateLoaded;
        }

        const filesystem::path recordsDir = GetRecordsDir(metadataDir);
        if (!recordsDir.empty()) {
            result.recordsDirExists = filesystem::exists(recordsDir, ec);
            if (ec) {
                result.recordsDirExists = false;
                result.recordLoadFailed = true;
                if (!requestedArchiveFileNames.empty()) result.requestedRecordsMalformed = true;
                ec.clear();
            }
        }
        else {
            result.recordLoadFailed = true;
            if (!requestedArchiveFileNames.empty()) result.requestedRecordsMalformed = true;
        }

        vector<wstring> recordsToLoad;
        recordsToLoad.reserve(requestedArchiveFileNames.size());
        for (const auto& archiveFileName : requestedArchiveFileNames) {
            if (FolderRewindFormat::IsSafeSinglePathSegment(archiveFileName)) {
                recordsToLoad.push_back(archiveFileName);
            }
            else {
                result.recordLoadFailed = true;
                result.hasMissingRequestedRecords = true;
                result.requestedRecordsMalformed = true;
            }
        }

        if (requestedArchiveFileNames.empty() && result.recordsDirExists) {
            for (const auto& entry : filesystem::directory_iterator(recordsDir, ec)) {
                if (ec) break;

                const bool isRegularFile = entry.is_regular_file(ec);
                if (ec) {
                    result.recordLoadFailed = true;
                    result.requestedRecordsMalformed = true;
                    ec.clear();
                    continue;
                }
                if (!isRegularFile) continue;
                if (entry.path().extension().wstring() != L".json") continue;

                const wstring archiveFileName = entry.path().stem().wstring();
                if (FolderRewindFormat::IsSafeSinglePathSegment(archiveFileName)) {
                    recordsToLoad.push_back(archiveFileName);
                }
                else {
                    result.recordLoadFailed = true;
                    result.requestedRecordsMalformed = true;
                }
            }
            if (ec) {
                result.recordLoadFailed = true;
                result.requestedRecordsMalformed = true;
                ec.clear();
            }
        }

        for (const auto& archiveFileName : recordsToLoad) {
            const auto recordPath = TryGetRecordPath(metadataDir, archiveFileName);
            if (!recordPath) {
                result.recordLoadFailed = true;
                if (!requestedArchiveFileNames.empty()) {
                    result.hasMissingRequestedRecords = true;
                    result.requestedRecordsMalformed = true;
                }
                continue;
            }

            const bool recordFileExists = filesystem::exists(*recordPath, ec);
            if (ec) {
                result.recordLoadFailed = true;
                if (!requestedArchiveFileNames.empty()) {
                    result.hasMissingRequestedRecords = true;
                    result.requestedRecordsMalformed = true;
                }
                ec.clear();
                continue;
            }

            FolderRewindFormat::ChangeRecord record;
            if (recordFileExists && LoadRecord(metadataDir, archiveFileName, record)) {
                result.records[record.archiveFileName] = std::move(record);
            }
            else {
                result.recordLoadFailed = true;
                if (!requestedArchiveFileNames.empty()) {
                    result.hasMissingRequestedRecords = true;
                    if (recordFileExists) {
                        result.requestedRecordsMalformed = true;
                    }
                    else {
                        result.requestedRecordsMissing = true;
                    }
                }
            }
        }
    }
    catch (...) {
        result.recordLoadFailed = true;
        if (!requestedArchiveFileNames.empty()) result.requestedRecordsMalformed = true;
    }

    return result;
}

SaveResult SaveStateDetailed(
    const filesystem::path& metadataDir,
    const FolderRewindFormat::MetadataState& state) {
    try {
        error_code error;
        filesystem::create_directories(metadataDir, error);
        if (error) return Failure(SystemErrorText(L"Could not create metadata directory", error));

        nlohmann::json root;
        root["Version"] = "3.0";
        if (!IsSafeArchiveReference(state.lastBackupFileName, true)
            || !IsSafeArchiveReference(state.basedOnFullBackup, true)) {
            return Failure(L"Could not serialize metadata state: unsafe archive reference.");
        }

        root["LastBackupTime"] = wstring_to_utf8(state.lastBackupTime);
        root["LastBackupFileName"] = wstring_to_utf8(state.lastBackupFileName);
        root["BasedOnFullBackup"] = wstring_to_utf8(state.basedOnFullBackup);
        root["FileStates"] = nlohmann::json::object();

        for (const auto& item : state.fileStates) {
            wstring relativePath;
            if (!TryNormalizeRelativeEntry(item.first, relativePath)) {
                return Failure(L"Could not serialize metadata state: unsafe file-state path.");
            }
            root["FileStates"][wstring_to_utf8(relativePath)] = SerializeFileState(item.second);
        }

        const filesystem::path statePath = GetStatePath(metadataDir);
        if (statePath.empty()) return Failure(L"Could not resolve metadata state path.");

        return AtomicWriteJson(statePath, root);
    }
    catch (const exception& error) {
        return ExceptionFailure(L"Could not save metadata state", error);
    }
    catch (...) {
        return Failure(L"Could not save metadata state: unknown exception.");
    }
}

SaveResult SaveRecordDetailed(
    const filesystem::path& metadataDir,
    const FolderRewindFormat::ChangeRecord& record) {
    try {
        const auto recordPath = TryGetRecordPath(metadataDir, record.archiveFileName);
        if (!recordPath) {
            return Failure(L"Could not serialize change record: unsafe archive file name.");
        }

        nlohmann::json root;
        if (!TrySerializeRecord(record, root)) {
            return Failure(L"Could not serialize change record: unsafe metadata value.");
        }

        const filesystem::path recordsDirectory = GetRecordsDir(metadataDir);
        if (recordsDirectory.empty()) return Failure(L"Could not resolve metadata records directory.");
        error_code error;
        filesystem::create_directories(recordsDirectory, error);
        if (error) return Failure(SystemErrorText(L"Could not create metadata records directory", error));

        return AtomicWriteJson(*recordPath, root);
    }
    catch (const exception& error) {
        return ExceptionFailure(L"Could not save change record", error);
    }
    catch (...) {
        return Failure(L"Could not save change record: unknown exception.");
    }
}

bool SaveState(const filesystem::path& metadataDir, const FolderRewindFormat::MetadataState& state) {
    return SaveStateDetailed(metadataDir, state).success;
}

bool SaveRecord(const filesystem::path& metadataDir, const FolderRewindFormat::ChangeRecord& record) {
    return SaveRecordDetailed(metadataDir, record).success;
}

bool Save(const filesystem::path& metadataDir, const FolderRewindFormat::MetadataState& state, const FolderRewindFormat::ChangeRecord& record) {
    try {
        return SaveRecord(metadataDir, record) && SaveState(metadataDir, state);
    }
    catch (...) {
        return false;
    }
}

bool DeleteRecord(const filesystem::path& metadataDir, const wstring& archiveFileName) {
    try {
        const auto recordPath = TryGetRecordPath(metadataDir, archiveFileName);
        if (!recordPath) return false;

        error_code ec;
        filesystem::remove(*recordPath, ec);
        return !ec;
    }
    catch (...) {
        return false;
    }
}

bool RewriteRecordArchiveName(const filesystem::path& metadataDir, const wstring& oldArchiveFileName, const wstring& newArchiveFileName) {
    try {
        if (!FolderRewindFormat::IsSafeSinglePathSegment(oldArchiveFileName)
            || !FolderRewindFormat::IsSafeSinglePathSegment(newArchiveFileName)) {
            return false;
        }

        if (_wcsicmp(oldArchiveFileName.c_str(), newArchiveFileName.c_str()) == 0) return true;

        const auto newRecordPath = TryGetRecordPath(metadataDir, newArchiveFileName);
        if (!newRecordPath) return false;

        error_code ec;
        if (filesystem::exists(*newRecordPath, ec) || ec) return false;

        FolderRewindFormat::ChangeRecord record;
        if (!LoadRecord(metadataDir, oldArchiveFileName, record)) return false;

        record.archiveFileName = newArchiveFileName;
        if (_wcsicmp(record.basedOnFullBackup.c_str(), oldArchiveFileName.c_str()) == 0) {
            record.basedOnFullBackup = newArchiveFileName;
        }
        if (_wcsicmp(record.previousBackupFileName.c_str(), oldArchiveFileName.c_str()) == 0) {
            record.previousBackupFileName = newArchiveFileName;
        }

        if (!SaveRecord(metadataDir, record)) return false;
        return DeleteRecord(metadataDir, oldArchiveFileName);
    }
    catch (...) {
        return false;
    }
}

} // namespace FolderRewindMetadataStore
