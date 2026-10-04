#pragma once

#include "KnotLinkProtocol.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace minebackup::knotlink {

class KnotLinkService {
public:
    KnotLinkService();
    ~KnotLinkService();

    KnotLinkService(const KnotLinkService&) = delete;
    KnotLinkService& operator=(const KnotLinkService&) = delete;

    bool Start();
    void Stop();
    bool IsRunning() const noexcept;

    std::string HandlePayload(std::string_view payload);
    void Broadcast(
        std::string_view eventName,
        const KnotLinkProtocolFormatter::Fields& fields = {},
        const std::shared_ptr<KnotLinkCommandContext>& context = {});
    void BroadcastLegacyPayload(std::string_view payload);

    static std::shared_ptr<KnotLinkCommandContext> CurrentCommandContext();
    // Legacy integrated callbacks have no world identity and use fresh UUIDs.
    // Hold across handshake, mutation and terminal event, including GUI callers.
    static std::recursive_mutex& ModConversationMutex();

private:
    class ContextScope;
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    std::atomic<bool> running_{false};
    mutable std::mutex lifecycleMutex_;

    std::string HandleRequest(
        const std::shared_ptr<KnotLinkCommandContext>& context);
};

KnotLinkService& GetKnotLinkService();

} // namespace minebackup::knotlink
