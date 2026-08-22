#include "AtomicFileWriter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <fstream>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>

#include "text_to_text.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace std;

namespace AtomicFileWriter {
namespace {

atomic<unsigned long long> g_counter{0};
mutex g_writeMutex;

wstring ErrorText(
    const wchar_t* operation,
    const error_code& error = {},
    size_t attempts = 0) {
    wstring text(operation);
    if (error) {
        text += L" (";
#ifdef _WIN32
        text += L"win32=";
#else
        text += L"native=";
#endif
        text += to_wstring(error.value());
        if (attempts > 0) text += L", attempts=" + to_wstring(attempts);
        text += L"): ";
        text += utf8_to_wstring(error.message());
    }
    return text;
}

bool IsLinkOrReparsePoint(const filesystem::path& path) {
    error_code error;
    if (!filesystem::exists(path, error) || error) return false;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return filesystem::is_symlink(filesystem::symlink_status(path, error));
#endif
}

bool HasLinkOrReparsePointInExistingPath(const filesystem::path& path) {
    filesystem::path cursor;
    for (const auto& component : path) {
        cursor /= component;
        error_code error;
        if (!filesystem::exists(cursor, error)) {
            if (error) return true;
            continue;
        }
        if (IsLinkOrReparsePoint(cursor)) return true;
    }
    return false;
}

bool SyncFile(const filesystem::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool success = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    return success;
#else
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) return false;
    const bool success = fsync(descriptor) == 0;
    close(descriptor);
    return success;
#endif
}

bool SyncDirectory(const filesystem::path& path) {
#ifdef _WIN32
    (void)path;
    return true;
#else
    const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) return false;
    const bool success = fsync(descriptor) == 0;
    close(descriptor);
    return success;
#endif
}

struct ReplaceResult {
    bool success = false;
    error_code error;
    size_t attempts = 0;
};

#ifdef _WIN32
bool IsSharingViolation(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}

bool TargetHasIncompatibleDeleteSharing(const filesystem::path& target) {
    HANDLE probe = CreateFileW(
        target.c_str(),
        DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        return false;
    }
    return IsSharingViolation(GetLastError());
}

bool IsTransientReplaceError(DWORD error, const filesystem::path& target) {
    if (IsSharingViolation(error)) return true;
    // MoveFileExW reports ERROR_ACCESS_DENIED for some handles that do not
    // share DELETE. Confirm that specific sharing condition before retrying;
    // ordinary permission and read-only failures remain non-transient.
    return error == ERROR_ACCESS_DENIED && TargetHasIncompatibleDeleteSharing(target);
}
#endif

ReplaceResult Replace(
    const filesystem::path& source,
    const filesystem::path& target,
    const function<void(const error_code&, size_t)>& failureObserver = {}) {
    ReplaceResult result;
#ifdef _WIN32
    constexpr array<DWORD, 6> retryDelaysMs{5, 10, 20, 40, 80, 160};
    for (size_t attempt = 0;; ++attempt) {
        result.attempts = attempt + 1;
        if (MoveFileExW(source.c_str(), target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            result.success = true;
            return result;
        }

        const DWORD nativeError = GetLastError();
        result.error = error_code(static_cast<int>(nativeError), system_category());
        const bool transient = IsTransientReplaceError(nativeError, target);
        if (failureObserver) failureObserver(result.error, result.attempts);
        if (!transient || attempt >= retryDelaysMs.size()) {
            return result;
        }
        this_thread::sleep_for(chrono::milliseconds(retryDelaysMs[attempt]));
    }
#else
    ++result.attempts;
    filesystem::rename(source, target, result.error);
    if (result.error && failureObserver) failureObserver(result.error, result.attempts);
    result.success = !result.error;
    return result;
#endif
}

filesystem::path UniqueSibling(const filesystem::path& target, const wchar_t* suffix) {
#ifdef _WIN32
    const auto process = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto process = static_cast<unsigned long long>(getpid());
#endif
    return target.parent_path() /
        (target.filename().wstring() + suffix + L"." + to_wstring(process) + L"." + to_wstring(++g_counter));
}

void RemoveQuietly(const filesystem::path& path);

bool WriteExclusiveTemporary(
    const filesystem::path& target, const string& content, filesystem::path& temporary, wstring& errorText) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        temporary = UniqueSibling(target, L".tmp");
#ifdef _WIN32
        HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) continue;
            errorText = L"Could not exclusively create the atomic write temporary file.";
            return false;
        }
        bool success = true;
        size_t offset = 0;
        while (offset < content.size()) {
            const size_t remaining = content.size() - offset;
            const size_t maximumChunk = static_cast<size_t>((numeric_limits<DWORD>::max)());
            const DWORD chunk = static_cast<DWORD>(remaining < maximumChunk ? remaining : maximumChunk);
            DWORD written = 0;
            if (!WriteFile(handle, content.data() + offset, chunk, &written, nullptr) || written != chunk) {
                success = false;
                break;
            }
            offset += written;
        }
        if (success) success = FlushFileBuffers(handle) != FALSE;
        CloseHandle(handle);
#else
        const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            errorText = L"Could not exclusively create the atomic write temporary file.";
            return false;
        }
        bool success = true;
        size_t offset = 0;
        while (offset < content.size()) {
            const ssize_t written = write(descriptor, content.data() + offset, content.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                success = false;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (success) success = fsync(descriptor) == 0;
        close(descriptor);
#endif
        if (success) return true;
        RemoveQuietly(temporary);
        errorText = L"Could not write and synchronize the complete atomic temporary file.";
        return false;
    }
    errorText = L"Could not allocate a unique atomic write temporary file.";
    return false;
}

bool ReadExactly(const filesystem::path& path, const string& expected) {
    ifstream input(path, ios::binary);
    if (!input.is_open()) return false;
    string actual((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
    return (input.good() || input.eof()) && actual == expected;
}

void RemoveQuietly(const filesystem::path& path) {
    error_code ignored;
    filesystem::remove(path, ignored);
}

} // namespace

WriteResult WriteText(const filesystem::path& requestedTarget, const string& content, const WriteOptions& options) {
    lock_guard<mutex> writeLock(g_writeMutex);
    WriteResult result;
    if (requestedTarget.empty()) {
        result.error = L"The atomic write target is empty.";
        return result;
    }

    error_code error;
    filesystem::path target = filesystem::absolute(requestedTarget, error).lexically_normal();
    if (error) {
        result.error = ErrorText(L"Could not resolve the atomic write target", error);
        return result;
    }

    const filesystem::path parent = target.parent_path();
    if (HasLinkOrReparsePointInExistingPath(target)) {
        result.error = L"Atomic writes do not follow a symlink or reparse-point target.";
        return result;
    }
    if (options.createParentDirectories) {
        filesystem::create_directories(parent, error);
        if (error) {
            result.error = ErrorText(L"Could not create the atomic write directory", error);
            return result;
        }
    }
    if (HasLinkOrReparsePointInExistingPath(target)) {
        result.error = L"Atomic writes do not follow a symlink or reparse-point target.";
        return result;
    }

    filesystem::path temporary;
    if (!WriteExclusiveTemporary(target, content, temporary, result.error)) {
        return result;
    }

    if (!ReadExactly(temporary, content)) {
        result.error = L"The atomic temporary file failed read-back verification.";
        RemoveQuietly(temporary);
        return result;
    }

    const bool targetExists = filesystem::exists(target, error) && !error;
    if (error) {
        result.error = ErrorText(L"Could not inspect the atomic write target", error);
        RemoveQuietly(temporary);
        return result;
    }

    if (options.keepBackup && targetExists) {
        result.backupPath = target.wstring() + L".bak";
        const filesystem::path backupTemporary = UniqueSibling(result.backupPath, L".tmp");
        filesystem::copy_file(target, backupTemporary, filesystem::copy_options::overwrite_existing, error);
        if (error || !SyncFile(backupTemporary)) {
            result.error = ErrorText(L"Could not preserve the previous file as a backup", error);
            RemoveQuietly(backupTemporary);
            RemoveQuietly(temporary);
            return result;
        }
        const auto backupReplacement = Replace(backupTemporary, result.backupPath);
        if (!backupReplacement.success) {
            result.error = ErrorText(
                L"Could not commit the previous-file backup",
                backupReplacement.error,
                backupReplacement.attempts);
            RemoveQuietly(backupTemporary);
            RemoveQuietly(temporary);
            return result;
        }
    }

    const auto replacement = Replace(temporary, target, options.replaceFailureObserver);
    if (!replacement.success) {
        result.error = ErrorText(
            L"Could not atomically replace the target",
            replacement.error,
            replacement.attempts);
        RemoveQuietly(temporary);
        // commitState 保持 NotReplaced：target 仍是旧内容。
        return result;
    }
    // replace 已成功：commit point 已越过，target 从此是新内容，
    // 任何后续失败都不允许上层按“未发生”处理。
    result.commitState = WriteCommitState::ReplacedNotDurable;

    // 测试注入的 directorySyncOverride 仅用于确定性地模拟目录同步失败；
    // 生产路径下为空，调用真正的 SyncDirectory。
    const bool directorySynced = options.directorySyncOverride
        ? options.directorySyncOverride(parent)
        : SyncDirectory(parent);
    if (!directorySynced) {
        result.error = L"The target was replaced but its parent directory could not be synchronized.";
        // success 仍为 false，但 commitState = ReplacedNotDurable 让上层
        // 能够区分“replace 未发生”与“已替换但持久化未确认”。
        return result;
    }

    result.commitState = WriteCommitState::Durable;
    result.success = true;
    return result;
}

} // namespace AtomicFileWriter
