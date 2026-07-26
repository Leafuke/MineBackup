#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

enum class InstanceRequestType {
    Activate,
    SelectConfig,
    RunSpecial
};

struct InstanceRequest {
    InstanceRequestType type = InstanceRequestType::Activate;
    std::wstring stableId;
};

enum class InstanceAcquireResult {
    Acquired,
    AlreadyRunning,
    Failed
};

class SingleInstanceService {
public:
    SingleInstanceService();
    ~SingleInstanceService();
    SingleInstanceService(const SingleInstanceService&) = delete;
    SingleInstanceService& operator=(const SingleInstanceService&) = delete;

    InstanceAcquireResult Acquire(
        const std::wstring& profileIdentity,
        const std::filesystem::path& runtimeRoot,
        std::wstring& error);
    bool Send(const InstanceRequest& request, std::wstring& error) const;
    std::vector<InstanceRequest> PollRequests(std::wstring& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
