#pragma once

#include "KnotLinkProtocol.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

struct Console;

namespace minebackup::knotlink {

class KnotLinkService {
public:
    KnotLinkService();
    ~KnotLinkService();

    KnotLinkService(const KnotLinkService&) = delete;
    KnotLinkService& operator=(const KnotLinkService&) = delete;

    bool Start(Console& console);
    void Stop();
    bool IsRunning() const noexcept;

    std::string HandlePayload(std::string_view payload, Console& console);
    void Broadcast(
        std::string_view eventName,
        const KnotLinkProtocolFormatter::Fields& fields = {},
        const std::shared_ptr<KnotLinkCommandContext>& context = {});
    void BroadcastLegacyPayload(std::string_view payload);

    static std::shared_ptr<KnotLinkCommandContext> CurrentCommandContext();

private:
    class ContextScope;
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    std::atomic<bool> running_{false};
    mutable std::mutex lifecycleMutex_;

    std::string HandleRequest(
        const std::shared_ptr<KnotLinkCommandContext>& context,
        Console& console);
};

KnotLinkService& GetKnotLinkService();

} // namespace minebackup::knotlink
