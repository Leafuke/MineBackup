#include "KnotLinkProtocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <set>

namespace minebackup::knotlink {
namespace {

bool IsKeyCharacter(unsigned char value) {
    return std::isalnum(value) != 0 || value == '_';
}

bool IsUnreserved(unsigned char value) {
    return std::isalnum(value) != 0 || value == '-' || value == '.' ||
           value == '_' || value == '~';
}

bool IsHex(unsigned char value) {
    return std::isxdigit(value) != 0;
}

unsigned char FromHex(unsigned char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
    }
    value = static_cast<unsigned char>(std::tolower(value));
    return static_cast<unsigned char>(value - 'a' + 10);
}

std::string UpperAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

bool IsBlank(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
}

bool IsReservedField(
    std::string_view key, bool hasStatus, bool hasEvent,
    const KnotLinkCommandContext* context) {
    const std::string normalized = KnotLinkKeyValueCodec::NormalizeKey(key);
    return (hasStatus && normalized == "status") ||
           (hasEvent && normalized == "event") ||
           (context != nullptr && !context->metadata.from.empty() && normalized == "from") ||
           (context != nullptr && !context->metadata.requestId.empty() &&
            normalized == "request_id");
}

constexpr std::string_view kManifest = R"json({
  "specVersion":"1.0",
  "manifestVersion":"2.0.0",
  "appName":"MineBackup",
  "openSocket":{
    "get_capabilities":{"appID":"0x00000020","openSocketID":"0x00000010","command":"GET_CAPABILITIES","args":["cmd"],"returns":["status","content_type","encoding","manifest_version","func_list"]},
    "ping":{"appID":"0x00000020","openSocketID":"0x00000010","command":"PING","args":["cmd"],"returns":["status","message"]},
    "get_status":{"appID":"0x00000020","openSocketID":"0x00000010","command":"GET_STATUS","args":["cmd"],"returns":["status","data"]},
    "list_configs":{"appID":"0x00000020","openSocketID":"0x00000010","command":"LIST_CONFIGS","args":["cmd"],"returns":["status","data"]},
    "list_folders":{"appID":"0x00000020","openSocketID":"0x00000010","command":"LIST_FOLDERS","args":["cmd","config_id"],"returns":["status","data"]},
    "list_backups":{"appID":"0x00000020","openSocketID":"0x00000010","command":"LIST_BACKUPS","args":["cmd","config_id","folder","current_save"],"returns":["status","data"]},
    "get_config":{"appID":"0x00000020","openSocketID":"0x00000010","command":"GET_CONFIG","args":["cmd","config_id"],"returns":["status","data"]},
    "backup":{"appID":"0x00000020","openSocketID":"0x00000010","command":"BACKUP","args":["cmd","from","request_id","config_id","folder","current_save","comment","backup_mode","compression_method","compression_level","backup_blacklist"],"returns":["status","message"]},
    "restore":{"appID":"0x00000020","openSocketID":"0x00000010","command":"RESTORE","args":["cmd","from","request_id","config_id","folder","current_save","file","mode","confirm_partial_clean","restore_whitelist"],"returns":["status","message"]},
    "backup_all":{"appID":"0x00000020","openSocketID":"0x00000010","command":"BACKUP_ALL","args":["cmd","from","request_id","config_id","comment","backup_mode","compression_method","compression_level","backup_blacklist"],"returns":["status","message"]},
    "auto_backup":{"appID":"0x00000020","openSocketID":"0x00000010","command":"AUTO_BACKUP","args":["cmd","from","request_id","config_id","folder","interval_minutes"],"returns":["status","message"]},
    "stop_auto_backup":{"appID":"0x00000020","openSocketID":"0x00000010","command":"STOP_AUTO_BACKUP","args":["cmd","from","request_id","config_id","folder"],"returns":["status","message"]},
    "mark_important":{"appID":"0x00000020","openSocketID":"0x00000010","command":"MARK_IMPORTANT","args":["cmd","from","request_id","config_id","folder","file","important"],"returns":["status","message"]}
  },
  "signal":{
    "app_startup":{"appID":"0x00000020","signalID":"0x00000020"},
    "command_accepted":{"appID":"0x00000020","signalID":"0x00000020"},
    "command_started":{"appID":"0x00000020","signalID":"0x00000020"},
    "command_completed":{"appID":"0x00000020","signalID":"0x00000020"},
    "command_failed":{"appID":"0x00000020","signalID":"0x00000020"},
    "backup_started":{"appID":"0x00000020","signalID":"0x00000020"},
    "backup_success":{"appID":"0x00000020","signalID":"0x00000020"},
    "backup_failed":{"appID":"0x00000020","signalID":"0x00000020"},
    "restore_started":{"appID":"0x00000020","signalID":"0x00000020"},
    "restore_success":{"appID":"0x00000020","signalID":"0x00000020"},
    "restore_failed":{"appID":"0x00000020","signalID":"0x00000020"},
    "backup_all_started":{"appID":"0x00000020","signalID":"0x00000020"},
    "backup_all_completed":{"appID":"0x00000020","signalID":"0x00000020"},
    "backup_all_failed":{"appID":"0x00000020","signalID":"0x00000020"},
    "auto_backup_started":{"appID":"0x00000020","signalID":"0x00000020"},
    "auto_backup_executed":{"appID":"0x00000020","signalID":"0x00000020"},
    "auto_backup_error":{"appID":"0x00000020","signalID":"0x00000020"},
    "auto_backup_stopped":{"appID":"0x00000020","signalID":"0x00000020"},
    "mark_important":{"appID":"0x00000020","signalID":"0x00000020"}
  }
})json";

} // namespace

bool KnotLinkKeyValueCodec::HasCommandField(std::string_view payload) {
    std::size_t start = 0;
    while (start <= payload.size()) {
        const std::size_t end = payload.find(';', start);
        const std::string_view segment =
            payload.substr(start, end == std::string_view::npos ? payload.size() - start
                                                                : end - start);
        const std::size_t separator = segment.find('=');
        if (separator != std::string_view::npos &&
            NormalizeKey(segment.substr(0, separator)) == "cmd") {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

KnotLinkKeyValuePayload KnotLinkKeyValueCodec::Parse(std::string_view payload) {
    if (payload.empty()) {
        throw KnotLinkProtocolError("The v2 payload is empty.");
    }

    KnotLinkKeyValuePayload result;
    std::size_t start = 0;
    while (start <= payload.size()) {
        const std::size_t end = payload.find(';', start);
        const std::string_view segment =
            payload.substr(start, end == std::string_view::npos ? payload.size() - start
                                                                : end - start);
        if (segment.empty()) {
            throw KnotLinkProtocolError("Empty key-value segment is not allowed.");
        }
        const std::size_t separator = segment.find('=');
        if (separator == 0 || separator == std::string_view::npos ||
            segment.find('=', separator + 1) != std::string_view::npos) {
            throw KnotLinkProtocolError("Invalid key-value segment.");
        }

        const std::string key = NormalizeKey(segment.substr(0, separator));
        if (key.empty() ||
            !std::all_of(key.begin(), key.end(), [](unsigned char value) {
                return IsKeyCharacter(value);
            })) {
            throw KnotLinkProtocolError("Invalid key.");
        }
        if (result.values.contains(key)) {
            throw KnotLinkProtocolError("Duplicate key: " + key);
        }

        const std::string encoded(segment.substr(separator + 1));
        ValidateEncodedValue(encoded);
        result.encodedValues.emplace(key, encoded);
        result.values.emplace(key, DecodeValue(encoded));

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::string KnotLinkKeyValueCodec::Serialize(const Fields& fields) {
    std::set<std::string, std::less<>> seen;
    std::string result;
    for (const auto& [sourceKey, value] : fields) {
        const std::string key = NormalizeKey(sourceKey);
        if (key.empty() ||
            !std::all_of(key.begin(), key.end(), [](unsigned char character) {
                return IsKeyCharacter(character);
            })) {
            throw KnotLinkProtocolError("Invalid KnotLink key: " + sourceKey);
        }
        if (!seen.insert(key).second) {
            throw KnotLinkProtocolError("Duplicate KnotLink key: " + key);
        }
        if (!result.empty()) {
            result.push_back(';');
        }
        result.append(key).push_back('=');
        result.append(EncodeValue(value));
    }
    return result;
}

std::string KnotLinkKeyValueCodec::EncodeValue(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if (IsUnreserved(character)) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hex[character >> 4]);
            result.push_back(hex[character & 0x0F]);
        }
    }
    return result;
}

std::string KnotLinkKeyValueCodec::DecodeValue(std::string_view encodedValue) {
    ValidateEncodedValue(encodedValue);
    std::string result;
    result.reserve(encodedValue.size());
    for (std::size_t index = 0; index < encodedValue.size(); ++index) {
        if (encodedValue[index] != '%') {
            result.push_back(encodedValue[index]);
            continue;
        }
        const auto high = FromHex(static_cast<unsigned char>(encodedValue[index + 1]));
        const auto low = FromHex(static_cast<unsigned char>(encodedValue[index + 2]));
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return result;
}

std::string KnotLinkKeyValueCodec::EncodeList(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (value.empty() || IsBlank(value)) {
            continue;
        }
        if (!result.empty()) {
            result.push_back(',');
        }
        result.append(EncodeValue(value));
    }
    return result;
}

std::vector<std::string> KnotLinkKeyValueCodec::DecodeList(std::string_view encodedValue) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= encodedValue.size()) {
        const std::size_t end = encodedValue.find(',', start);
        const std::string_view item =
            encodedValue.substr(start, end == std::string_view::npos
                                           ? encodedValue.size() - start
                                           : end - start);
        if (!item.empty()) {
            const std::string decoded = DecodeValue(item);
            if (!decoded.empty() && !IsBlank(decoded)) {
                result.push_back(decoded);
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::string KnotLinkKeyValueCodec::NormalizeKey(std::string_view key) {
    std::string result(key);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

void KnotLinkKeyValueCodec::ValidateEncodedValue(std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (IsUnreserved(character) || character == ',') {
            continue;
        }
        if (character == '%' && index + 2 < value.size() &&
            IsHex(static_cast<unsigned char>(value[index + 1])) &&
            IsHex(static_cast<unsigned char>(value[index + 2]))) {
            index += 2;
            continue;
        }
        throw KnotLinkProtocolError(
            "Value contains a character that must be percent-encoded.");
    }
}

KnotLinkCommandRequest KnotLinkCommandRequest::Parse(std::string_view payload) {
    auto parsed = KnotLinkKeyValueCodec::Parse(payload);
    const auto command = parsed.values.find("cmd");
    if (command == parsed.values.end() || command->second.empty() ||
        IsBlank(command->second)) {
        throw KnotLinkProtocolError("Missing or empty cmd field.");
    }
    if (!std::all_of(command->second.begin(), command->second.end(),
                     [](unsigned char character) { return IsKeyCharacter(character); })) {
        throw KnotLinkProtocolError(
            "The cmd value may contain only letters, digits, and underscores.");
    }

    KnotLinkCommandRequest request;
    request.command = UpperAscii(command->second);
    request.rawPayload = std::string(payload);
    request.values = std::move(parsed.values);
    request.encodedValues = std::move(parsed.encodedValues);
    request.metadata.from = request.Get("from");
    request.metadata.requestId = request.Get("request_id");
    request.metadata.replyTo = request.Get("reply_to");
    request.metadata.protocolVersion = request.Get("protocol_version");
    request.metadata.flow = request.Get("flow");
    return request;
}

bool KnotLinkCommandRequest::Has(std::string_view key) const {
    return values.contains(KnotLinkKeyValueCodec::NormalizeKey(key));
}

std::string KnotLinkCommandRequest::Get(
    std::string_view key, std::string_view fallback) const {
    const auto value = values.find(KnotLinkKeyValueCodec::NormalizeKey(key));
    return value == values.end() ? std::string(fallback) : value->second;
}

std::string KnotLinkCommandRequest::GetEncoded(std::string_view key) const {
    const auto value = encodedValues.find(KnotLinkKeyValueCodec::NormalizeKey(key));
    return value == encodedValues.end() ? std::string{} : value->second;
}

bool KnotLinkCommandValidator::RequiresConversationMetadata(std::string_view command) {
    constexpr std::array<std::string_view, 6> commands = {
        "BACKUP", "RESTORE", "BACKUP_ALL", "AUTO_BACKUP",
        "STOP_AUTO_BACKUP", "MARK_IMPORTANT"};
    const std::string normalized = UpperAscii(command);
    return std::find(commands.begin(), commands.end(), normalized) != commands.end();
}

std::optional<std::string> KnotLinkCommandValidator::Validate(
    const KnotLinkCommandRequest& request) {
    if (!RequiresConversationMetadata(request.command)) {
        return std::nullopt;
    }
    if (request.metadata.from.empty() || IsBlank(request.metadata.from)) {
        return "Missing required from field.";
    }
    if (request.metadata.requestId.empty() || IsBlank(request.metadata.requestId)) {
        return "Missing required request_id field.";
    }
    return std::nullopt;
}

std::string KnotLinkProtocolFormatter::FormatOk(
    const KnotLinkCommandContext& context, const Fields& fields) {
    return Format(&context, "ok", std::nullopt, fields);
}

std::string KnotLinkProtocolFormatter::FormatError(
    const KnotLinkCommandContext* context, std::string_view message,
    const Fields& fields) {
    Fields combined{{"message", std::string(message)}};
    combined.insert(combined.end(), fields.begin(), fields.end());
    return Format(context, "error", std::nullopt, combined);
}

std::string KnotLinkProtocolFormatter::FormatEvent(
    const KnotLinkCommandContext* context, std::string_view eventName,
    const Fields& fields) {
    return Format(context, std::nullopt, eventName, fields);
}

std::string KnotLinkProtocolFormatter::Format(
    const KnotLinkCommandContext* context,
    std::optional<std::string_view> status,
    std::optional<std::string_view> eventName,
    const Fields& fields) {
    Fields output;
    if (status.has_value()) {
        output.emplace_back("status", std::string(*status));
    }
    if (eventName.has_value()) {
        output.emplace_back("event", std::string(*eventName));
    }
    if (context != nullptr && !context->metadata.from.empty()) {
        output.emplace_back("from", context->metadata.from);
    }
    if (context != nullptr && !context->metadata.requestId.empty()) {
        output.emplace_back("request_id", context->metadata.requestId);
    }
    for (const auto& field : fields) {
        if (!IsReservedField(
                field.first, status.has_value(), eventName.has_value(), context)) {
            output.push_back(field);
        }
    }
    return KnotLinkKeyValueCodec::Serialize(output);
}

std::string_view KnotLinkCapabilities::ManifestJson() {
    return kManifest;
}

} // namespace minebackup::knotlink
