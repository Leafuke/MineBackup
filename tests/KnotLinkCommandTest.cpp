#include "AppState.h"
#include "Console.h"
#include "KnotLinkProtocol.h"
#include "KnotLinkService.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

using namespace minebackup::knotlink;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (condition) {
        return;
    }
    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

KnotLinkKeyValuePayload ParseResponse(const std::string& response) {
    try {
        return KnotLinkKeyValueCodec::Parse(response);
    } catch (const std::exception& error) {
        Check(false, std::string("response should be valid v2: ") + error.what());
        return {};
    }
}

void CheckError(const std::string& response, const std::string& message) {
    const auto parsed = ParseResponse(response);
    const auto status = parsed.values.find("status");
    Check(status != parsed.values.end() && status->second == "error", message);
}

Config MakeConfig() {
    Config config;
    config.name = "Primary";
    config.configId = L"stable-config-id";
    config.saveRoot = L"C:\\Minecraft\\saves";
    config.backupPath = L"C:\\MineBackup\\backups";
    config.zipMethod = L"LZMA2";
    config.zipLevel = 5;
    config.worlds.push_back({L"World One", L"Test world"});
    return config;
}

void TestQueriesAndTargetResolution(KnotLinkService& service, Console& output) {
    g_appState.configs.clear();
    g_appState.configs.emplace(7, MakeConfig());

    for (const std::string identifier : {"stable-config-id", "Primary", "7"}) {
        const auto response = ParseResponse(service.HandlePayload(
            "cmd=LIST_FOLDERS;config_id=" +
                KnotLinkKeyValueCodec::EncodeValue(identifier),
            output));
        Check(response.values.find("status") != response.values.end() &&
                  response.values.at("status") == "ok",
              "config_id should resolve by stable ID, name, and numeric key");
        Check(response.values.at("data").find("World One") != std::string::npos,
              "folder query should return the configured world");
    }

    const auto ping = ParseResponse(
        service.HandlePayload("cmd=ping;future_extension=value", output));
    Check(ping.values.at("status") == "ok" &&
              ping.values.at("message") == "pong",
          "unknown extension keys should be ignored");
}

void TestMetadataAndUnsupportedParameters(
    KnotLinkService& service, Console& output) {
    CheckError(service.HandlePayload("cmd=BACKUP;config_id=7;folder=0", output),
               "BACKUP should require session metadata");

    const auto missingMetadata = ParseResponse(
        service.HandlePayload("cmd=BACKUP;config_id=7;folder=0", output));
    Check(!missingMetadata.values.contains("from") &&
              !missingMetadata.values.contains("request_id"),
          "missing metadata must not be fabricated");

    const auto unsupported = ParseResponse(service.HandlePayload(
        "cmd=BACKUP;from=test;request_id=req-1;config_id=7;folder=0;"
        "backup_scope=region",
        output));
    Check(unsupported.values.at("status") == "error" &&
              unsupported.values.at("from") == "test" &&
              unsupported.values.at("request_id") == "req-1" &&
              unsupported.values.at("code") == "unsupported_parameter",
          "unsupported features should return correlated structured errors");

    CheckError(service.HandlePayload(
                   "cmd=BACKUP;from=test;request_id=req-2;config_id=7;folder=0;"
                   "compression_method=zstd;compression_level=23",
                   output),
               "compression levels should use the FolderRewind ranges");

    CheckError(service.HandlePayload(
                   "cmd=BACKUP;from=test;request_id=req-3;config_id=7;folder=0;"
                   "preserve_player_data=true",
                   output),
               "player data preservation should be explicitly unsupported");
}

void TestLegacyCommandsHaveNoDispatch(
    KnotLinkService& service, Console& output) {
    for (const std::string payload : {
             "BACKUP 0 0",
             "SEND anything",
             "RESTORE_CURRENT backup.7z"}) {
        const auto response = ParseResponse(service.HandlePayload(payload, output));
        Check(response.values.at("status") == "error" &&
                  response.values.at("message").find("requires KnotLink v2") !=
                      std::string::npos,
              "legacy positional payload should only return the upgrade diagnostic");
    }

    for (const std::string command : {
             "SEND", "SET_CONFIG", "BACKUP_MODS", "SHUTDOWN_WORLD_SUCCESS",
             "RESTORE_CURRENT", "LIST_WORLDS"}) {
        const auto response = ParseResponse(service.HandlePayload(
            "cmd=" + command + ";from=test;request_id=legacy", output));
        Check(response.values.at("status") == "error" &&
                  response.values.at("code") == "unknown_command",
              command + " should not have a compatibility dispatch path");
    }
}

void TestStrictModVersion(KnotLinkService& service, Console& output) {
    auto response = ParseResponse(service.HandlePayload(
        "cmd=HANDSHAKE_RESPONSE;mod_version=3.1.0", output));
    Check(response.values.at("status") == "ok" &&
              response.values.at("compatible") == "true",
          "3.1.0 mod should be compatible");

    response = ParseResponse(service.HandlePayload(
        "cmd=HANDSHAKE_RESPONSE;mod_version=3.1", output));
    Check(response.values.at("status") == "ok" &&
              response.values.at("compatible") == "false",
          "non-semver mod version should be incompatible");

    response = ParseResponse(service.HandlePayload(
        "cmd=HANDSHAKE_RESPONSE;mod_version=3.1.0-extra", output));
    Check(response.values.at("compatible") == "false",
          "version suffixes should be rejected by strict parsing");
}

} // namespace

int main() {
    Console output;
    KnotLinkService service;
    TestQueriesAndTargetResolution(service, output);
    TestMetadataAndUnsupportedParameters(service, output);
    TestLegacyCommandsHaveNoDispatch(service, output);
    TestStrictModVersion(service, output);
    g_appState.configs.clear();

    if (failures == 0) {
        std::cout << "KnotLink command tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
