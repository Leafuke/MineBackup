#pragma once

#include "KnotLinkProtocol.h"

#include <string>
#include <string_view>

void BroadcastEvent(const std::string& eventPayload);
void BroadcastEvent(
    std::string_view eventName,
    const minebackup::knotlink::KnotLinkProtocolFormatter::Fields& fields);
bool InitKnotLink();
void CleanupKnotLink();

bool PerformModHandshake(
    const std::string& action,
    const std::string& worldName,
    int timeoutMs = 3000,
    const std::string& requestId = {});
