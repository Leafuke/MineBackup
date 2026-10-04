#include "KnotLinkCommandDispatcher.h"

#include "Logging.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
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
    const bool backup = request.command == "BACKUP" || request.command == "BACKUP_ALL";
    const bool restore = request.command == "RESTORE";
    const set<string, less<>> backupKeys{
        "backup_mode", "backup_blacklist", "backup_whitelist", "backup_scope",
        "scope_dimensions", "scope_areas", "compression_method", "compression_level"};
    const set<string, less<>> restoreKeys{
        "mode", "restore_whitelist", "restore_preserve_paths",
        "preserve_player_data", "confirm_partial_clean"};
    const map<string, set<string>, less<>> commandParameters{
        {"config_id", {"LIST_FOLDERS", "LIST_BACKUPS", "GET_CONFIG", "BACKUP", "BACKUP_ALL", "RESTORE", "MARK_IMPORTANT", "GET_IMPORTANCE"}},
        {"folder", {"LIST_BACKUPS", "BACKUP", "RESTORE", "MARK_IMPORTANT", "GET_IMPORTANCE"}},
        {"current_save", {"LIST_BACKUPS", "BACKUP", "RESTORE", "MARK_IMPORTANT", "GET_IMPORTANCE"}},
        {"comment", {"BACKUP", "BACKUP_ALL"}},
        {"file", {"RESTORE", "MARK_IMPORTANT", "GET_IMPORTANCE"}},
        {"important", {"MARK_IMPORTANT"}},
        {"protect", {"BACKUP"}},
        {"mod_version", {"HANDSHAKE_RESPONSE"}},
        {"result", {"REJOIN_RESULT"}},
        {"reason", {"REJOIN_RESULT"}},
        {"world", {"HANDSHAKE_RESPONSE", "WORLD_SAVED", "WORLD_SAVE_AND_EXIT_COMPLETE", "REJOIN_RESULT"}}};
    for (const auto& [key, value] : request.values) {
        if (const auto known = commandParameters.find(key); known != commandParameters.end()
            && !known->second.contains(request.command)) {
            return "Parameter '" + key + "' is not supported for " + request.command + ".";
        }
        if (key.starts_with("scope_") && !backupKeys.contains(key)) {
            return "Parameter '" + key + "' is not supported by MineBackup.";
        }
        if ((backupKeys.contains(key) && !backup)
            || (restoreKeys.contains(key) && !restore)) {
            return "Parameter '" + key + "' is not supported for " + request.command + ".";
        }
        // A misspelled safety option must never silently become a destructive default.
        if (((key.starts_with("restore_") || key.starts_with("preserve_"))
                && !restoreKeys.contains(key))
            || ((key.starts_with("backup_") || key.starts_with("compression_"))
                && !backupKeys.contains(key))) {
            return "Unknown operation parameter '" + key + "'.";
        }
        if (key == "preserve_player_data" || key == "confirm_partial_clean"
            || key == "protect" || key == "important" || key == "current_save") {
            if (!ParseBoolean(value).has_value()) {
                return key + " must be true or false.";
            }
        }
    }
    if (request.command == "BACKUP" && ParseBoolean(request.Get("protect", "false")).value_or(false)) {
        const string scope = LowerAscii(request.Get("backup_scope"));
        if (!request.Get("backup_whitelist").empty()
            || (scope != "" && scope != "full" && scope != "all"
                && scope != "default" && scope != "none")
            || !request.Get("scope_dimensions").empty() || !request.Get("scope_areas").empty()) {
            return "protect=true requires a complete-world backup; partial selection is not supported.";
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
    shared_ptr<KnotLinkCommandContext> context;
    try {
        context = make_shared<KnotLinkCommandContext>(
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
        return KnotLinkProtocolFormatter::FormatError(context.get(), error.what());
    }
    catch (const exception& error) {
        MB_LOG_ERROR(logging::LogCategory::KnotLink,
            "knotlink.request.failed", "KnotLink request failed: {}", error.what());
        return KnotLinkProtocolFormatter::FormatError(
            context.get(), string("Command failed: ") + error.what());
    }
}

} // namespace minebackup::knotlink
