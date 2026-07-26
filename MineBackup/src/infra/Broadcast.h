#pragma once

#include "KnotLinkProtocol.h"

#include <string>
#include <string_view>

struct Console;

void BroadcastEvent(const std::string& eventPayload);
void BroadcastEvent(
    std::string_view eventName,
    const minebackup::knotlink::KnotLinkProtocolFormatter::Fields& fields);
bool InitKnotLink(Console& console);
void CleanupKnotLink();

bool PerformModHandshake(
    const std::string& action,
    const std::string& worldName,
    int timeoutMs = 3000);
