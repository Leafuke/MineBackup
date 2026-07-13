#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

enum class NetworkStatus {
    Succeeded,
    InvalidRequest,
    Cancelled,
    TimedOut,
    TooLarge,
    Truncated,
    RedirectLimit,
    InsecureRedirect,
    HttpError,
    TlsError,
    IoError,
    HashMismatch,
    BackendUnavailable,
    SinkRejected
};

struct NetworkRequest {
    std::string url;
    std::string userAgent = "MineBackup/1.16";
    std::chrono::milliseconds connectTimeout{8000};
    std::chrono::milliseconds totalTimeout{30000};
    int maximumRedirects = 5;
};

struct NetworkResult {
    NetworkStatus status = NetworkStatus::BackendUnavailable;
    long httpStatus = 0;
    std::string finalUrl;
    std::uint64_t transferredBytes = 0;
    std::wstring error;
};

using NetworkChunkSink = std::function<bool(const char*, std::size_t)>;

class NetworkBackend {
public:
    virtual ~NetworkBackend() = default;
    virtual NetworkResult Get(
        const NetworkRequest& request,
        const NetworkChunkSink& sink,
        std::stop_token stopToken) = 0;
};

struct NetworkTextResult : NetworkResult {
    std::string text;
};

struct NetworkDownloadResult : NetworkResult {
    std::filesystem::path path;
    std::string sha256;
};

class NetworkService {
public:
    static constexpr std::size_t MaximumTextBytes = 1u * 1024u * 1024u;
    static constexpr std::uint64_t MaximumDownloadBytes = 256ull * 1024ull * 1024ull;

    explicit NetworkService(std::shared_ptr<NetworkBackend> backend);

    NetworkTextResult GetText(const NetworkRequest& request, std::stop_token stopToken = {}) const;
    NetworkDownloadResult Download(
        const NetworkRequest& request,
        const std::filesystem::path& destination,
        const std::string& expectedSha256,
        std::stop_token stopToken = {}) const;

private:
    std::shared_ptr<NetworkBackend> backend_;
};
