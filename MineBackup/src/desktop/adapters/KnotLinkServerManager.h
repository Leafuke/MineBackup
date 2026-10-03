#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace minebackup::knotlink {

enum class KnotLinkServerState {
    Disabled,
    NotInstalled,
    Incompatible,
    Stopped,
    Starting,
    Ready,
    Failed
};

struct KnotLinkServerDiscovery {
    bool managedPlatform = false;
    bool installed = false;
    std::filesystem::path executablePath;
    std::string version;
    std::string detail;
};

struct KnotLinkServerStatus {
    KnotLinkServerState state = KnotLinkServerState::Disabled;
    bool managedPlatform = false;
    bool installed = false;
    bool processRunning = false;
    bool signalPortReady = false;
    bool responderPortReady = false;
    std::filesystem::path executablePath;
    std::string version;
    std::string message;
};

class IKnotLinkServerPlatform {
public:
    virtual ~IKnotLinkServerPlatform() = default;
    virtual KnotLinkServerDiscovery Discover() = 0;
    virtual bool IsProcessRunning() = 0;
    virtual bool IsPortReady(unsigned short port) = 0;
    virtual bool Start(const std::filesystem::path& executablePath) = 0;
    virtual bool WaitForReady(
        std::chrono::milliseconds timeout,
        unsigned short signalPort,
        unsigned short responderPort) = 0;
};

class KnotLinkServerManager {
public:
    static constexpr std::string_view MinimumVersion = "3.2.0.0";
    static constexpr unsigned short SignalPort = 6370;
    static constexpr unsigned short ResponderPort = 6378;

    explicit KnotLinkServerManager(
        std::shared_ptr<IKnotLinkServerPlatform> platform);

    KnotLinkServerStatus Refresh(bool integrationEnabled);
    KnotLinkServerStatus EnsureReady(
        bool integrationEnabled, bool autoStart);
    KnotLinkServerStatus StartCompatibleServer();
    KnotLinkServerStatus GetStatus() const;

    static bool IsVersionCompatible(std::string_view version);
    static const char* StateName(KnotLinkServerState state);

private:
    std::shared_ptr<IKnotLinkServerPlatform> platform_;
    mutable std::mutex mutex_;
    KnotLinkServerStatus status_;

    KnotLinkServerStatus RefreshLocked(bool integrationEnabled);
};

std::shared_ptr<IKnotLinkServerPlatform> CreateKnotLinkServerPlatform();
KnotLinkServerManager& GetKnotLinkServerManager();

} // namespace minebackup::knotlink
