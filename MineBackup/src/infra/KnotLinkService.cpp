#include "KnotLinkService.h"
#include "GameSessionManager.h"

#include "KnotLinkCommandDispatcher.h"
#include "Logging.h"
#include "knotlink/OpenSocketResponser.hpp"
#include "knotlink/SignalSender.hpp"

#include "AppState.h"
#include "BackupManager.h"
#include "Broadcast.h"
#include "FolderRewindFormat.h"
#include "Globals.h"
#include "HistoryManager.h"
#include "TaskCoordinator.h"
#include "text_to_text.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <stop_token>
#include <utility>

namespace minebackup::knotlink {
namespace {

thread_local std::shared_ptr<KnotLinkCommandContext> g_commandContext;

struct ResolvedFolder {
    Config config;
    int configIndex = -1;
    int folderIndex = -1;
    std::wstring folderName;
    std::wstring folderPath;

    MyFolder ToMyFolder() const {
        return {folderPath,
                folderName,
                config.worlds[static_cast<std::size_t>(folderIndex)].second,
                config,
                configIndex,
                folderIndex};
    }
};

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string JoinDelimited(
    const std::vector<std::string>& values, char delimiter = ';') {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            result.push_back(delimiter);
        }
        result.append(values[index]);
    }
    return result;
}

std::string BackupModeName(int mode) {
    switch (mode) {
    case 2:
        return "Incremental";
    case 3:
        return "Overwrite";
    default:
        return "Full";
    }
}

bool IsSupportedBackupArchive(const std::filesystem::path& path) {
    const std::string extension =
        LowerAscii(wstring_to_utf8(path.extension().wstring()));
    return extension == ".7z" || extension == ".zip";
}

bool TryParseInteger(std::string_view value, int& result) {
    if (value.empty()) {
        return false;
    }
    const char* first = value.data();
    const char* last = first + value.size();
    const auto parsed = std::from_chars(first, last, result);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

std::optional<bool> ParseBoolean(std::string_view value) {
    const std::string normalized = LowerAscii(std::string(value));
    if (normalized == "true") {
        return true;
    }
    if (normalized == "false") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::pair<int, Config>> ResolveConfig(std::string_view identifier) {
    if (identifier.empty()) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(g_appState.configsMutex);
    const std::wstring wideIdentifier = utf8_to_wstring(std::string(identifier));
    for (const auto& [index, config] : g_appState.configs) {
        if (!config.configId.empty() && config.configId == wideIdentifier) {
            return std::pair{index, config};
        }
    }
    for (const auto& [index, config] : g_appState.configs) {
        if (config.name == identifier) {
            return std::pair{index, config};
        }
    }
    int numericIndex = -1;
    if (TryParseInteger(identifier, numericIndex)) {
        const auto found = g_appState.configs.find(numericIndex);
        if (found != g_appState.configs.end()) {
            return std::pair{found->first, found->second};
        }
    }
    return std::nullopt;
}

std::optional<ResolvedFolder> ResolveFolder(
    const KnotLinkCommandRequest& request, std::string& error) {
    const auto currentSave = ParseBoolean(request.Get("current_save", "false"));
    if (!currentSave.has_value()) {
        error = "current_save must be true or false.";
        return std::nullopt;
    }
    if (*currentSave) {
        std::vector<ResolvedFolder> occupied;
        {
            std::lock_guard<std::mutex> lock(g_appState.configsMutex);
            for (const auto& [configIndex, config] : g_appState.configs) {
                for (std::size_t worldIndex = 0;
                     worldIndex < config.worlds.size(); ++worldIndex) {
                    const auto& world = config.worlds[worldIndex];
                    const std::filesystem::path path =
                        JoinPath(config.saveRoot, world.first);
                    if (IsWorldOccupied(path)) {
                        occupied.push_back({
                            config, configIndex, static_cast<int>(worldIndex),
                            world.first, path.wstring()});
                    }
                }
            }
        }
        if (occupied.empty()) {
            error = "No active world was found.";
            return std::nullopt;
        }
        if (occupied.size() != 1) {
            error = "Multiple active worlds were found; specify config_id and folder.";
            return std::nullopt;
        }
        return occupied.front();
    }

    const auto resolvedConfig = ResolveConfig(request.Get("config_id"));
    if (!resolvedConfig.has_value()) {
        error = "Unknown or missing config_id.";
        return std::nullopt;
    }
    const std::string selector = request.Get("folder");
    if (selector.empty()) {
        error = "Missing folder.";
        return std::nullopt;
    }

    const auto& [configIndex, config] = *resolvedConfig;
    int folderIndex = -1;
    if (TryParseInteger(selector, folderIndex) && folderIndex >= 0 &&
        folderIndex < static_cast<int>(config.worlds.size())) {
        const auto& folder = config.worlds[static_cast<std::size_t>(folderIndex)];
        return ResolvedFolder{
            config, configIndex, folderIndex, folder.first,
            JoinPath(config.saveRoot, folder.first).wstring()};
    }

    const std::wstring wideSelector = utf8_to_wstring(selector);
    for (std::size_t index = 0; index < config.worlds.size(); ++index) {
        const auto& folder = config.worlds[index];
        const std::filesystem::path fullPath = JoinPath(config.saveRoot, folder.first);
        if (folder.first == wideSelector ||
            fullPath.lexically_normal() == std::filesystem::path(wideSelector).lexically_normal()) {
            return ResolvedFolder{
                config, configIndex, static_cast<int>(index), folder.first,
                fullPath.wstring()};
        }
    }
    error = "Unknown folder.";
    return std::nullopt;
}

std::optional<Config> ApplyBackupOverrides(
    const KnotLinkCommandRequest& request, const Config& source, std::string& error) {
    Config config = source;
    if (request.Has("backup_mode") && !request.Get("backup_mode").empty()) {
        const std::string mode = LowerAscii(request.Get("backup_mode"));
        if (mode == "full") {
            config.backupMode = 1;
        } else if (mode == "incremental") {
            config.backupMode = 2;
        } else {
            error = "backup_mode must be full or incremental.";
            return std::nullopt;
        }
    }

    if (request.Has("compression_method") &&
        !request.Get("compression_method").empty()) {
        const std::string method = LowerAscii(request.Get("compression_method"));
        if (method == "lzma2") {
            config.zipMethod = L"LZMA2";
        } else if (method == "deflate") {
            config.zipMethod = L"Deflate";
        } else if (method == "bzip2") {
            config.zipMethod = L"BZip2";
        } else if (method == "zstd") {
            config.zipMethod = L"zstd";
        } else {
            error = "Unsupported compression_method.";
            return std::nullopt;
        }
    }

    if (request.Has("compression_level") &&
        !request.Get("compression_level").empty()) {
        int level = -1;
        if (!TryParseInteger(request.Get("compression_level"), level)) {
            error = "compression_level must be an integer.";
            return std::nullopt;
        }
        const std::string method = LowerAscii(wstring_to_utf8(config.zipMethod));
        const int minimum = method == "zstd" || method == "bzip2" ? 1 : 0;
        const int maximum = method == "zstd" ? 22 : 9;
        if (level < minimum || level > maximum) {
            error = "compression_level is outside the supported range.";
            return std::nullopt;
        }
        config.zipLevel = level;
    }

    if (request.Has("backup_blacklist")) {
        for (const auto& item :
             KnotLinkKeyValueCodec::DecodeList(request.GetEncoded("backup_blacklist"))) {
            const std::wstring rule = utf8_to_wstring(item);
            if (std::find(config.blacklist.begin(), config.blacklist.end(), rule) ==
                config.blacklist.end()) {
                config.blacklist.push_back(rule);
            }
        }
    }
    return config;
}

std::filesystem::path BackupDirectory(const ResolvedFolder& folder) {
    FolderRewindFormat::StoragePaths paths;
    if (FolderRewindFormat::TryResolveStoragePaths(
            folder.config.backupPath, folder.folderName, folder.folderPath, paths)) {
        return paths.backupSubDir;
    }
    return JoinPath(folder.config.backupPath, folder.folderName);
}

std::optional<std::wstring> LatestBackup(const ResolvedFolder& folder) {
    const auto directory = BackupDirectory(folder);
    if (!std::filesystem::exists(directory)) {
        return std::nullopt;
    }
    std::optional<std::filesystem::directory_entry> latest;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            return std::nullopt;
        }
        if (!entry.is_regular_file() ||
            !IsSupportedBackupArchive(entry.path())) {
            continue;
        }
        if (!latest.has_value() ||
            entry.last_write_time(error) > latest->last_write_time(error)) {
            latest = entry;
        }
    }
    return latest.has_value()
        ? std::optional<std::wstring>(latest->path().filename().wstring())
        : std::nullopt;
}

KnotLinkProtocolFormatter::Fields EventTargetFields(const ResolvedFolder& folder) {
    return {
        {"config", wstring_to_utf8(folder.config.configId)},
        {"folder", wstring_to_utf8(folder.folderName)}};
}

} // namespace

struct KnotLinkService::Implementation {
    std::unique_ptr<::knotlink::SignalSender> sender;
    std::unique_ptr<::knotlink::OpenSocketResponser> responder;
    KnotLinkCommandDispatcher dispatcher;
};

class KnotLinkService::ContextScope {
public:
    explicit ContextScope(std::shared_ptr<KnotLinkCommandContext> context)
        : previous_(std::move(g_commandContext)) {
        g_commandContext = std::move(context);
    }

    ~ContextScope() {
        g_commandContext = std::move(previous_);
    }

private:
    std::shared_ptr<KnotLinkCommandContext> previous_;
};

KnotLinkService::KnotLinkService()
    : implementation_(std::make_unique<Implementation>()) {
    implementation_->dispatcher.SetHandler(
        [this](const std::shared_ptr<KnotLinkCommandContext>& context) {
            return HandleRequest(context);
        });
}

KnotLinkService::~KnotLinkService() {
    Stop();
}

bool KnotLinkService::Start() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (running_.load()) {
        return true;
    }
    try {
        implementation_->sender = std::make_unique<::knotlink::SignalSender>(
            std::string(KnotLinkCapabilities::AppId),
            std::string(KnotLinkCapabilities::SignalId));
        implementation_->responder =
            std::make_unique<::knotlink::OpenSocketResponser>(
                std::string(KnotLinkCapabilities::AppId),
                std::string(KnotLinkCapabilities::OpenSocketId));
        implementation_->responder->setQuestionHandler(
            [this](const std::string& payload) {
                return HandlePayload(payload);
            });
        running_.store(true);
        MB_LOG_INFO(logging::LogCategory::KnotLink,
            "knotlink.service.started", "KnotLink service started");
        return true;
    } catch (const std::exception& error) {
        implementation_->responder.reset();
        implementation_->sender.reset();
        MB_LOG_ERROR(logging::LogCategory::KnotLink,
            "knotlink.service.start_failed",
            "KnotLink initialization failed: {}", error.what());
        running_.store(false);
        return false;
    }
}

void KnotLinkService::Stop() {
    std::unique_ptr<::knotlink::OpenSocketResponser> responder;
    std::unique_ptr<::knotlink::SignalSender> sender;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        running_.store(false);
        responder = std::move(implementation_->responder);
        sender = std::move(implementation_->sender);
    }
    responder.reset();
    sender.reset();
}

bool KnotLinkService::IsRunning() const noexcept {
    return running_.load();
}

std::shared_ptr<KnotLinkCommandContext> KnotLinkService::CurrentCommandContext() {
    return g_commandContext;
}

void KnotLinkService::Broadcast(
    std::string_view eventName,
    const KnotLinkProtocolFormatter::Fields& fields,
    const std::shared_ptr<KnotLinkCommandContext>& context) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (!implementation_->sender) {
        return;
    }
    const auto effectiveContext = context ? context : g_commandContext;
    implementation_->sender->emitt(KnotLinkProtocolFormatter::FormatEvent(
        effectiveContext.get(), eventName, fields));
}

void KnotLinkService::BroadcastLegacyPayload(std::string_view payload) {
    KnotLinkProtocolFormatter::Fields fields;
    std::string eventName;
    try {
        const auto parsed = KnotLinkKeyValueCodec::Parse(payload);
        const auto event = parsed.values.find("event");
        if (event == parsed.values.end() || event->second.empty()) {
            return;
        }
        eventName = event->second;
        for (const auto& [key, value] : parsed.values) {
            if (key != "event") {
                fields.emplace_back(key, value);
            }
        }
    } catch (const KnotLinkProtocolError&) {
        std::size_t start = 0;
        while (start <= payload.size()) {
            const std::size_t end = payload.find(';', start);
            const std::string_view segment =
                payload.substr(start, end == std::string_view::npos
                                          ? payload.size() - start
                                          : end - start);
            const std::size_t separator = segment.find('=');
            if (separator != std::string_view::npos && separator > 0) {
                const std::string key =
                    KnotLinkKeyValueCodec::NormalizeKey(segment.substr(0, separator));
                const std::string value(segment.substr(separator + 1));
                if (key == "event") {
                    eventName = value;
                } else {
                    fields.emplace_back(key, value);
                }
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
    }
    if (!eventName.empty()) {
        Broadcast(eventName, fields);
    }
}

std::string KnotLinkService::HandlePayload(
    std::string_view payload) {
    return implementation_->dispatcher.Dispatch(payload);
}

std::string KnotLinkService::HandleRequest(
    const std::shared_ptr<KnotLinkCommandContext>& context) {
    const auto& request = context->request;
    auto error = [&](std::string_view message,
                     KnotLinkProtocolFormatter::Fields fields = {}) {
        fields.emplace_back("command", request.command);
        return KnotLinkProtocolFormatter::FormatError(
            context.get(), message, fields);
    };
    auto ok = [&](KnotLinkProtocolFormatter::Fields fields = {}) {
        return KnotLinkProtocolFormatter::FormatOk(*context, fields);
    };

    if (request.command == "PING") {
        return ok({{"message", "pong"}, {"version", CURRENT_VERSION}});
    }
    if (request.command == "GET_CAPABILITIES") {
        return ok({
            {"content_type", "application/json"},
            {"encoding", "percent"},
            {"manifest_version", std::string(KnotLinkCapabilities::ManifestVersion)},
            {"func_list", std::string(KnotLinkCapabilities::ManifestJson())}});
    }
    if (request.command == "GET_STATUS") {
        const std::string enabled = g_enableKnotLink ? "True" : "False";
        const std::string initialized = IsRunning() ? "True" : "False";
        const std::string activeTaskCount =
            std::to_string(TaskCoordinator::Instance().ActiveTaskCount());
        const std::string data =
            "enabled=" + enabled +
            ";initialized=" + initialized +
            ";active_tasks=" + activeTaskCount;
        Broadcast("status",
                  {{"enabled", enabled},
                   {"initialized", initialized},
                   {"active_tasks", activeTaskCount}},
                  context);
        return ok({{"data", data}});
    }
    if (request.command == "LIST_CONFIGS") {
        std::vector<std::string> records;
        {
            std::lock_guard<std::mutex> lock(g_appState.configsMutex);
            for (const auto& [index, config] : g_appState.configs) {
                (void)index;
                records.push_back(
                    wstring_to_utf8(config.configId) + "," + config.name);
            }
        }
        const std::string data = JoinDelimited(records);
        Broadcast("list_configs", {{"data", data}}, context);
        return ok({{"data", data}});
    }
    if (request.command == "LIST_FOLDERS") {
        const auto resolved = ResolveConfig(request.Get("config_id"));
        if (!resolved.has_value()) {
            return error("Unknown or missing config_id.");
        }
        const auto& [configIndex, config] = *resolved;
        (void)configIndex;
        std::vector<std::string> folders;
        for (const auto& folder : config.worlds) {
            folders.push_back(wstring_to_utf8(folder.first));
        }
        const std::string data = JoinDelimited(folders);
        Broadcast("list_folders",
                  {{"config", wstring_to_utf8(config.configId)}, {"data", data}},
                  context);
        return ok({{"data", data}});
    }
    if (request.command == "LIST_BACKUPS") {
        std::string targetError;
        const auto folder = ResolveFolder(request, targetError);
        if (!folder.has_value()) {
            return error(targetError);
        }
        std::vector<std::string> archiveNames;
        const auto directory = BackupDirectory(*folder);
        std::error_code iteratorError;
        if (std::filesystem::exists(directory)) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(directory, iteratorError)) {
                if (!iteratorError && entry.is_regular_file() &&
                    IsSupportedBackupArchive(entry.path())) {
                    archiveNames.push_back(
                        wstring_to_utf8(entry.path().filename().wstring()));
                }
            }
        }
        const std::string data = JoinDelimited(archiveNames);
        auto fields = EventTargetFields(*folder);
        fields.emplace_back("data", data);
        Broadcast("list_backups", fields, context);
        return ok({{"data", data}});
    }
    if (request.command == "GET_CONFIG") {
        const auto resolved = ResolveConfig(request.Get("config_id"));
        if (!resolved.has_value()) {
            return error("Unknown or missing config_id.");
        }
        const auto& [configIndex, config] = *resolved;
        (void)configIndex;
        const std::string mode = BackupModeName(config.backupMode);
        const std::string format = wstring_to_utf8(config.zipFormat);
        const std::string keepCount = std::to_string(config.keepCount);
        const std::string data =
            "name=" + config.name +
            ";backup_mode=" + mode +
            ";format=" + format +
            ";keep_count=" + keepCount;
        Broadcast("get_config",
                  {{"config", wstring_to_utf8(config.configId)},
                   {"name", config.name},
                   {"backup_mode", mode},
                   {"format", format},
                   {"keep_count", keepCount}},
                  context);
        return ok({{"data", data}});
    }

    if (request.command == "HANDSHAKE_RESPONSE") {
        const std::string version = request.Get("mod_version");
        if (version.empty()) {
            return error("Missing mod_version.");
        }
        auto& mod = g_appState.knotLinkMod;
        mod.modDetected = true;
        mod.modVersion = version;
        mod.versionCompatible = KnotLinkModInfo::IsVersionCompatible(
            version, KnotLinkModInfo::MIN_MOD_VERSION);
        mod.notifyFlag(&KnotLinkModInfo::handshakeReceived);
        return ok({
            {"compatible", mod.versionCompatible.load() ? "true" : "false"},
            {"minimum_mod_version", KnotLinkModInfo::MIN_MOD_VERSION}});
    }
    if (request.command == "WORLD_SAVED") {
        g_appState.knotLinkMod.notifyFlag(&KnotLinkModInfo::worldSaveComplete);
        return ok({{"message", "World save acknowledged."}});
    }
    if (request.command == "WORLD_SAVE_AND_EXIT_COMPLETE") {
        if (g_appState.hotkeyRestoreState != HotRestoreState::WAITING_FOR_MOD) {
            return error("MineBackup is not waiting for world save-and-exit.");
        }
        g_appState.isRespond = true;
        g_appState.knotLinkMod.notifyFlag(
            &KnotLinkModInfo::worldSaveAndExitComplete);
        return ok({{"message", "World save-and-exit acknowledged."}});
    }
    if (request.command == "REJOIN_RESULT") {
        const std::string result = LowerAscii(request.Get("result"));
        if (result != "success" && result != "failure") {
            return error("result must be success or failure.");
        }
        g_appState.knotLinkMod.rejoinSuccess = result == "success";
        g_appState.knotLinkMod.notifyFlag(
            &KnotLinkModInfo::rejoinResponseReceived);
        return ok({{"message", "Rejoin result acknowledged."}});
    }

    auto submit = [&](std::wstring taskName,
                      std::vector<std::wstring> resources,
                      std::function<std::pair<bool, std::string>()> work) {
        Broadcast("command_accepted", {{"command", request.command}}, context);
        const bool queued = TaskCoordinator::Instance().Submit(
            std::move(taskName), std::move(resources),
            [this, context, command = request.command, work = std::move(work)](
                std::stop_token) mutable {
                ContextScope scope(context);
                Broadcast("command_started", {{"command", command}}, context);
                try {
                    const auto [success, message] = work();
                    Broadcast(
                        success ? "command_completed" : "command_failed",
                        {{"command", command}, {"message", message}}, context);
                } catch (const std::exception& exception) {
                    Broadcast("command_failed",
                              {{"command", command}, {"message", exception.what()}},
                              context);
                } catch (...) {
                    Broadcast("command_failed",
                              {{"command", command},
                               {"message", "Unknown task failure."}},
                              context);
                }
            });
        if (!queued) {
            Broadcast("command_failed",
                      {{"command", request.command},
                       {"message", "Application is shutting down."}},
                      context);
            return error("Application is shutting down.");
        }
        return ok({{"message", "Command accepted."}});
    };

    if (request.command == "BACKUP") {
        std::string targetError;
        auto folder = ResolveFolder(request, targetError);
        if (!folder.has_value()) {
            return error(targetError);
        }
        auto config =
            ApplyBackupOverrides(request, folder->config, targetError);
        if (!config.has_value()) {
            return error(targetError);
        }
        folder->config = std::move(*config);
        const MyFolder target = folder->ToMyFolder();
        const std::wstring comment = utf8_to_wstring(request.Get("comment"));
        return submit(
            L"KnotLink v2 backup",
            {TaskCoordinator::WorldResourceKey(target.config.configId, target.path)},
            [target, comment] {
                const BackupOutcome outcome = DoBackup(target, comment);
                switch (outcome) {
                    case BackupOutcome::Created:
                        return std::pair{true, std::string("Backup created.")};
                    case BackupOutcome::NoChanges:
                        return std::pair{true, std::string("No changes.")};
                    case BackupOutcome::Rejected:
                        return std::pair{false, std::string("Backup rejected.")};
                    case BackupOutcome::Failed:
                        return std::pair{false, std::string("Backup failed.")};
                }
                return std::pair{false, std::string("Backup failed.")};
            });
    }
    if (request.command == "BACKUP_ALL") {
        const auto resolved = ResolveConfig(request.Get("config_id"));
        if (!resolved.has_value()) {
            return error("Unknown or missing config_id.");
        }
        std::string overrideError;
        const auto overridden =
            ApplyBackupOverrides(request, resolved->second, overrideError);
        if (!overridden.has_value()) {
            return error(overrideError);
        }
        std::vector<MyFolder> targets;
        std::vector<std::wstring> resources;
        for (std::size_t index = 0; index < overridden->worlds.size(); ++index) {
            const auto& world = overridden->worlds[index];
            const std::wstring path =
                JoinPath(overridden->saveRoot, world.first).wstring();
            targets.push_back({
                path, world.first, world.second, *overridden,
                resolved->first, static_cast<int>(index)});
            resources.push_back(
                TaskCoordinator::WorldResourceKey(overridden->configId, path));
        }
        const std::wstring comment = utf8_to_wstring(request.Get("comment"));
        return submit(
            L"KnotLink v2 backup all", std::move(resources),
            [this, context, targets = std::move(targets), comment] {
                Broadcast("backup_all_started", {}, context);
                int created = 0;
                int unchanged = 0;
                int failed = 0;
                for (const auto& target : targets) {
                    switch (DoBackup(target, comment)) {
                        case BackupOutcome::Created: ++created; break;
                        case BackupOutcome::NoChanges: ++unchanged; break;
                        case BackupOutcome::Failed:
                        case BackupOutcome::Rejected: ++failed; break;
                    }
                }
                const bool success = failed == 0;
                Broadcast(
                    success ? "backup_all_completed" : "backup_all_failed",
                    {{"created", std::to_string(created)},
                     {"unchanged", std::to_string(unchanged)},
                     {"failed", std::to_string(failed)}},
                    context);
                return std::pair{
                    success,
                    "created=" + std::to_string(created) +
                        ", unchanged=" + std::to_string(unchanged) +
                        ", failed=" + std::to_string(failed)};
            });
    }
    if (request.command == "RESTORE") {
        std::string targetError;
        const auto folder = ResolveFolder(request, targetError);
        if (!folder.has_value()) {
            return error(targetError);
        }
        std::wstring backupFile = utf8_to_wstring(request.Get("file"));
        if (backupFile.empty()) {
            const auto latest = LatestBackup(*folder);
            if (!latest.has_value()) {
                return error("No backup is available for the selected folder.");
            }
            backupFile = *latest;
        }
        const std::string mode = LowerAscii(request.Get("mode", "clean"));
        if (mode != "overwrite" && mode != "clean") {
            return error("mode must be overwrite or clean.");
        }
        if (mode == "clean") {
            HistoryEntry entry;
            if (TryGetHistoryEntry(
                    folder->configIndex, folder->folderName, backupFile, entry)
                && entry.isPartialBackup) {
                const auto confirmed = ParseBoolean(
                    request.Get("confirm_partial_clean", "false"));
                if (!confirmed.has_value() || !*confirmed) {
                    return error(
                        "Clean restore from a partial backup requires "
                        "confirm_partial_clean=true.");
                }
            }
        }
        std::vector<std::wstring> restoreWhitelist;
        if (request.Has("restore_whitelist")) {
            for (const auto& item : KnotLinkKeyValueCodec::DecodeList(
                     request.GetEncoded("restore_whitelist"))) {
                restoreWhitelist.push_back(utf8_to_wstring(item));
            }
        }
        const auto currentSave =
            ParseBoolean(request.Get("current_save", "false")).value_or(false);
        if (currentSave) {
            const MyFolder target = folder->ToMyFolder();
            const std::string requestId = context->metadata.requestId;
            return submit(
                L"KnotLink v2 current restore",
                {TaskCoordinator::WorldResourceKey(
                    target.config.configId, target.path)},
                [target, backupFile, mode,
                 restoreWhitelist = std::move(restoreWhitelist), requestId] {
                    HotRestoreState expected = HotRestoreState::IDLE;
                    if (!g_appState.hotkeyRestoreState.compare_exchange_strong(
                            expected, HotRestoreState::WAITING_FOR_MOD)) {
                        return std::pair{
                            false,
                            std::string("A hot restore is already in progress.")};
                    }
                    g_appState.isRespond = false;
                    const bool modAvailable = PerformModHandshake(
                        "restore", wstring_to_utf8(target.name), 3000, requestId);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (!modAvailable) {
                        g_appState.hotkeyRestoreState = HotRestoreState::IDLE;
                        g_appState.isRespond = false;
                        return std::pair{
                            false,
                            std::string(
                                "A compatible linkage mod is required for "
                                "current-world restore.")};
                    }
                    const bool restored = DoHotRestore(
                        target, false, backupFile,
                        mode == "clean" ? 0 : 1, &restoreWhitelist, "", requestId);
                    return std::pair{
                        restored,
                        restored
                            ? std::string("Current-world restore completed.")
                            : std::string("Current-world restore failed.")};
                });
        }
        const Config config = folder->config;
        const std::wstring worldName = folder->folderName;
        return submit(
            L"KnotLink v2 restore",
            {TaskCoordinator::WorldResourceKey(config.configId, folder->folderPath)},
            [config, worldName, backupFile, mode,
             restoreWhitelist = std::move(restoreWhitelist)] {
                const bool restored = DoRestore(
                    config, worldName, backupFile,
                    mode == "clean" ? 0 : 1, "", &restoreWhitelist);
                return std::pair{
                    restored,
                    restored ? std::string("Restore completed.")
                             : std::string("Restore failed.")};
            });
    }
    if (request.command == "MARK_IMPORTANT") {
        std::string targetError;
        const auto folder = ResolveFolder(request, targetError);
        if (!folder.has_value()) {
            return error(targetError);
        }
        const std::wstring file = utf8_to_wstring(request.Get("file"));
        const auto important = ParseBoolean(request.Get("important"));
        if (file.empty() || !important.has_value()) {
            return error("file and important=true|false are required.");
        }
        HistoryEntry entry;
        if (!TryGetHistoryEntry(
                folder->configIndex, folder->folderName, file, entry)) {
            return error("Backup history entry was not found.");
        }
        (void)UpdateHistoryEntry(
            folder->configIndex,
            folder->folderName,
            file,
            [important](HistoryEntry& value) { value.isImportant = *important; });
        auto fields = EventTargetFields(*folder);
        fields.emplace_back("file", wstring_to_utf8(file));
        fields.emplace_back("important", *important ? "true" : "false");
        Broadcast("mark_important", fields, context);
        return ok({{"message", "Importance flag updated."}});
    }

    return error("Unknown command.", {{"code", "unknown_command"}});
}

KnotLinkService& GetKnotLinkService() {
    static KnotLinkService service;
    return service;
}

} // namespace minebackup::knotlink
