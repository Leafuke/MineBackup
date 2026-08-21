#include "KnotLinkProtocol.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <initializer_list>
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

using nlohmann::json;

json StaticArgument(std::string_view value) {
    return {
        {"type", "static"},
        {"value", std::string(value)},
        {"description", "Operation command."}};
}

json InputArgument(std::string_view description, std::string_view defaultValue) {
    return {
        {"type", "input"},
        {"description", std::string(description)},
        {"defaultVal", std::string(defaultValue)}};
}

json OptionalArgument(
    std::string_view description,
    std::initializer_list<std::pair<std::string_view, std::string_view>> options) {
    json encodedOptions = json::array();
    for (const auto& [label, value] : options) {
        encodedOptions.push_back(
            json::array({std::string(label), std::string(value)}));
    }
    return {
        {"type", "optional"},
        {"options", std::move(encodedOptions)},
        {"description", std::string(description)}};
}

json BooleanArgument(std::string_view description) {
    return OptionalArgument(description, {{"True", "true"}, {"False", "false"}});
}

json StatusReturns(std::initializer_list<std::string_view> extraFields) {
    json result = json::array({
        json::array({"Operation status.", "status"})});
    for (const auto field : extraFields) {
        result.push_back(json::array({
            "Response " + std::string(field) + ".", std::string(field)}));
    }
    return result;
}

void AddFunction(
    json& manifest,
    std::string_view name,
    std::string_view command,
    std::string_view description,
    json args,
    std::initializer_list<std::string_view> returnFields) {
    args["cmd"] = StaticArgument(command);
    manifest["openSocket"][std::string(name)] = {
        {"appID", std::string(KnotLinkCapabilities::AppId)},
        {"openSocketID", std::string(KnotLinkCapabilities::OpenSocketId)},
        {"description", std::string(description)},
        {"args", std::move(args)},
        {"returns", StatusReturns(returnFields)}};
}

void AddSignal(
    json& manifest,
    std::string_view name,
    std::string_view description,
    std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
    json returns = json::object({
        {"event", {
            {"description", "Signal event name."},
            {"verification", std::string(name)}}}});
    for (const auto& [field, fieldDescription] : fields) {
        returns[std::string(field)] = {
            {"description", std::string(fieldDescription)}};
    }
    manifest["signal"][std::string(name)] = {
        {"appID", std::string(KnotLinkCapabilities::AppId)},
        {"signalID", std::string(KnotLinkCapabilities::SignalId)},
        {"description", std::string(description)},
        {"returns", std::move(returns)}};
}

json ConversationArguments(json args) {
    args["from"] =
        InputArgument("Required caller identifier.", "example.client");
    args["request_id"] =
        InputArgument("Required unique request correlation ID.", "request-001");
    return args;
}

std::string BuildManifest() {
    json manifest = {
        {"specVersion", std::string(KnotLinkCapabilities::SpecVersion)},
        {"manifestVersion", std::string(KnotLinkCapabilities::ManifestVersion)},
        {"appName", "MineBackup"},
        {"openSocket", json::object()},
        {"signal", json::object()}};

    AddFunction(
        manifest, "get_capabilities", "GET_CAPABILITIES",
        "Get the runtime MineBackup funcList manifest.", json::object(),
        {"content_type", "encoding", "manifest_version", "func_list"});
    AddFunction(
        manifest, "ping", "PING",
        "Check whether the MineBackup KnotLink endpoint is available.",
        json::object(), {"message"});
    AddFunction(
        manifest, "get_status", "GET_STATUS",
        "Get MineBackup runtime status.", json::object(), {"data"});
    AddFunction(
        manifest, "list_configs", "LIST_CONFIGS",
        "List backup configurations.", json::object(), {"data"});
    AddFunction(
        manifest, "list_folders", "LIST_FOLDERS",
        "List managed folders in a backup configuration.",
        json::object({
            {"config_id", InputArgument(
                "Backup configuration ID.", "config-id")}}),
        {"data"});
    AddFunction(
        manifest, "list_backups", "LIST_BACKUPS",
        "List backup archives for a managed folder.",
        json::object({
            {"config_id", InputArgument(
                "Backup configuration ID.", "config-id")},
            {"folder", InputArgument("Folder name or index.", "0")},
            {"current_save", BooleanArgument(
                "Use the currently active Minecraft world.")}}),
        {"data"});
    AddFunction(
        manifest, "get_config", "GET_CONFIG",
        "Get public settings for a backup configuration.",
        json::object({
            {"config_id", InputArgument(
                "Backup configuration ID.", "config-id")}}),
        {"data"});

    const json backupOverrides = {
        {"comment", InputArgument("Optional backup comment.", "")},
        {"backup_mode", OptionalArgument(
            "Optional one-shot backup mode.",
            {{"Full", "full"}, {"Incremental", "incremental"}})},
        {"compression_method", OptionalArgument(
            "Optional one-shot compression method.",
            {{"LZMA2", "LZMA2"}, {"Deflate", "Deflate"},
             {"BZip2", "BZip2"}, {"zstd", "zstd"}})},
        {"compression_level", InputArgument(
            "Optional one-shot compression level.", "")},
        {"backup_blacklist", InputArgument(
            "Comma-separated one-shot blacklist rules.", "")}};

    json backupArgs = backupOverrides;
    backupArgs["config_id"] =
        InputArgument("Backup configuration ID.", "config-id");
    backupArgs["folder"] = InputArgument("Folder name or index.", "0");
    backupArgs["current_save"] =
        BooleanArgument("Use the currently active Minecraft world.");
    AddFunction(
        manifest, "backup", "BACKUP",
        "Start a backup for one managed folder.",
        ConversationArguments(std::move(backupArgs)), {"message"});

    AddFunction(
        manifest, "restore", "RESTORE",
        "Start restoring a managed folder from an archive.",
        ConversationArguments(json::object({
            {"config_id", InputArgument(
                "Backup configuration ID.", "config-id")},
            {"folder", InputArgument("Folder name or index.", "0")},
            {"current_save", BooleanArgument(
                "Use the currently active Minecraft world.")},
            {"file", InputArgument(
                "Backup archive file name; empty selects the latest archive.", "")},
            {"mode", OptionalArgument(
                "Restore mode.",
                {{"Overwrite", "overwrite"}, {"Clean", "clean"}})},
            {"confirm_partial_clean", BooleanArgument(
                "Confirm clean restore from a partial backup.")},
            {"restore_whitelist", InputArgument(
                "Comma-separated one-shot restore whitelist rules.", "")}})),
        {"message"});

    json backupAllArgs = backupOverrides;
    backupAllArgs["config_id"] =
        InputArgument("Backup configuration ID.", "config-id");
    AddFunction(
        manifest, "backup_all", "BACKUP_ALL",
        "Start backing up every folder in a configuration.",
        ConversationArguments(std::move(backupAllArgs)), {"message"});
    AddFunction(
        manifest, "mark_important", "MARK_IMPORTANT",
        "Mark or unmark a backup archive as important.",
        ConversationArguments(json::object({
            {"config_id", InputArgument(
                "Backup configuration ID.", "config-id")},
            {"folder", InputArgument("Folder name or index.", "0")},
            {"file", InputArgument("Backup archive file name.", "backup.7z")},
            {"important", BooleanArgument(
                "Whether the archive is important.")}})),
        {"message"});

    AddSignal(
        manifest, "app_startup", "MineBackup KnotLink endpoint started.",
        {{"version", "Application version."}});
    AddSignal(
        manifest, "list_configs", "Backup configuration list was queried.",
        {{"data", "Encoded result data."}});
    AddSignal(
        manifest, "list_folders", "Managed folder list was queried.",
        {{"config", "Configuration ID."}, {"data", "Encoded result data."}});
    AddSignal(
        manifest, "list_backups", "Backup archive list was queried.",
        {{"config", "Configuration ID."}, {"folder", "Folder name."},
         {"data", "Encoded result data."}});
    AddSignal(
        manifest, "get_config", "Backup configuration details were queried.",
        {{"config", "Configuration ID."}});
    AddSignal(manifest, "status", "Runtime status was queried.");
    for (const auto name : {
             "command_accepted", "command_started", "command_completed",
             "command_failed"}) {
        AddSignal(
            manifest, name, "Command lifecycle event.",
            {{"command", "Command name."},
             {"request_id", "Request correlation ID."}});
    }
    for (const auto name : {
             "backup_started", "backup_warning", "backup_success", "backup_failed",
             "restore_started", "restore_success", "restore_failed"}) {
        AddSignal(
            manifest, name, "Folder operation event.",
            {{"config", "Configuration ID."}, {"folder", "Folder name."}});
    }
    for (const auto name : {
             "backup_all_started", "backup_all_completed",
             "backup_all_failed"}) {
        AddSignal(
            manifest, name, "Configuration backup event.",
            {{"config", "Configuration ID."}});
    }
    AddSignal(
        manifest, "mark_important", "A backup importance flag changed.",
        {{"config", "Configuration ID."}, {"folder", "Folder name."},
         {"file", "Backup archive file."},
         {"important", "New importance value."}});
    return manifest.dump();
}

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
    constexpr std::array<std::string_view, 4> commands = {
        "BACKUP", "RESTORE", "BACKUP_ALL", "MARK_IMPORTANT"};
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
    static const std::string manifest = BuildManifest();
    return manifest;
}

} // namespace minebackup::knotlink
