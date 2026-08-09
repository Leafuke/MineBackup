#pragma once

#include "KnotLinkProtocol.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace minebackup::knotlink {

// Shared protocol ingress for desktop and headless runtimes. It owns command
// parsing, metadata validation, unsupported-parameter rejection, and error
// formatting; the injected handler owns runtime-specific execution.
class KnotLinkCommandDispatcher {
public:
    using Handler = std::function<std::string(
        const std::shared_ptr<KnotLinkCommandContext>&)>;

    explicit KnotLinkCommandDispatcher(Handler handler = {});

    void SetHandler(Handler handler);
    std::string Dispatch(std::string_view payload) const;

private:
    mutable std::mutex mutex_;
    Handler handler_;
};

} // namespace minebackup::knotlink
