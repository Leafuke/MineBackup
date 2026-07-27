#include "knotlink/OpenSocketQuerier.hpp"

#include "KnotLinkProtocol.h"
#include "KnotLinkServerManager.h"
#include "KnotLinkService.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main() {
    using namespace minebackup::knotlink;

    const auto server =
        GetKnotLinkServerManager().Refresh(true);
    if (server.state != KnotLinkServerState::Ready) {
        std::cout << "KnotLink live smoke skipped: "
                  << server.message << '\n';
        return 0;
    }

    KnotLinkService service;
    if (!service.Start()) {
        std::cerr << "[FAIL] compatible ready server rejected SDK startup\n";
        return 1;
    }

    try {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ::knotlink::OpenSocketQuerier querier;
        querier.setConfig(
            std::string(KnotLinkCapabilities::AppId),
            std::string(KnotLinkCapabilities::OpenSocketId));

        const auto ping = KnotLinkKeyValueCodec::Parse(
            querier.query_l("cmd=PING", 2000));
        if (ping.values.at("status") != "ok" ||
            ping.values.at("message") != "pong") {
            std::cerr << "[FAIL] live PING response was invalid\n";
            return 1;
        }

        const auto capabilities = KnotLinkKeyValueCodec::Parse(
            querier.query_l("cmd=GET_CAPABILITIES", 2000));
        if (capabilities.values.at("status") != "ok" ||
            capabilities.values.at("manifest_version") != "2.0.0" ||
            capabilities.values.at("func_list").find("\"appName\":\"MineBackup\"") ==
                std::string::npos) {
            std::cerr << "[FAIL] live capability response was invalid\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] live KnotLink query failed: "
                  << error.what() << '\n';
        return 1;
    }

    service.Stop();
    if (!service.Start()) {
        std::cerr << "[FAIL] KnotLink client did not restart cleanly\n";
        return 1;
    }
    service.Stop();
    std::cout << "KnotLink live smoke passed\n";
    return 0;
}
