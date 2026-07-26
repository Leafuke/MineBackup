#include "KnotLinkProtocol.h"

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

void TestCapabilities() {
    const std::string manifest(KnotLinkCapabilities::ManifestJson());
    Check(manifest.find("\"specVersion\":\"1.0\"") != std::string::npos,
          "funcList spec version should be embedded");
    Check(manifest.find("\"manifestVersion\":\"2.0.0\"") != std::string::npos,
          "funcList manifest version should be embedded");
    for (const std::string command : {
             "PING", "GET_CAPABILITIES", "GET_STATUS", "LIST_CONFIGS",
             "LIST_FOLDERS", "LIST_BACKUPS", "GET_CONFIG", "BACKUP", "RESTORE",
             "BACKUP_ALL", "AUTO_BACKUP", "STOP_AUTO_BACKUP", "MARK_IMPORTANT"}) {
        Check(manifest.find("\"command\":\"" + command + "\"") != std::string::npos,
              "funcList should advertise " + command);
    }
    for (const std::string unsupported : {
             "backup_whitelist", "backup_scope", "preserve_player_data",
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
    if (failures == 0) {
        std::cout << "KnotLink protocol tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
