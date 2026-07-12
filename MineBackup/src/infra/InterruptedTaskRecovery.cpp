#include "InterruptedTaskRecovery.h"

#include "AtomicFileWriter.h"
#include "json.hpp"
#include "text_to_text.h"

#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;

namespace {

bool HasRecoverablePrefix(const wstring& name) {
    static constexpr const wchar_t* prefixes[] = {
        L"MineBackup_Filelist_",
        L"MineBackup_DeleteOnly_",
        L"MineBackup_Merge_",
        L"MineBackup_Export",
        L"MineBackup_CoreValidation",
        L"MineBackup_cloud_"
    };
    for (const auto* prefix : prefixes) {
        if (name.starts_with(prefix)) return true;
    }
    return false;
}

bool IsLinkOrReparsePoint(const filesystem::path& path) {
    error_code error;
    if (filesystem::is_symlink(filesystem::symlink_status(path, error))) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

bool HasLinkOrReparsePointInExistingPath(const filesystem::path& path) {
    filesystem::path cursor;
    for (const auto& component : filesystem::absolute(path)) {
        cursor /= component;
        error_code error;
        if (filesystem::exists(cursor, error) && !error && IsLinkOrReparsePoint(cursor)) return true;
        if (error) return true;
    }
    return false;
}

bool TryCalculateBytes(const filesystem::path& path, uintmax_t& bytes) {
    error_code error;
    bytes = 0;
    if (filesystem::is_regular_file(path, error)) {
        bytes = filesystem::file_size(path, error);
        return !error;
    }
    filesystem::recursive_directory_iterator iterator(
        path, filesystem::directory_options::skip_permission_denied, error);
    const filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (IsLinkOrReparsePoint(iterator->path())) {
            return false;
        }
        else if (iterator->is_regular_file(error)) {
            bytes += iterator->file_size(error);
        }
        iterator.increment(error);
    }
    return !error;
}

} // namespace

InterruptedTaskRecoveryReport RecoverInterruptedTaskArtifacts(
    const filesystem::path& runtimeRoot, const filesystem::path& requestedReportPath) {
    InterruptedTaskRecoveryReport report;
    error_code error;
    if (!filesystem::is_directory(runtimeRoot, error) || error) return report;
    if (HasLinkOrReparsePointInExistingPath(runtimeRoot)) {
        report.errors.push_back(L"Refused to recover interrupted tasks through a linked runtime path.");
        return report;
    }

    vector<filesystem::path> candidates;
    filesystem::directory_iterator iterator(runtimeRoot, filesystem::directory_options::skip_permission_denied, error);
    const filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const auto path = iterator->path();
        if (HasRecoverablePrefix(path.filename().wstring())) candidates.push_back(path);
        iterator.increment(error);
    }
    if (error) report.errors.push_back(L"Could not finish scanning the runtime directory.");

    for (const auto& path : candidates) {
        if (IsLinkOrReparsePoint(path)) {
            report.errors.push_back(L"Refused linked interrupted-task artifact: " + path.wstring());
        }
        else {
            uintmax_t bytes = 0;
            if (!TryCalculateBytes(path, bytes)) {
                report.errors.push_back(L"Refused unsafe or unreadable interrupted-task artifact: " + path.wstring());
            }
            else {
                error_code removeError;
                filesystem::remove_all(path, removeError);
                if (removeError) {
                    report.errors.push_back(L"Could not remove interrupted-task artifact: " + path.wstring());
                }
                else {
                    report.removedPaths.push_back(path);
                    report.removedBytes += bytes;
                }
            }
        }
    }
    if (report.removedPaths.empty() && report.errors.empty()) return report;

    nlohmann::json document;
    document["SchemaVersion"] = 1;
    document["RecoveredAtUnix"] = chrono::duration_cast<chrono::seconds>(
        chrono::system_clock::now().time_since_epoch()).count();
    document["RemovedBytes"] = report.removedBytes;
    document["RemovedPaths"] = nlohmann::json::array();
    for (const auto& path : report.removedPaths) {
        document["RemovedPaths"].push_back(wstring_to_utf8(path.wstring()));
    }
    document["Errors"] = nlohmann::json::array();
    for (const auto& message : report.errors) document["Errors"].push_back(wstring_to_utf8(message));

    AtomicFileWriter::WriteOptions options;
    options.keepBackup = false;
    const auto write = AtomicFileWriter::WriteText(requestedReportPath, document.dump(2), options);
    if (write.success) report.reportPath = requestedReportPath;
    else report.errors.push_back(L"Could not persist the interrupted-task recovery report: " + write.error);
    return report;
}
