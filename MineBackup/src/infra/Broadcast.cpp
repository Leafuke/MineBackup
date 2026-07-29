#include "Broadcast.h"

#include "AppState.h"
#include "Globals.h"
#include "KnotLinkServerManager.h"
#include "KnotLinkService.h"
#include "Logging.h"

#include <chrono>

void BroadcastEvent(const std::string& eventPayload) {
    minebackup::knotlink::GetKnotLinkService().BroadcastLegacyPayload(eventPayload);
}

void BroadcastEvent(
    std::string_view eventName,
    const minebackup::knotlink::KnotLinkProtocolFormatter::Fields& fields) {
    minebackup::knotlink::GetKnotLinkService().Broadcast(eventName, fields);
}

bool InitKnotLink() {
    const auto serverStatus =
        minebackup::knotlink::GetKnotLinkServerManager().EnsureReady(
            g_enableKnotLink, g_autoStartKnotLinkServer);
    if (serverStatus.state !=
        minebackup::knotlink::KnotLinkServerState::Ready) {
        MB_LOG_ERROR(minebackup::logging::LogCategory::KnotLink,
            "knotlink.connection.blocked",
            "Client connection blocked: {}", serverStatus.message);
        return false;
    }
    return minebackup::knotlink::GetKnotLinkService().Start();
}

void CleanupKnotLink() {
    minebackup::knotlink::GetKnotLinkService().Stop();
}

bool PerformModHandshake(
    const std::string& action,
    const std::string& worldName,
    int timeoutMs) {
    auto& mod = g_appState.knotLinkMod;
    mod.resetForOperation();
    mod.modDetected = false;
    mod.versionCompatible = false;
    mod.modVersion.clear();

    BroadcastEvent("handshake", {
        {"version", CURRENT_VERSION},
        {"action", action},
        {"world", worldName},
        {"min_mod_version", KnotLinkModInfo::MIN_MOD_VERSION}});

    const bool received = mod.waitForFlag(
        &KnotLinkModInfo::handshakeReceived,
        std::chrono::milliseconds(timeoutMs));
    if (!received) {
        mod.modDetected = false;
        mod.versionCompatible = false;
        return false;
    }
    return mod.modDetected.load() && mod.versionCompatible.load();
}
