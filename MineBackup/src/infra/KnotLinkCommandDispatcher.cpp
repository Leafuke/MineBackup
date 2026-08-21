#include "KnotLinkCommandDispatcher.h"

#include "Logging.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

using namespace std;

namespace minebackup::knotlink {
namespace {

string LowerAscii(string value) {
    transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(tolower(character));
    });
    return value;
}

optional<bool> ParseBoolean(string_view value) {
    const string normalized = LowerAscii(string(value));
    if (normalized == "true") return true;
    if (normalized == "false") return false;
    return nullopt;
}

optional<string> ValidateUnsupportedParameters(
    const KnotLinkCommandRequest& request) {
    for (const auto& [key, value] : request.values) {
        if ((key == "backup_whitelist" || key == "backup_scope"
                || key.starts_with("scope_"))
            && !value.empty()) {
            return "Parameter '" + key + "' is not supported by MineBackup.";
        }
        if (key == "preserve_player_data") {
            const auto enabled = ParseBoolean(value);
            if (!enabled.has_value()) {
                return "preserve_player_data must be true or false.";
            }
            if (*enabled) {
                return "Parameter 'preserve_player_data' is not supported by MineBackup.";
            }
        }
    }
    return nullopt;
}

} // namespace

KnotLinkCommandDispatcher::KnotLinkCommandDispatcher(Handler handler)
    : handler_(std::move(handler)) {
}

void KnotLinkCommandDispatcher::SetHandler(Handler handler) {
	lock_guard lock(mutex_);
    handler_ = std::move(handler);
}

string KnotLinkCommandDispatcher::Dispatch(string_view payload) const {
    if (!KnotLinkKeyValueCodec::HasCommandField(payload)) {
        return KnotLinkProtocolFormatter::FormatError(
            nullptr,
            "MineBackup requires KnotLink v2 key=value commands; upgrade the caller.");
    }
    try {
        auto context = make_shared<KnotLinkCommandContext>(
            KnotLinkCommandRequest::Parse(payload));
        if (const auto metadataError =
                KnotLinkCommandValidator::Validate(context->request);
            metadataError.has_value()) {
            return KnotLinkProtocolFormatter::FormatError(
                context.get(), *metadataError);
        }
        if (const auto unsupported = ValidateUnsupportedParameters(context->request);
            unsupported.has_value()) {
            return KnotLinkProtocolFormatter::FormatError(
                context.get(), *unsupported, {{"code", "unsupported_parameter"}});
        }
        logging::ScopedLogContext requestContext({
            {"request_id", context->metadata.requestId},
            {"command", context->request.command}});
        MB_LOG_DEBUG(logging::LogCategory::KnotLink,
            "knotlink.request.received",
            "Received KnotLink request '{}'", context->request.command);
        Handler handler;
        {
            lock_guard lock(mutex_);
            handler = handler_;
        }
        if (!handler) {
            return KnotLinkProtocolFormatter::FormatError(
                context.get(), "KnotLink command handling is unavailable.");
        }
        return handler(context);
    }
    catch (const KnotLinkProtocolError& error) {
        MB_LOG_WARNING(logging::LogCategory::KnotLink,
            "knotlink.request.invalid", "Invalid KnotLink request: {}", error.what());
        return KnotLinkProtocolFormatter::FormatError(nullptr, error.what());
    }
    catch (const exception& error) {
        MB_LOG_ERROR(logging::LogCategory::KnotLink,
            "knotlink.request.failed", "KnotLink request failed: {}", error.what());
        return KnotLinkProtocolFormatter::FormatError(
            nullptr, string("Command failed: ") + error.what());
    }
}

} // namespace minebackup::knotlink
