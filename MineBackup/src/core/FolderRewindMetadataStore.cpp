#include "FolderRewindMetadataStore.h"

#include "AtomicFileWriter.h"
#include "json.hpp"
#include "PlatformCompat.h"
#include "text_to_text.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <limits>
#include <utility>

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

string JsonStringLiteral(const wstring& value) {
    return nlohmann::json(wstring_to_utf8(value)).dump();
}

bool WriteStringArrayJson(
    const AtomicFileWriter::ChunkSink& sink,
    const char* name,
    const vector<wstring>& values) {
    if (!sink("  \"") || !sink(name) || !sink("\": [")) return false;
    bool first = true;
    for (const auto& value : values) {
        wstring normalized;
        if (!TryNormalizeRelativeEntry(value, normalized)) return false;
        if (!first && !sink(", ")) return false;
        if (!sink(JsonStringLiteral(normalized))) return false;
        first = false;
    }
    return sink("]");
}

bool WriteStateJson(
    const AtomicFileWriter::ChunkSink& sink,
    const FolderRewindFormat::MetadataState& state) {
    if (!sink("{\n  \"Version\": \"3.0\",\n  \"LastBackupTime\": ")
        || !sink(JsonStringLiteral(state.lastBackupTime))
        || !sink(",\n  \"LastBackupFileName\": ")
        || !sink(JsonStringLiteral(state.lastBackupFileName))
        || !sink(",\n  \"BasedOnFullBackup\": ")
        || !sink(JsonStringLiteral(state.basedOnFullBackup))
        || !sink(",\n  \"FileStates\": {")) {
        return false;
    }

    bool first = true;
    for (const auto& [rawPath, fileState] : state.fileStates) {
        wstring relativePath;
        if (!TryNormalizeRelativeEntry(rawPath, relativePath)) return false;
        if (!first && !sink(",")) return false;
        if (!sink("\n    ")
            || !sink(JsonStringLiteral(relativePath))
            || !sink(": {\"Size\": ")
            || !sink(to_string(fileState.size))
            || !sink(", \"LastWriteTimeUtc\": ")
            || !sink(JsonStringLiteral(fileState.lastWriteTimeUtc))
            || !sink(", \"Hash\": ")
            || !sink(JsonStringLiteral(fileState.hash))
            || !sink("}")) {
            return false;
        }
        first = false;
    }
    return sink(first ? "}\n}\n" : "\n  }\n}\n");
}

bool WriteRecordJson(
    const AtomicFileWriter::ChunkSink& sink,
    const FolderRewindFormat::ChangeRecord& record) {
    if (!sink("{\n  \"ArchiveFileName\": ")
        || !sink(JsonStringLiteral(record.archiveFileName))
        || !sink(",\n  \"BackupType\": ")
        || !sink(JsonStringLiteral(record.backupType))
        || !sink(",\n  \"BasedOnFullBackup\": ")
        || !sink(JsonStringLiteral(record.basedOnFullBackup))
        || !sink(",\n  \"PreviousBackupFileName\": ")
        || !sink(JsonStringLiteral(record.previousBackupFileName))
        || !sink(",\n  \"CreatedAtUtc\": ")
        || !sink(JsonStringLiteral(record.createdAtUtc))
        || !sink(",\n")) {
        return false;
    }
    if (!WriteStringArrayJson(sink, "AddedFiles", record.addedFiles) || !sink(",\n")
        || !WriteStringArrayJson(sink, "ModifiedFiles", record.modifiedFiles) || !sink(",\n")
        || !WriteStringArrayJson(sink, "DeletedFiles", record.deletedFiles) || !sink(",\n")
        || !WriteStringArrayJson(sink, "FullFileList", record.fullFileList)) {
        return false;
    }
    return sink("\n}\n");
}

class StateSaxHandler final : public nlohmann::json_sax<nlohmann::json> {
public:
    using number_integer_t = nlohmann::json::number_integer_t;
    using number_unsigned_t = nlohmann::json::number_unsigned_t;
    using number_float_t = nlohmann::json::number_float_t;
    using string_t = nlohmann::json::string_t;

    bool Complete(FolderRewindFormat::MetadataState& outState) {
        if (!rootEnded_ || !sawFileStates_ || !frames_.empty()) return false;
        if (state_.version.empty()) state_.version = L"3.0";
        if (!IsSafeArchiveReference(state_.lastBackupFileName, true)
            || !IsSafeArchiveReference(state_.basedOnFullBackup, true)) {
            return false;
        }
        outState = std::move(state_);
        return true;
    }

    bool null() override { return Primitive(); }
    bool boolean(bool) override { return Primitive(); }
    bool number_integer(number_integer_t value) override {
        if (frames_.empty()) return false;
        Frame& frame = frames_.back();
        if (frame.kind == Kind::FileStates) return false;
        if (frame.kind == Kind::FileState && frame.key == "Size") {
            if (value < 0) return false;
            frame.fileState.size = static_cast<uintmax_t>(value);
            frame.sizeSeen = true;
        }
        return true;
    }
    bool number_unsigned(number_unsigned_t value) override {
        if (frames_.empty()) return false;
        Frame& frame = frames_.back();
        if (frame.kind == Kind::FileStates) return false;
        if (frame.kind == Kind::FileState && frame.key == "Size") {
            if (value > (numeric_limits<uintmax_t>::max)()) return false;
            frame.fileState.size = static_cast<uintmax_t>(value);
            frame.sizeSeen = true;
        }
        return true;
    }
    bool number_float(number_float_t, const string_t&) override { return Primitive(); }
    bool string(string_t& value) override {
        if (frames_.empty()) return false;
        Frame& frame = frames_.back();
        if (frame.kind == Kind::FileStates) return false;
        const wstring converted = utf8_to_wstring(value);
        if (frame.kind == Kind::Root) {
            if (frame.key == "Version") state_.version = converted;
            else if (frame.key == "LastBackupTime") state_.lastBackupTime = converted;
            else if (frame.key == "LastBackupFileName") state_.lastBackupFileName = converted;
            else if (frame.key == "BasedOnFullBackup") state_.basedOnFullBackup = converted;
        }
        else if (frame.kind == Kind::FileState) {
            if (frame.key == "LastWriteTimeUtc") frame.fileState.lastWriteTimeUtc = converted;
            else if (frame.key == "Hash") frame.fileState.hash = converted;
        }
        return true;
    }
    bool start_object(size_t) override {
        if (frames_.empty()) {
            if (rootStarted_) return false;
            rootStarted_ = true;
            frames_.push_back({Kind::Root});
            return true;
        }

        Frame& parent = frames_.back();
        if (parent.kind == Kind::Root && parent.key == "FileStates") {
            state_.fileStates.clear();
            sawFileStates_ = true;
            parent.key.clear();
            frames_.push_back({Kind::FileStates});
            return true;
        }
        if (parent.kind == Kind::FileStates) {
            wstring relativePath;
            if (!TryNormalizeRelativeEntry(utf8_to_wstring(parent.key), relativePath)) return false;
            parent.key.clear();
            Frame frame{Kind::FileState};
            frame.relativePath = std::move(relativePath);
            frames_.push_back(std::move(frame));
            return true;
        }
        frames_.push_back({Kind::IgnoredObject});
        return true;
    }
    bool key(string_t& value) override {
        if (frames_.empty()) return false;
        Frame& frame = frames_.back();
        frame.key = value;
        if (frame.kind == Kind::Root) {
            if (value == "Version") state_.version.clear();
            else if (value == "LastBackupTime") state_.lastBackupTime.clear();
            else if (value == "LastBackupFileName") state_.lastBackupFileName.clear();
            else if (value == "BasedOnFullBackup") state_.basedOnFullBackup.clear();
        }
        else if (frame.kind == Kind::FileState) {
            if (value == "Size") frame.sizeSeen = false;
            else if (value == "LastWriteTimeUtc") frame.fileState.lastWriteTimeUtc.clear();
            else if (value == "Hash") frame.fileState.hash.clear();
        }
        return true;
    }
    bool end_object() override {
        if (frames_.empty()) return false;
        Frame frame = std::move(frames_.back());
        frames_.pop_back();
        if (frame.kind == Kind::FileState) {
            if (!frame.sizeSeen || frame.fileState.lastWriteTimeUtc.empty()) return false;
            state_.fileStates[frame.relativePath] = std::move(frame.fileState);
        }
        else if (frame.kind == Kind::Root) {
            if (!frames_.empty()) return false;
            rootEnded_ = true;
        }
        return true;
    }
    bool start_array(size_t) override {
        if (frames_.empty()) return false;
        const Frame& parent = frames_.back();
        if (parent.kind == Kind::FileStates
            || (parent.kind == Kind::Root && parent.key == "FileStates")) {
            return false;
        }
        frames_.push_back({Kind::IgnoredArray});
        return true;
    }
    bool end_array() override {
        if (frames_.empty() || frames_.back().kind != Kind::IgnoredArray) return false;
        frames_.pop_back();
        return true;
    }
    bool parse_error(size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }

private:
    enum class Kind { Root, FileStates, FileState, IgnoredObject, IgnoredArray };
    struct Frame {
        Kind kind;
        std::string key;
        wstring relativePath;
        FolderRewindFormat::FileState fileState;
        bool sizeSeen = false;
    };

    bool Primitive() {
        if (frames_.empty()) return false;
        const Frame& frame = frames_.back();
        return frame.kind != Kind::FileStates
            && !(frame.kind == Kind::Root && frame.key == "FileStates");
    }

    vector<Frame> frames_;
    FolderRewindFormat::MetadataState state_;
    bool rootStarted_ = false;
    bool rootEnded_ = false;
    bool sawFileStates_ = false;
};

class RecordSaxHandler final : public nlohmann::json_sax<nlohmann::json> {
public:
    using number_integer_t = nlohmann::json::number_integer_t;
    using number_unsigned_t = nlohmann::json::number_unsigned_t;
    using number_float_t = nlohmann::json::number_float_t;
    using string_t = nlohmann::json::string_t;

    explicit RecordSaxHandler(wstring fallbackArchiveFileName)
        : fallbackArchiveFileName_(std::move(fallbackArchiveFileName)) {}

    bool Complete(FolderRewindFormat::ChangeRecord& outRecord) {
        if (!rootEnded_ || !frames_.empty()
            || !IsSafeArchiveReference(record_.archiveFileName, false)
            || !IsSafeArchiveReference(record_.basedOnFullBackup, true)
            || !IsSafeArchiveReference(record_.previousBackupFileName, true)) {
            return false;
        }
        if (!fallbackArchiveFileName_.empty()
            && _wcsicmp(record_.archiveFileName.c_str(), fallbackArchiveFileName_.c_str()) != 0) {
            return false;
        }
        outRecord = std::move(record_);
        return true;
    }

    bool null() override { return Primitive(); }
    bool boolean(bool) override { return Primitive(); }
    bool number_integer(number_integer_t) override { return Primitive(); }
    bool number_unsigned(number_unsigned_t) override { return Primitive(); }
    bool number_float(number_float_t, const string_t&) override { return Primitive(); }
    bool string(string_t& value) override {
        if (frames_.empty()) return false;
        Frame& frame = frames_.back();
        if (frame.kind == Kind::KnownArray) {
            wstring normalized;
            if (!TryNormalizeRelativeEntry(utf8_to_wstring(value), normalized)) return false;
            Array(frame.arrayKind).push_back(std::move(normalized));
            return true;
        }
        if (frame.kind != Kind::Root) return true;
        if (IsArrayKey(frame.key)) return false;

        const wstring converted = utf8_to_wstring(value);
        if (frame.key == "ArchiveFileName") record_.archiveFileName = converted;
        else if (frame.key == "BackupType") record_.backupType = converted;
        else if (frame.key == "BasedOnFullBackup") record_.basedOnFullBackup = converted;
        else if (frame.key == "PreviousBackupFileName") record_.previousBackupFileName = converted;
        else if (frame.key == "CreatedAtUtc") record_.createdAtUtc = converted;
        return true;
    }
    bool start_object(size_t) override {
        if (frames_.empty()) {
            if (rootStarted_) return false;
            rootStarted_ = true;
            frames_.push_back({Kind::Root});
            return true;
        }
        const Frame& parent = frames_.back();
        if (parent.kind == Kind::KnownArray
            || (parent.kind == Kind::Root && IsArrayKey(parent.key))) {
            return false;
        }
        frames_.push_back({Kind::IgnoredObject});
        return true;
    }
    bool key(string_t& value) override {
        if (frames_.empty()) return false;
        Frame& frame = frames_.back();
        frame.key = value;
        if (frame.kind == Kind::Root) {
            if (value == "ArchiveFileName") record_.archiveFileName.clear();
            else if (value == "BackupType") record_.backupType.clear();
            else if (value == "BasedOnFullBackup") record_.basedOnFullBackup.clear();
            else if (value == "PreviousBackupFileName") record_.previousBackupFileName.clear();
            else if (value == "CreatedAtUtc") record_.createdAtUtc.clear();
        }
        return true;
    }
    bool end_object() override {
        if (frames_.empty()) return false;
        const Kind kind = frames_.back().kind;
        frames_.pop_back();
        if (kind == Kind::Root) {
            if (!frames_.empty()) return false;
            rootEnded_ = true;
        }
        return true;
    }
    bool start_array(size_t) override {
        if (frames_.empty()) return false;
        const Frame& parent = frames_.back();
        if (parent.kind == Kind::KnownArray) return false;
        if (parent.kind == Kind::Root) {
            ArrayKind arrayKind;
            if (TryGetArrayKind(parent.key, arrayKind)) {
                Array(arrayKind).clear();
                Frame frame{Kind::KnownArray};
                frame.arrayKind = arrayKind;
                frames_.push_back(std::move(frame));
                return true;
            }
        }
        frames_.push_back({Kind::IgnoredArray});
        return true;
    }
    bool end_array() override {
        if (frames_.empty()) return false;
        Frame frame = std::move(frames_.back());
        frames_.pop_back();
        if (frame.kind == Kind::KnownArray) {
            auto& values = Array(frame.arrayKind);
            sort(values.begin(), values.end());
            if (adjacent_find(values.begin(), values.end()) != values.end()) return false;
        }
        else if (frame.kind != Kind::IgnoredArray) {
            return false;
        }
        return true;
    }
    bool parse_error(size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }

private:
    enum class Kind { Root, KnownArray, IgnoredObject, IgnoredArray };
    enum class ArrayKind { Added, Modified, Deleted, Full };
    struct Frame {
        Kind kind;
        std::string key;
        ArrayKind arrayKind = ArrayKind::Added;
    };

    static bool TryGetArrayKind(const std::string& key, ArrayKind& outKind) {
        if (key == "AddedFiles") outKind = ArrayKind::Added;
        else if (key == "ModifiedFiles") outKind = ArrayKind::Modified;
        else if (key == "DeletedFiles") outKind = ArrayKind::Deleted;
        else if (key == "FullFileList") outKind = ArrayKind::Full;
        else return false;
        return true;
    }
    static bool IsArrayKey(const std::string& key) {
        ArrayKind ignored;
        return TryGetArrayKind(key, ignored);
    }
    vector<wstring>& Array(ArrayKind kind) {
        switch (kind) {
        case ArrayKind::Added: return record_.addedFiles;
        case ArrayKind::Modified: return record_.modifiedFiles;
        case ArrayKind::Deleted: return record_.deletedFiles;
        case ArrayKind::Full: return record_.fullFileList;
        }
        return record_.fullFileList;
    }
    bool Primitive() {
        if (frames_.empty()) return false;
        const Frame& frame = frames_.back();
        return frame.kind != Kind::KnownArray
            && !(frame.kind == Kind::Root && IsArrayKey(frame.key));
    }

    vector<Frame> frames_;
    FolderRewindFormat::ChangeRecord record_;
    wstring fallbackArchiveFileName_;
    bool rootStarted_ = false;
    bool rootEnded_ = false;
};

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

        StateSaxHandler handler;
        return nlohmann::json::sax_parse(in, &handler) && handler.Complete(outState);
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

        RecordSaxHandler handler(archiveFileName);
        return nlohmann::json::sax_parse(in, &handler) && handler.Complete(outRecord);
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

        if (!IsSafeArchiveReference(state.lastBackupFileName, true)
            || !IsSafeArchiveReference(state.basedOnFullBackup, true)) {
            return Failure(L"Could not serialize metadata state: unsafe archive reference.");
        }

        const filesystem::path statePath = GetStatePath(metadataDir);
        if (statePath.empty()) return Failure(L"Could not resolve metadata state path.");

        const auto write = AtomicFileWriter::WriteStreamed(statePath, [&](const AtomicFileWriter::ChunkSink& sink) {
            return WriteStateJson(sink, state);
        });
        if (!write.success) return Failure(write.error);
        return {true, {}};
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

        if (!IsSafeArchiveReference(record.archiveFileName, false)
            || !IsSafeArchiveReference(record.basedOnFullBackup, true)
            || !IsSafeArchiveReference(record.previousBackupFileName, true)) {
            return Failure(L"Could not serialize change record: unsafe metadata value.");
        }

        const filesystem::path recordsDirectory = GetRecordsDir(metadataDir);
        if (recordsDirectory.empty()) return Failure(L"Could not resolve metadata records directory.");
        error_code error;
        filesystem::create_directories(recordsDirectory, error);
        if (error) return Failure(SystemErrorText(L"Could not create metadata records directory", error));

        const auto write = AtomicFileWriter::WriteStreamed(*recordPath, [&](const AtomicFileWriter::ChunkSink& sink) {
            return WriteRecordJson(sink, record);
        });
        if (!write.success) return Failure(write.error);
        return {true, {}};
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
