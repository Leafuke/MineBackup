#include "KnotLinkProtocol.h"
#include "json.hpp"

#include <functional>
#include <iostream>
#include <string>
#include <vector>

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

void CheckThrows(const std::function<void()>& action, const std::string& message) {
    try {
        action();
    } catch (const KnotLinkProtocolError&) {
        return;
    }
    Check(false, message);
}

void TestEncoding() {
    const std::string unicode = "主世界 / Nether;50%";
    const std::string encoded = KnotLinkKeyValueCodec::EncodeValue(unicode);
    Check(encoded == "%E4%B8%BB%E4%B8%96%E7%95%8C%20%2F%20Nether%3B50%25",
          "Unicode and reserved characters should use RFC 3986 encoding");
    Check(KnotLinkKeyValueCodec::DecodeValue(encoded) == unicode,
          "encoded Unicode should round-trip");

    const std::vector<std::string> list{"a,b", "世界", "path/value"};
    const std::string encodedList = KnotLinkKeyValueCodec::EncodeList(list);
    Check(encodedList == "a%2Cb,%E4%B8%96%E7%95%8C,path%2Fvalue",
          "list entries should be encoded independently");
    Check(KnotLinkKeyValueCodec::DecodeList(encodedList) == list,
          "encoded list should round-trip");
}

void TestStrictParsing() {
    const auto request = KnotLinkCommandRequest::Parse(
        "CmD=backup;FROM=mod;Request_ID=req-1;comment=hello%20world");
    Check(request.command == "BACKUP", "command should normalize to uppercase");
    Check(request.metadata.from == "mod" && request.metadata.requestId == "req-1",
          "metadata keys should be case-insensitive");
    Check(request.Get("comment") == "hello world", "values should be decoded");
    Check(KnotLinkKeyValueCodec::HasCommandField("foo=x;CMD=PING"),
          "command field detection should be case-insensitive");

    CheckThrows([] { KnotLinkCommandRequest::Parse(""); }, "empty payload must fail");
    CheckThrows([] { KnotLinkCommandRequest::Parse("cmd=PING;"); },
                "empty segment must fail");
    CheckThrows([] { KnotLinkCommandRequest::Parse("cmd=PING;;from=x"); },
                "middle empty segment must fail");
    CheckThrows([] { KnotLinkCommandRequest::Parse("cmd=PING;CMD=PONG"); },
                "duplicate normalized key must fail");
    CheckThrows([] { KnotLinkCommandRequest::Parse("cmd=PING=BAD"); },
                "multiple equals signs must fail");
    CheckThrows([] { KnotLinkCommandRequest::Parse("bad-key=x;cmd=PING"); },
                "invalid key must fail");
    CheckThrows([] { KnotLinkCommandRequest::Parse("cmd=PING;value=%GG"); },
                "invalid percent escape must fail");
    CheckThrows([] { KnotLinkCommandRequest::Parse("cmd=PING;value=raw space"); },
                "unencoded reserved character must fail");
    CheckThrows([] { KnotLinkCommandRequest::Parse("from=mod"); },
                "missing command must fail");
    CheckThrows([] { KnotLinkCommandRequest::Parse("cmd=BACK-UP"); },
                "invalid command character must fail");
}

void TestMetadataAndFormatting() {
    const auto missing = KnotLinkCommandRequest::Parse("cmd=BACKUP");
    Check(KnotLinkCommandValidator::Validate(missing).has_value(),
          "mutating command should require conversation metadata");
    const auto query = KnotLinkCommandRequest::Parse("cmd=PING");
    Check(!KnotLinkCommandValidator::Validate(query).has_value(),
          "query should not require conversation metadata");

    KnotLinkCommandContext context(KnotLinkCommandRequest::Parse(
        "cmd=BACKUP;from=test.client;request_id=req%2F1"));
    const auto response = KnotLinkProtocolFormatter::FormatOk(
        context, {{"status", "spoofed"}, {"from", "spoofed"}, {"message", "done"}});
    Check(response ==
              "status=ok;from=test.client;request_id=req%2F1;message=done",
          "reserved response fields must not be overwritten");

    const auto event = KnotLinkProtocolFormatter::FormatEvent(
        &context, "command_completed", {{"command", "BACKUP"}});
    Check(event ==
              "event=command_completed;from=test.client;request_id=req%2F1;command=BACKUP",
          "events should inherit correlation metadata");
}

void TestOperationContextAndCallbacks() {
    auto outer = std::make_shared<KnotLinkCommandContext>(KnotLinkCommandRequest::Parse(
        "cmd=RESTORE;from=test.client;request_id=restore-1"));
    Check(!KnotLinkCommandScope::Current(), "No ambient command should exist outside a scope");
    {
        KnotLinkCommandScope scope(outer);
        Check(KnotLinkCommandScope::Current() == outer, "Runtime events inherit the executing command");
        {
            KnotLinkCommandScope nested({});
            Check(!KnotLinkCommandScope::Current(), "Nested scopes may clear context");
        }
        Check(KnotLinkCommandScope::Current() == outer, "Nested scope restores its caller context");
    }
    Check(!KnotLinkCommandScope::Current(), "Command context must not leak into later operations");
    KnotLinkCallbackTracker tracker;
    const auto callback = [](const std::string& text) { return KnotLinkCommandRequest::Parse(text); };
    Check(tracker.Validate(callback("cmd=WORLD_SAVED")).has_value(), "Orphan callbacks are rejected");
    tracker.ObserveEvent("handshake", {{"world", "world-one"}}, outer.get());
    Check(!tracker.Validate(callback("cmd=HANDSHAKE_RESPONSE;request_id=fresh-callback-id;mod_version=3.3.2")),
        "Integrated-server callbacks use their own new UUID");
    Check(tracker.Validate(callback("cmd=WORLD_SAVED")).has_value(), "Out-of-phase callbacks are rejected");
    tracker.ObserveEvent("pre_hot_backup", {});
    Check(tracker.Validate(callback("cmd=WORLD_SAVED;world=world-two")).has_value(), "Cross-world callbacks are rejected");
    Check(!tracker.Validate(callback("cmd=WORLD_SAVED;world=world-one;request_id=another-uuid")),
        "Matching phase/world accepts the callback's independent UUID");
    tracker.ObserveEvent("backup_success", {});
    Check(tracker.Validate(callback("cmd=WORLD_SAVED")).has_value(), "Terminal events close pending callbacks");
    Check(KnotLinkKeyValueCodec::DecodeOperationList("ftbquests%2F,ftbteams%2F")
            == std::vector<std::string>{"ftbquests/", "ftbteams/"}, "Canonical operation lists decode per item");
    Check(KnotLinkKeyValueCodec::DecodeOperationList("ftbquests%2F%2Cftbteams%2F")
            == std::vector<std::string>{"ftbquests/", "ftbteams/"}, "Legacy whole-value CSV remains supported");
    CheckThrows([] { KnotLinkKeyValueCodec::DecodeOperationList("a,,b", true); },
        "Canonical preserve paths reject empty elements");
    CheckThrows([] { KnotLinkKeyValueCodec::DecodeOperationList("a%2C%20%2Cb", true); },
        "Legacy preserve paths reject blank elements");
    Check(KnotLinkKeyValueCodec::DecodeOperationList("a%252Fb%2Cc")
            == std::vector<std::string>{"a%2Fb", "c"}, "Legacy CSV values are decoded only once");
}

void TestCapabilities() {
    const std::string manifest(KnotLinkCapabilities::ManifestJson());
    const auto document = nlohmann::json::parse(manifest);
    Check(document.at("specVersion") == "1.0",
          "funcList spec version should be embedded");
    Check(document.at("manifestVersion") == "2.1.0",
          "funcList manifest version should be embedded");
    Check(document.at("openSocket").is_object() &&
              document.at("signal").is_object(),
          "funcList openSocket and signal sections should be objects");
    for (const auto& [functionName, command] :
         std::vector<std::pair<std::string, std::string>>{
             {"ping", "PING"},
             {"get_capabilities", "GET_CAPABILITIES"},
             {"get_status", "GET_STATUS"},
             {"list_configs", "LIST_CONFIGS"},
             {"list_folders", "LIST_FOLDERS"},
             {"list_backups", "LIST_BACKUPS"},
             {"get_config", "GET_CONFIG"},
             {"backup", "BACKUP"},
             {"restore", "RESTORE"},
             {"backup_all", "BACKUP_ALL"},
             {"mark_important", "MARK_IMPORTANT"}}) {
        const auto& function = document.at("openSocket").at(functionName);
        Check(function.at("args").is_object() &&
                  function.at("args").at("cmd").at("type") == "static" &&
                  function.at("args").at("cmd").at("value") == command,
              "funcList should advertise " + command +
                  " through a static cmd argument");
        Check(function.at("returns").is_array(),
              "funcList returns should use FolderRewind tuple arrays");
    }
    Check(!document.at("openSocket").at("list_configs").contains("command"),
          "funcList functions should not use the non-standard command property");
    for (const std::string command : {"backup", "backup_all"}) {
        for (const std::string field : {"backup_whitelist", "backup_scope", "scope_dimensions", "scope_areas"}) {
            Check(document.at("openSocket").at(command).at("args").contains(field),
                "Validated one-shot backup selection is discoverable: " + field);
        }
    }
    Check(document.at("openSocket").at("restore").at("args").contains("restore_preserve_paths")
        && document.at("openSocket").at("restore").at("args").contains("preserve_player_data"),
        "Preservation controls are discoverable for restore");
    Check(document.at("signal").contains("backup_warning"),
          "funcList should advertise the backup warning notification");
    for (const std::string unsupported : {
             "AUTO_BACKUP", "STOP_AUTO_BACKUP",
             "RESTORE_CURRENT", "LIST_WORLDS", "SEND"}) {
        Check(manifest.find(unsupported) == std::string::npos,
              "funcList must not advertise unsupported feature " + unsupported);
    }
}

} // namespace

int main() {
    TestEncoding();
    TestStrictParsing();
    TestMetadataAndFormatting();
    TestCapabilities();
    TestOperationContextAndCallbacks();
    if (failures == 0) {
        std::cout << "KnotLink protocol tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
