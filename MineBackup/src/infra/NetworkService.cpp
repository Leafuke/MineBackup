#include "NetworkService.h"

#include "Sha256.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace std;

namespace {

atomic<unsigned long long> g_downloadCounter{0};

bool IsHttpsUrl(const string& url) {
    if (url.size() < 8) return false;
    string scheme = url.substr(0, 8);
    transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char value) {
        return static_cast<char>(tolower(value));
    });
    return scheme == "https://";
}

bool IsValidRequest(const NetworkRequest& request) {
    return IsHttpsUrl(request.url)
        && !request.userAgent.empty()
        && request.connectTimeout.count() > 0
        && request.totalTimeout >= request.connectTimeout
        && request.connectTimeout <= chrono::seconds(60)
        && request.totalTimeout <= chrono::minutes(10)
        && request.maximumRedirects >= 0
        && request.maximumRedirects <= 5;
}

bool IsLinkOrReparsePoint(const filesystem::path& path) {
    error_code error;
    if (!filesystem::exists(path, error) || error) return false;
    if (filesystem::is_symlink(filesystem::symlink_status(path, error))) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

bool HasLinkInExistingPath(const filesystem::path& path) {
    filesystem::path cursor;
    error_code error;
    const auto absolute = filesystem::absolute(path, error);
    if (error) return true;
    for (const auto& component : absolute) {
        cursor /= component;
        if (filesystem::exists(cursor, error) && !error && IsLinkOrReparsePoint(cursor)) return true;
        if (error) return true;
    }
    return false;
}

filesystem::path UniqueSibling(const filesystem::path& destination) {
#ifdef _WIN32
    const auto processId = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto processId = static_cast<unsigned long long>(getpid());
#endif
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto candidate = destination.parent_path() / (destination.filename().wstring() + L".download."
            + to_wstring(processId) + L"." + to_wstring(++g_downloadCounter));
        error_code error;
        if (!filesystem::exists(candidate, error) && !error) return candidate;
    }
    return {};
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

bool ReplaceFile(const filesystem::path& source, const filesystem::path& destination, error_code& error) {
#ifdef _WIN32
    if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error.clear();
        return true;
    }
    error = error_code(static_cast<int>(GetLastError()), system_category());
    return false;
#else
    filesystem::rename(source, destination, error);
    return !error;
#endif
}

string NormalizeHash(string value) {
    transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(tolower(character));
    });
    return value;
}

} // namespace

NetworkService::NetworkService(shared_ptr<NetworkBackend> backend) : backend_(std::move(backend)) {}

NetworkTextResult NetworkService::GetText(const NetworkRequest& request, stop_token stopToken) const {
    NetworkTextResult output;
    if (!backend_) return output;
    if (!IsValidRequest(request)) {
        output.status = NetworkStatus::InvalidRequest;
        output.error = L"Network text requests require HTTPS and bounded timeouts and redirects.";
        return output;
    }

    bool tooLarge = false;
    auto result = backend_->Get(request, [&](const char* data, size_t size) {
        if (size > MaximumTextBytes - output.text.size()) {
            tooLarge = true;
            return false;
        }
        output.text.append(data, size);
        return true;
    }, stopToken);
    static_cast<NetworkResult&>(output) = std::move(result);
    if (tooLarge) {
        output.status = NetworkStatus::TooLarge;
        output.error = L"The text response exceeded 1 MiB.";
        output.text.clear();
    }
    else if (output.status != NetworkStatus::Succeeded) {
        output.text.clear();
    }
    return output;
}

NetworkDownloadResult NetworkService::Download(
    const NetworkRequest& request,
    const filesystem::path& requestedDestination,
    const string& requestedExpectedSha256,
    stop_token stopToken) const {
    NetworkDownloadResult output;
    const string expectedSha256 = NormalizeHash(requestedExpectedSha256);
    if (!backend_) return output;
    if (!IsValidRequest(request) || requestedDestination.empty()
        || expectedSha256.size() != 64
        || !all_of(expectedSha256.begin(), expectedSha256.end(), [](unsigned char value) { return isxdigit(value) != 0; })) {
        output.status = NetworkStatus::InvalidRequest;
        output.error = L"Downloads require HTTPS, a destination, and a 64-character SHA-256.";
        return output;
    }

    error_code error;
    const auto destination = filesystem::absolute(requestedDestination, error).lexically_normal();
    if (error || HasLinkInExistingPath(destination)) {
        output.status = NetworkStatus::IoError;
        output.error = L"The download destination is invalid or traverses a link.";
        return output;
    }
    filesystem::create_directories(destination.parent_path(), error);
    if (error || HasLinkInExistingPath(destination)) {
        output.status = NetworkStatus::IoError;
        output.error = L"The download destination directory could not be prepared safely.";
        return output;
    }

    const auto staging = UniqueSibling(destination);
    if (staging.empty()) {
        output.status = NetworkStatus::IoError;
        output.error = L"Could not allocate a unique download staging file.";
        return output;
    }
    ofstream file(staging, ios::binary | ios::trunc);
    if (!file.is_open()) {
        output.status = NetworkStatus::IoError;
        output.error = L"Could not create the download staging file.";
        return output;
    }

    Sha256 hash;
    uint64_t received = 0;
    bool tooLarge = false;
    bool writeFailed = false;
    auto result = backend_->Get(request, [&](const char* data, size_t size) {
        if (size > MaximumDownloadBytes - received) {
            tooLarge = true;
            return false;
        }
        file.write(data, static_cast<streamsize>(size));
        if (!file) {
            writeFailed = true;
            return false;
        }
        hash.Update(data, size);
        received += size;
        return true;
    }, stopToken);
    file.flush();
    if (!file) writeFailed = true;
    file.close();
    static_cast<NetworkResult&>(output) = std::move(result);
    output.transferredBytes = received;

    if (tooLarge) {
        output.status = NetworkStatus::TooLarge;
        output.error = L"The download exceeded 256 MiB.";
    }
    else if (writeFailed) {
        output.status = NetworkStatus::IoError;
        output.error = L"The download staging file could not be written completely.";
    }
    else if (output.status == NetworkStatus::Succeeded) {
        output.sha256 = hash.FinalHex();
        if (output.sha256 != expectedSha256) {
            output.status = NetworkStatus::HashMismatch;
            output.error = L"The downloaded file did not match the expected SHA-256.";
        }
        else if (!SyncFile(staging)) {
            output.status = NetworkStatus::IoError;
            output.error = L"The downloaded file could not be synchronized.";
        }
        else if (!ReplaceFile(staging, destination, error)) {
            output.status = NetworkStatus::IoError;
            output.error = L"The verified download could not be committed atomically.";
        }
        else if (!SyncDirectory(destination.parent_path())) {
            output.status = NetworkStatus::IoError;
            output.error = L"The download was committed but its directory could not be synchronized.";
        }
        else {
            output.path = destination;
        }
    }

    if (output.status != NetworkStatus::Succeeded) {
        error_code ignored;
        filesystem::remove(staging, ignored);
    }
    return output;
}
