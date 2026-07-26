#pragma once

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minebackup::knotlink {

class KnotLinkProtocolError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

struct KnotLinkKeyValuePayload {
    std::map<std::string, std::string, std::less<>> values;
    std::map<std::string, std::string, std::less<>> encodedValues;
};

class KnotLinkKeyValueCodec {
public:
    using Fields = std::vector<std::pair<std::string, std::string>>;

    static bool HasCommandField(std::string_view payload);
    static KnotLinkKeyValuePayload Parse(std::string_view payload);
    static std::string Serialize(const Fields& fields);
    static std::string EncodeValue(std::string_view value);
    static std::string DecodeValue(std::string_view encodedValue);
    static std::string EncodeList(const std::vector<std::string>& values);
    static std::vector<std::string> DecodeList(std::string_view encodedValue);
    static std::string NormalizeKey(std::string_view key);

private:
    static void ValidateEncodedValue(std::string_view value);
};

struct KnotLinkCommandMetadata {
    std::string from;
    std::string requestId;
    std::string replyTo;
    std::string protocolVersion;
    std::string flow;
};

struct KnotLinkCommandRequest {
    std::string command;
    std::string rawPayload;
    std::map<std::string, std::string, std::less<>> values;
    std::map<std::string, std::string, std::less<>> encodedValues;
    KnotLinkCommandMetadata metadata;

    static KnotLinkCommandRequest Parse(std::string_view payload);
    bool Has(std::string_view key) const;
    std::string Get(std::string_view key, std::string_view fallback = {}) const;
    std::string GetEncoded(std::string_view key) const;
};

struct KnotLinkCommandContext {
    explicit KnotLinkCommandContext(KnotLinkCommandRequest requestValue)
        : request(std::move(requestValue)), metadata(request.metadata) {}

    KnotLinkCommandRequest request;
    KnotLinkCommandMetadata metadata;
};

class KnotLinkCommandValidator {
public:
    static bool RequiresConversationMetadata(std::string_view command);
    static std::optional<std::string> Validate(const KnotLinkCommandRequest& request);
};

class KnotLinkProtocolFormatter {
public:
    using Fields = KnotLinkKeyValueCodec::Fields;

    static std::string FormatOk(
        const KnotLinkCommandContext& context, const Fields& fields = {});
    static std::string FormatError(
        const KnotLinkCommandContext* context, std::string_view message,
        const Fields& fields = {});
    static std::string FormatEvent(
        const KnotLinkCommandContext* context, std::string_view eventName,
        const Fields& fields = {});

private:
    static std::string Format(
        const KnotLinkCommandContext* context,
        std::optional<std::string_view> status,
        std::optional<std::string_view> eventName,
        const Fields& fields);
};

class KnotLinkCapabilities {
public:
    static constexpr std::string_view SpecVersion = "1.0";
    static constexpr std::string_view ManifestVersion = "2.0.0";
    static constexpr std::string_view AppId = "0x00000020";
    static constexpr std::string_view OpenSocketId = "0x00000010";
    static constexpr std::string_view SignalId = "0x00000020";

    static std::string_view ManifestJson();
};

} // namespace minebackup::knotlink
