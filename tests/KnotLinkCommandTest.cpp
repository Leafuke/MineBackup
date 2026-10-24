#include "AppState.h"
#include "AppPaths.h"
#include "HistoryManager.h"
#include "MigrationCoordinator.h"
#include "KnotLinkProtocol.h"
#include "KnotLinkService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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
    config.name = "Primary 示例";
    config.configId = L"stable-config-id";
    config.saveRoot = L"C:\\Minecraft\\saves";
    config.backupPath = L"C:\\MineBackup\\backups";
    config.zipMethod = L"LZMA2";
    config.zipLevel = 5;
    config.worlds.push_back({L"World One", L"Test world"});
    config.worlds.push_back({L"Nether 世界", L"Second test world"});
    return config;
}

void TestQueriesAndTargetResolution(KnotLinkService& service) {
    g_appState.configuration.Write().Configs().clear();
    g_appState.configuration.Write().Configs().emplace(7, MakeConfig());
    Config secondConfig = MakeConfig();
    secondConfig.configId = L"second-config-id";
    secondConfig.name = "Second";
    g_appState.configuration.Write().Configs().emplace(8, std::move(secondConfig));

    const std::string listConfigs = service.HandlePayload(
        "cmd=LIST_CONFIGS");
    Check(listConfigs ==
              "status=ok;data=stable-config-id%2CPrimary%20"
              "%E7%A4%BA%E4%BE%8B%3Bsecond-config-id%2CSecond",
          "config query should use the FolderRewind id,name wire format");

    for (const std::string identifier : {
             "stable-config-id", "Primary 示例", "7"}) {
        const auto response = ParseResponse(service.HandlePayload(
            "cmd=LIST_FOLDERS;config_id=" +
                KnotLinkKeyValueCodec::EncodeValue(identifier)));
        Check(response.values.find("status") != response.values.end() &&
                  response.values.at("status") == "ok",
              "config_id should resolve by stable ID, name, and numeric key");
        Check(response.values.at("data") == "World One;Nether 世界",
              "folder query should use the FolderRewind semicolon list format");
    }

    Check(service.HandlePayload(
              "cmd=LIST_FOLDERS;config_id=stable-config-id") ==
              "status=ok;data=World%20One%3BNether%20"
              "%E4%B8%96%E7%95%8C",
          "folder names should be encoded once as the outer data scalar");

    Check(service.HandlePayload(
              "cmd=GET_CONFIG;config_id=stable-config-id") ==
              "status=ok;data=name%3DPrimary%20%E7%A4%BA%E4%BE%8B"
              "%3Bbackup_mode%3DFull%3Bformat%3D7z%3Bkeep_count%3D0",
          "config details should use the FolderRewind nested key-value format");

    const auto backupRoot =
        std::filesystem::temp_directory_path() /
        ("minebackup-knotlink-command-test-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));
    std::error_code cleanupError;
    std::filesystem::remove_all(backupRoot, cleanupError);
    const auto backupDirectory = backupRoot / "World One";
    std::filesystem::create_directories(backupDirectory);
    std::ofstream(backupDirectory / "[Full] World One.7z").put('x');
    std::ofstream(backupDirectory / "[Smart] World One.zip").put('x');
    std::ofstream(backupDirectory / "ignore.txt").put('x');
    g_appState.configuration.Write().Configs().at(7).backupPath = backupRoot.wstring();
    const auto backups = ParseResponse(service.HandlePayload(
        "cmd=LIST_BACKUPS;config_id=stable-config-id;folder=0"));
    Check(backups.values.at("data") ==
              "[Full] World One.7z;[Smart] World One.zip" ||
              backups.values.at("data") ==
                  "[Smart] World One.zip;[Full] World One.7z",
          "backup query should return separate archive completion candidates");
    Check(backups.values.at("data").find("ignore.txt") == std::string::npos,
          "backup query should exclude non-archive files");
    std::filesystem::remove_all(backupRoot, cleanupError);

    const auto capabilities = ParseResponse(
        service.HandlePayload("cmd=GET_CAPABILITIES"));
    Check(capabilities.values.at("encoding") == "percent",
          "capability metadata should describe percent-encoded values");

    const auto status = ParseResponse(
        service.HandlePayload("cmd=GET_STATUS"));
    Check(status.values.at("data").starts_with("enabled=") &&
              status.values.at("data").find(";initialized=") !=
                  std::string::npos &&
              status.values.at("data").find(";active_tasks=") !=
                  std::string::npos &&
              !status.values.at("data").starts_with("{"),
          "status query should use the FolderRewind nested key-value format");

    const auto ping = ParseResponse(
        service.HandlePayload("cmd=ping;future_extension=value"));
    Check(ping.values.at("status") == "ok" &&
              ping.values.at("message") == "pong",
          "unknown extension keys should be ignored");
}

void TestMetadataAndUnsupportedParameters(
    KnotLinkService& service) {
    CheckError(service.HandlePayload("cmd=BACKUP;config_id=7;folder=0"),
               "BACKUP should require session metadata");

    const auto missingMetadata = ParseResponse(
        service.HandlePayload("cmd=BACKUP;config_id=7;folder=0"));
    Check(!missingMetadata.values.contains("from") &&
              !missingMetadata.values.contains("request_id"),
          "missing metadata must not be fabricated");

    const auto unsupported = ParseResponse(service.HandlePayload(
        "cmd=BACKUP;from=test;request_id=req-1;config_id=7;folder=0;"
        "scope_unimplemented=region"));
    Check(unsupported.values.at("status") == "error" &&
              unsupported.values.at("from") == "test" &&
              unsupported.values.at("request_id") == "req-1" &&
              unsupported.values.at("code") == "unsupported_parameter",
          "unsupported features should return correlated structured errors");

    CheckError(service.HandlePayload(
                   "cmd=BACKUP;from=test;request_id=req-2;config_id=7;folder=0;"
                   "compression_method=zstd;compression_level=23"),
               "compression levels should use the FolderRewind ranges");

    CheckError(service.HandlePayload(
                   "cmd=BACKUP;from=test;request_id=req-3;config_id=7;folder=0;"
                   "preserve_player_data=true"),
               "restore-only preservation must not be silently accepted on backup");
    CheckError(service.HandlePayload(
        "cmd=RESTORE;from=test;request_id=bad-bool;preserve_player_data=maybe"),
        "invalid preservation booleans must fail closed");
    CheckError(service.HandlePayload(
        "cmd=RESTORE;from=test;request_id=typo;restore_preserve_path=level.dat"),
        "misspelled preservation options must fail closed");
    CheckError(service.HandlePayload(
        "cmd=RESTORE;from=test;request_id=wrong-command;backup_whitelist=region"),
        "backup selection cannot be silently ignored on restore");
    for (const std::string payload : {
        "cmd=BACKUP;from=test;request_id=protect-bool;protect=maybe",
        "cmd=BACKUP_ALL;from=test;request_id=protect-all;protect=true",
        "cmd=RESTORE;from=test;request_id=protect-restore;protect=true",
        "cmd=BACKUP;from=test;request_id=protect-partial;protect=true;backup_whitelist=region",
        "cmd=BACKUP;from=test;request_id=protect-scope;protect=true;backup_scope=selected-regions"}) {
        const auto denied = ParseResponse(service.HandlePayload(payload));
        Check(denied.values.at("status") == "error"
            && denied.values.at("code") == "unsupported_parameter"
            && denied.values.at("from") == "test",
            "invalid, misplaced, or partial protection is rejected before command submission");
    }
    CheckError(service.HandlePayload(
        "cmd=BACKUP_ALL;from=test;request_id=wrong-selector;current_save=true"),
        "current_save cannot be silently ignored by backup-all");
    const auto invalidList = ParseResponse(service.HandlePayload(
        "cmd=RESTORE;from=test;request_id=bad-list;config_id=7;folder=0;file=archive.7z;"
        "restore_preserve_paths=a,,b"));
    Check(invalidList.values.at("status") == "error"
        && invalidList.values.at("from") == "test"
        && invalidList.values.at("request_id") == "bad-list",
        "validation failures after parsing retain the command's correlation metadata");
}

void TestImportanceCommands(KnotLinkService& service) {
    const auto root = std::filesystem::temp_directory_path() /
        ("minebackup-importance-command-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    AppPaths paths;
    AppPathRequest pathRequest;
    pathRequest.dataDirectory = root / "profile";
    std::wstring pathError;
    Check(ResolveAppPaths(pathRequest, root / "MineBackup", paths, pathError), "importance test profile paths resolve");
    std::filesystem::create_directories(paths.dataRoot);
    SetCurrentAppPaths(paths);
    Config config = MakeConfig();
    config.saveRoot = (root / "worlds").wstring();
    config.backupPath = (root / "archives").wstring();
    config.worlds = {{L"world", L""}};
    g_appState.configuration.Write().Configs() = {{7, config}};
    std::filesystem::create_directories(root / "archives" / "world");
    std::ofstream(root / "archives" / "world" / "archive space.7z").put('x');
    HistoryEntry entry;
    entry.configId = config.configId;
    entry.worldName = L"world";
    entry.worldPath = (root / "worlds" / "world").wstring();
    entry.backupFile = L"archive space.7z";
    entry.backupType = L"Partial";
    entry.timestamp_str = L"2026-10-04T00:00:00";
    Check(GetHistoryRepository().ReplaceAll({{config.configId, {entry}}},
        paths.HistoryFile(), {{7, config}}, true), "importance fixture persists");
    const std::string target = ";from=test;request_id=pin-1;config_id=stable-config-id;folder=0";
    const std::string file = ";file=archive%20space.7z";
    const auto query = [&] { return ParseResponse(service.HandlePayload("cmd=GET_IMPORTANCE" + target + file)); };
    const auto revision = GetHistoryRepository().Snapshot()->revision;
    auto response = query();
    Check(response.values.at("status") == "ok" && response.values.at("file") == "archive space.7z"
        && response.values.at("important") == "false"
        && response.values.at("from") == "test" && response.values.at("request_id") == "pin-1",
        "importance query returns the exact local archive, boolean, and caller metadata");
    Check(GetHistoryRepository().Snapshot()->revision == revision,
        "importance query must not mutate history");
    MigrationCoordinator::SetHistoryPersistenceBlocked(true);
    CheckError(service.HandlePayload("cmd=MARK_IMPORTANT" + target + file + ";important=true"),
        "desktop migration gate blocks a protected history mutation");
    CheckError(service.HandlePayload("cmd=GET_IMPORTANCE" + target + file),
        "migration-blocked query cannot confirm unsaved in-memory history");
    MigrationCoordinator::SetHistoryPersistenceBlocked(false);
    for (int attempt = 0; attempt < 2; ++attempt) {
        response = ParseResponse(service.HandlePayload("cmd=MARK_IMPORTANT" + target + file + ";important=true"));
        Check(response.values.at("status") == "ok" && response.values.at("file") == "archive space.7z"
            && response.values.at("important") == "true",
            "pin is idempotent and synchronously confirms persisted importance");
    }
    Check(GetHistoryRepository().Load(paths.HistoryFile(), {{7, config}})
        && query().values.at("important") == "true", "pin survives history reload");
    response = ParseResponse(service.HandlePayload("cmd=MARK_IMPORTANT" + target + file + ";important=false"));
    Check(response.values.at("status") == "ok" && response.values.at("important") == "false"
        && query().values.at("important") == "false", "unpin returns and persists false");
    for (const auto& payload : {
        "cmd=GET_IMPORTANCE" + target,
        "cmd=GET_IMPORTANCE" + target + ";file=missing.7z",
        "cmd=GET_IMPORTANCE" + target + ";file=..%2Farchive%20space.7z",
        "cmd=MARK_IMPORTANT" + target + file,
        "cmd=MARK_IMPORTANT" + target + file + ";important=maybe"}) {
        CheckError(service.HandlePayload(payload), "invalid exact pin/query requests fail closed");
    }
    for (const std::string command : {"MARK_IMPORTANT", "GET_IMPORTANCE"}) {
        response = ParseResponse(service.HandlePayload("cmd=" + command
            + ";from=test;request_id=current-save;current_save=true" + file
            + (command == "MARK_IMPORTANT" ? ";important=true" : "")));
        Check(response.values.at("status") == "error"
            && response.values.at("message") == "No active world was found.",
            "current_save is accepted by importance commands and fails safely without an active world");
    }
    AppPaths blocked = paths;
    blocked.dataRoot = root / "blocked";
    std::filesystem::create_directories(blocked.HistoryFile());
    SetCurrentAppPaths(blocked);
    CheckError(service.HandlePayload("cmd=MARK_IMPORTANT" + target + file + ";important=true"),
        "history write failure must not acknowledge a successful pin");
    Check(query().values.at("important") == "false", "failed pin keeps the prior published flag");
    SetCurrentAppPaths(paths);
    std::filesystem::remove(root / "archives" / "world" / "archive space.7z");
    CheckError(service.HandlePayload("cmd=GET_IMPORTANCE" + target + file),
        "missing local archive must not be returned as an unprotected backup");
    GetHistoryRepository().ReplaceAll({}, paths.HistoryFile(), {{7, config}}, false);
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

void TestLegacyCommandsHaveNoDispatch(
    KnotLinkService& service) {
    for (const std::string payload : {
             "BACKUP 0 0",
             "SEND anything",
             "RESTORE_CURRENT backup.7z"}) {
        const auto response = ParseResponse(service.HandlePayload(payload));
        Check(response.values.at("status") == "error" &&
                  response.values.at("message").find("requires KnotLink v2") !=
                      std::string::npos,
              "legacy positional payload should only return the upgrade diagnostic");
    }

    for (const std::string command : {
             "SEND", "SET_CONFIG", "BACKUP_MODS", "SHUTDOWN_WORLD_SUCCESS",
             "RESTORE_CURRENT", "LIST_WORLDS",
             "AUTO_BACKUP", "STOP_AUTO_BACKUP"}) {
        const auto response = ParseResponse(service.HandlePayload(
            "cmd=" + command + ";from=test;request_id=legacy"));
        Check(response.values.at("status") == "error" &&
                  response.values.at("code") == "unknown_command",
              command + " should not have a compatibility dispatch path");
    }
}

void TestStrictModVersion(KnotLinkService& service) {
    service.Broadcast("handshake", {{"world", "test-world"}});
    auto response = ParseResponse(service.HandlePayload(
        "cmd=HANDSHAKE_RESPONSE;mod_version=3.1.0"));
    Check(response.values.at("status") == "ok" &&
              response.values.at("compatible") == "true",
          "3.1.0 mod should be compatible");

    response = ParseResponse(service.HandlePayload(
        "cmd=HANDSHAKE_RESPONSE;mod_version=3.1"));
    Check(response.values.at("status") == "ok" &&
              response.values.at("compatible") == "false",
          "non-semver mod version should be incompatible");

    response = ParseResponse(service.HandlePayload(
        "cmd=HANDSHAKE_RESPONSE;mod_version=3.1.0-extra"));
    Check(response.values.at("compatible") == "false",
          "version suffixes should be rejected by strict parsing");
}

} // namespace

int main() {
    KnotLinkService service;
    TestQueriesAndTargetResolution(service);
    TestMetadataAndUnsupportedParameters(service);
    TestImportanceCommands(service);
    TestLegacyCommandsHaveNoDispatch(service);
    TestStrictModVersion(service);
    g_appState.configuration.Write().Configs().clear();

    if (failures == 0) {
        std::cout << "KnotLink command tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
