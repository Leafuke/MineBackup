#include "Logging.h"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace minebackup::logging {
namespace {

constexpr std::size_t kSessionCapacity = 20'000;
constexpr std::size_t kStartupCapacity = 256;
constexpr std::size_t kAsyncQueueCapacity = 8'192;
constexpr std::size_t kMaximumLineBytes = 64 * 1024;
constexpr std::size_t kRotatingFileBytes = 10 * 1024 * 1024;
constexpr std::size_t kRotatingArchiveCount = 4;
constexpr std::string_view kTruncationMarker = " ...[truncated at 64 KiB]";

thread_local std::vector<LogField> g_context;

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return spdlog::level::trace;
    case LogLevel::Debug: return spdlog::level::debug;
    case LogLevel::Info: return spdlog::level::info;
    case LogLevel::Warning: return spdlog::level::warn;
    case LogLevel::Error: return spdlog::level::err;
    case LogLevel::Critical: return spdlog::level::critical;
    }
    return spdlog::level::info;
}

bool IsEnabledForFile(LogLevel level, LogFileLevel fileLevel) {
    switch (fileLevel) {
    case LogFileLevel::Off: return false;
    case LogFileLevel::Info: return level >= LogLevel::Info;
    case LogFileLevel::Debug: return level >= LogLevel::Debug;
    }
    return false;
}

std::string MakeSessionId() {
    try {
        std::array<unsigned char, 16> bytes{};
        std::random_device random;
        for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
        bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
        bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
        std::ostringstream stream;
        stream << std::hex << std::setfill('0');
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            stream << std::setw(2) << static_cast<unsigned int>(bytes[index]);
            if (index == 3 || index == 5 || index == 7 || index == 9) stream << '-';
        }
        return stream.str();
    } catch (...) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        std::ostringstream stream;
        stream << "fallback-" << std::hex << ticks;
        return stream.str();
    }
}

std::string CurrentThreadId() {
    std::ostringstream stream;
    stream << std::this_thread::get_id();
    return stream.str();
}

std::string SourceBasename(const char* file) {
    if (file == nullptr || *file == '\0') return {};
    const std::string_view value(file);
    const auto separator = value.find_last_of("/\\");
    return std::string(separator == std::string_view::npos ? value : value.substr(separator + 1));
}

std::string StripAnsiAndControls(std::string_view input) {
    enum class EscapeState { Text, Escape, Csi, Osc, OscEscape };
    EscapeState state = EscapeState::Text;
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto byte = static_cast<unsigned char>(input[index]);
        switch (state) {
        case EscapeState::Text:
            if (byte == 0x1b) {
                state = EscapeState::Escape;
            } else if (byte == '\r') {
                output.push_back('\n');
                if (index + 1 < input.size() && input[index + 1] == '\n') ++index;
            } else if (byte == '\n') {
                output.push_back('\n');
            } else if (byte == '\t') {
                output.push_back(' ');
            } else if (byte >= 0x20 && byte != 0x7f) {
                output.push_back(static_cast<char>(byte));
            }
            break;
        case EscapeState::Escape:
            if (byte == '[') state = EscapeState::Csi;
            else if (byte == ']') state = EscapeState::Osc;
            else state = EscapeState::Text;
            break;
        case EscapeState::Csi:
            if (byte >= 0x40 && byte <= 0x7e) state = EscapeState::Text;
            break;
        case EscapeState::Osc:
            if (byte == 0x07) state = EscapeState::Text;
            else if (byte == 0x1b) state = EscapeState::OscEscape;
            break;
        case EscapeState::OscEscape:
            state = byte == '\\' ? EscapeState::Text : EscapeState::Osc;
            break;
        }
    }
    return output;
}

std::size_t Utf8PrefixBoundary(std::string_view value, std::size_t maximum) {
    if (value.size() <= maximum) return value.size();
    std::size_t boundary = maximum;
    while (boundary > 0
        && (static_cast<unsigned char>(value[boundary]) & 0xc0) == 0x80) {
        --boundary;
    }
    return boundary;
}

std::string TruncateLine(std::string_view line) {
    if (line.size() <= kMaximumLineBytes) return std::string(line);
    const auto payloadLimit = kMaximumLineBytes - kTruncationMarker.size();
    const auto boundary = Utf8PrefixBoundary(line, payloadLimit);
    std::string output(line.substr(0, boundary));
    output.append(kTruncationMarker);
    return output;
}

std::vector<std::string> NormalizeLines(std::string_view message) {
    const auto cleaned = StripAnsiAndControls(message);
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= cleaned.size()) {
        const auto newline = cleaned.find('\n', begin);
        const auto end = newline == std::string::npos ? cleaned.size() : newline;
        lines.push_back(TruncateLine(std::string_view(cleaned).substr(begin, end - begin)));
        if (newline == std::string::npos) break;
        begin = newline + 1;
        if (begin == cleaned.size()) break;
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

std::string EscapeField(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '\\': escaped.append("\\\\"); break;
        case '=': escaped.append("\\="); break;
        case ';': escaped.append("\\;"); break;
        case ']': escaped.append("\\]"); break;
        case '"': escaped.append("\\\""); break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

const char* FileLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Critical: return "CRITICAL";
    }
    return "INFO";
}

const char* FileCategoryName(LogCategory category) {
    switch (category) {
    case LogCategory::Application: return "Application";
    case LogCategory::Backup: return "Backup";
    case LogCategory::Restore: return "Restore";
    case LogCategory::History: return "History";
    case LogCategory::Cloud: return "Cloud";
    case LogCategory::Task: return "Task";
    case LogCategory::Process: return "Process";
    case LogCategory::Network: return "Network";
    case LogCategory::KnotLink: return "KnotLink";
    case LogCategory::Migration: return "Migration";
    case LogCategory::Platform: return "Platform";
    case LogCategory::Validation: return "Validation";
    case LogCategory::Session: return "Session";
    }
    return "Application";
}

std::string FilePayload(const LogRecord& record, bool includeExtendedFields) {
    std::ostringstream stream;
    stream << '[' << FileLevelName(record.level) << "] ["
           << FileCategoryName(record.category) << "] " << record.message;
    if (!includeExtendedFields) return stream.str();

    stream << " | event=" << EscapeField(record.eventId)
           << " thread=" << EscapeField(record.threadId);
    if (!record.context.empty()) {
        stream << " context=[";
        bool first = true;
        for (const auto& field : record.context) {
            if (!first) stream << ';';
            first = false;
            stream << EscapeField(field.key) << '=' << EscapeField(field.value);
        }
        stream << ']';
    }
    if (!record.sourceFile.empty()) {
        stream << " source=" << EscapeField(record.sourceFile) << ':' << record.sourceLine;
    }
    return stream.str();
}

void FallbackWrite(std::string_view message) noexcept {
    try {
        std::fwrite("[MineBackup logging] ", 1, 21, stderr);
        std::fwrite(message.data(), 1, message.size(), stderr);
        std::fwrite("\n", 1, 1, stderr);
        std::fflush(stderr);
#ifdef _WIN32
        std::string debugMessage("[MineBackup logging] ");
        debugMessage.append(message);
        debugMessage.push_back('\n');
        OutputDebugStringA(debugMessage.c_str());
#endif
    } catch (...) {
    }
}

class LoggingService {
public:
    LoggingService() : sessionId_(MakeSessionId()) {}

    void InitializeService(const InitializeOptions& options) noexcept {
        bool reportPreviousAbnormalSession = false;
        try {
            {
                std::lock_guard lock(dispatchMutex_);
                logsDirectory_ = options.logsDirectory;
                activeSessionPath_ = logsDirectory_ / ".active-session";
                std::error_code markerError;
                previousSessionAbnormal_ = std::filesystem::is_regular_file(
                    activeSessionPath_, markerError) && !markerError;
                reportPreviousAbnormalSession = previousSessionAbnormal_;
                fileLevel_ = options.fileLevel;
                consoleEnabled_ = options.consoleEnabled;
                applicationVersion_ = options.applicationVersion;
                if (!threadPoolInitialized_) {
                    spdlog::init_thread_pool(kAsyncQueueCapacity, 1);
                    threadPoolInitialized_ = true;
                    spdlog::flush_every(std::chrono::seconds(1));
                }
                initialized_ = true;
                RebuildBackendLocked();
                UpdateActiveSessionMarkerLocked();
                if (logger_) {
                    for (const auto& record : startupRecords_) {
                        DispatchToBackendLocked(*record);
                    }
                }
                if (fileBackendActive_.load(std::memory_order_acquire)
                    && !sessionBoundaryWritten_) {
                    AppendSessionBoundaryLocked(true);
                }
                startupRecords_.clear();
            }
            if (reportPreviousAbnormalSession) {
                WriteRecord(LogLevel::Warning, LogCategory::Session,
                    "logging.previous_session_abnormal",
                    "The previous MineBackup session did not shut down cleanly.", {});
            }
        } catch (const std::exception& error) {
            SetBackendError(error.what());
        } catch (...) {
            SetBackendError("unknown logging initialization failure");
        }
    }

    void SetFileLevelValue(LogFileLevel level) noexcept {
        try {
            std::lock_guard lock(dispatchMutex_);
            fileLevel_ = level;
            if (initialized_) {
                RebuildBackendLocked();
                UpdateActiveSessionMarkerLocked();
                if (fileBackendActive_.load(std::memory_order_acquire)
                    && !sessionBoundaryWritten_) {
                    AppendSessionBoundaryLocked(true);
                }
            }
        } catch (const std::exception& error) {
            SetBackendError(error.what());
        } catch (...) {
            SetBackendError("unknown file logging reconfiguration failure");
        }
    }

    void SetConsoleEnabledValue(bool enabled) noexcept {
        try {
            std::lock_guard lock(dispatchMutex_);
            consoleEnabled_ = enabled;
            if (initialized_) RebuildBackendLocked();
        } catch (const std::exception& error) {
            SetBackendError(error.what());
        } catch (...) {
            SetBackendError("unknown console logging reconfiguration failure");
        }
    }

    void ShutdownService() noexcept {
        try {
            {
                std::lock_guard lock(dispatchMutex_);
                if (logger_ && sessionBoundaryWritten_) {
                    AppendSessionBoundaryLocked(false);
                }
                if (logger_) logger_->flush();
                logger_.reset();
                if (threadPoolInitialized_) {
                    spdlog::shutdown();
                    threadPoolInitialized_ = false;
                }
                initialized_ = false;
                fileBackendActive_.store(false, std::memory_order_release);
                RemoveActiveSessionMarkerLocked();
                sessionBoundaryWritten_ = false;
            }
        } catch (const std::exception& error) {
            SetBackendError(error.what());
        } catch (...) {
            SetBackendError("unknown logging shutdown failure");
        }
    }

    void WriteRecord(LogLevel level, LogCategory category, std::string_view eventId,
        std::string_view message, SourceLocation source) noexcept {
        try {
            auto lines = NormalizeLines(message);
            const auto timestamp = std::chrono::system_clock::now();
            const auto threadId = CurrentThreadId();
            const auto sourceFile = SourceBasename(source.file);
            auto context = g_context;
            for (auto& field : context) {
                field.key = StripAnsiAndControls(field.key);
                field.value = StripAnsiAndControls(field.value);
            }

            std::lock_guard lock(dispatchMutex_);
            for (auto& line : lines) {
                auto record = AppendRecordLocked(level, category,
                    StripAnsiAndControls(eventId), std::move(line), sourceFile,
                    source.line, timestamp, threadId, context);
                if (!initialized_) {
                    startupRecords_.push_back(record);
                    if (startupRecords_.size() > kStartupCapacity) startupRecords_.pop_front();
                } else {
                    DispatchToBackendLocked(*record);
                }
            }
        } catch (const std::exception& error) {
            FallbackWrite(error.what());
        } catch (...) {
            FallbackWrite("unknown logging write failure");
        }
    }

    LogReadResult Read(std::uint64_t sequence) noexcept {
        LogReadResult result;
        try {
            std::lock_guard lock(dispatchMutex_);
            result.latestSequence = latestSequence_;
            result.evictedCount = evictedCount_;
            result.oldestAvailableSequence = records_.empty() ? 0 : records_.front()->sequence;
            result.requestedSequenceWasEvicted = sequence != 0
                && result.oldestAvailableSequence != 0
                && sequence + 1 < result.oldestAvailableSequence;
            const auto effectiveSequence = result.requestedSequenceWasEvicted
                ? result.oldestAvailableSequence - 1 : sequence;
            for (const auto& record : records_) {
                if (record->sequence > effectiveSequence) result.records.push_back(record);
            }
        } catch (...) {
        }
        return result;
    }

    LoggingStatus Status() noexcept {
        LoggingStatus status;
        try {
            {
                std::lock_guard lock(dispatchMutex_);
                status.fileLevel = fileLevel_;
                status.initialized = initialized_;
                status.consoleEnabled = consoleEnabled_;
                status.retainedCount = records_.size();
                status.evictedCount = evictedCount_;
                status.oldestAvailableSequence = records_.empty() ? 0 : records_.front()->sequence;
                status.latestSequence = latestSequence_;
                status.sessionId = sessionId_;
                status.logsDirectory = logsDirectory_;
                status.previousSessionAbnormal = previousSessionAbnormal_;
            }
            status.fileBackendActive = fileBackendActive_.load(std::memory_order_acquire);
            {
                std::lock_guard lock(statusMutex_);
                status.lastBackendError = lastBackendError_;
            }
        } catch (...) {
        }
        return status;
    }

private:
    std::shared_ptr<const LogRecord> AppendRecordLocked(
        LogLevel level,
        LogCategory category,
        std::string eventId,
        std::string message,
        std::string sourceFile,
        std::uint_least32_t sourceLine,
        std::chrono::system_clock::time_point timestamp,
        std::string threadId,
        std::vector<LogField> context) {
        auto record = std::make_shared<LogRecord>();
        record->sequence = ++latestSequence_;
        record->timestamp = timestamp;
        record->level = level;
        record->category = category;
        record->eventId = std::move(eventId);
        record->message = std::move(message);
        record->sessionId = sessionId_;
        record->threadId = std::move(threadId);
        record->sourceFile = std::move(sourceFile);
        record->sourceLine = sourceLine;
        record->context = std::move(context);
        records_.push_back(record);
        if (records_.size() > kSessionCapacity) {
            records_.pop_front();
            ++evictedCount_;
        }
        return record;
    }

    std::string ShortSessionId() const {
        return sessionId_.substr(0, std::min<std::size_t>(8, sessionId_.size()));
    }

    void AppendSessionBoundaryLocked(bool starting) {
        std::string message;
        if (starting) {
            message = "===== MineBackup";
            if (!applicationVersion_.empty()) {
                message.append(" ").append(applicationVersion_);
            }
            message.append(" session started (").append(ShortSessionId()).append(") =====");
        } else {
            message = "===== MineBackup session ended normally (";
            message.append(ShortSessionId()).append(") =====");
        }
        const auto record = AppendRecordLocked(
            LogLevel::Info, LogCategory::Session,
            starting ? "logging.session.started" : "logging.session.ended",
            std::move(message), {}, 0, std::chrono::system_clock::now(),
            CurrentThreadId(), {});
        DispatchToBackendLocked(*record);
        sessionBoundaryWritten_ = starting;
    }

    void RebuildBackendLocked() {
        if (logger_) {
            logger_->flush();
            logger_.reset();
            spdlog::shutdown();
            threadPoolInitialized_ = false;
            spdlog::init_thread_pool(kAsyncQueueCapacity, 1);
            threadPoolInitialized_ = true;
            spdlog::flush_every(std::chrono::seconds(1));
        }

        std::vector<spdlog::sink_ptr> sinks;
        bool fileActive = false;
        if (fileLevel_ != LogFileLevel::Off && !logsDirectory_.empty()) {
            try {
                std::filesystem::create_directories(logsDirectory_);
#ifdef _WIN32
                const spdlog::filename_t filename = (logsDirectory_ / L"minebackup.log").wstring();
#else
                const spdlog::filename_t filename = (logsDirectory_ / "minebackup.log").string();
#endif
                auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    filename, kRotatingFileBytes, kRotatingArchiveCount, false);
                sink->set_level(fileLevel_ == LogFileLevel::Debug
                    ? spdlog::level::debug : spdlog::level::info);
                sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] %v");
                sinks.push_back(std::move(sink));
                fileActive = true;
                ClearBackendError();
            } catch (const std::exception& error) {
                SetBackendError(error.what());
            }
        }
        if (consoleEnabled_) {
            auto sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
            sink->set_level(spdlog::level::trace);
            sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] %v");
            sinks.push_back(std::move(sink));
        }

        if (sinks.empty()) {
            logger_.reset();
            fileBackendActive_.store(false, std::memory_order_release);
            return;
        }

        const std::string loggerName = "minebackup-logging-" + std::to_string(++loggerGeneration_);
        auto logger = std::make_shared<spdlog::async_logger>(
            loggerName, sinks.begin(), sinks.end(), spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
        logger->set_level(spdlog::level::trace);
        logger->set_error_handler([this](const std::string& message) {
            fileBackendActive_.store(false, std::memory_order_release);
            SetBackendError(message);
        });
        spdlog::register_logger(logger);
        logger_ = std::move(logger);
        fileBackendActive_.store(fileActive, std::memory_order_release);
    }

    void DispatchToBackendLocked(const LogRecord& record) {
        if (!logger_) return;
        const bool fileAccepts = IsEnabledForFile(record.level, fileLevel_);
        if (!fileAccepts && !consoleEnabled_) return;
        logger_->log(ToSpdlogLevel(record.level),
            FilePayload(record, fileLevel_ == LogFileLevel::Debug));
        if (record.level >= LogLevel::Error) logger_->flush();
    }

    void UpdateActiveSessionMarkerLocked() {
        if (!fileBackendActive_.load(std::memory_order_acquire)) {
            RemoveActiveSessionMarkerLocked();
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(logsDirectory_, error);
        if (error) {
            SetBackendError(error.message());
            return;
        }
        std::ofstream marker(activeSessionPath_, std::ios::binary | std::ios::trunc);
        if (!marker.is_open()) {
            SetBackendError("could not create the active logging session marker");
            return;
        }
        marker << sessionId_ << '\n';
        if (!marker.good()) {
            SetBackendError("could not write the active logging session marker");
            return;
        }
    }

    void RemoveActiveSessionMarkerLocked() noexcept {
        if (activeSessionPath_.empty()) return;
        std::error_code error;
        std::filesystem::remove(activeSessionPath_, error);
        if (error) SetBackendError(error.message());
    }

    void SetBackendError(std::string_view error) noexcept {
        {
            std::lock_guard lock(statusMutex_);
            lastBackendError_ = std::string(error);
        }
        FallbackWrite(error);
    }

    void ClearBackendError() noexcept {
        std::lock_guard lock(statusMutex_);
        lastBackendError_.clear();
    }

    std::mutex dispatchMutex_;
    std::mutex statusMutex_;
    std::deque<std::shared_ptr<const LogRecord>> records_;
    std::deque<std::shared_ptr<const LogRecord>> startupRecords_;
    std::shared_ptr<spdlog::logger> logger_;
    std::filesystem::path logsDirectory_;
    std::filesystem::path activeSessionPath_;
    std::string sessionId_;
    std::string applicationVersion_;
    std::string lastBackendError_;
    std::uint64_t latestSequence_ = 0;
    std::uint64_t evictedCount_ = 0;
    std::uint64_t loggerGeneration_ = 0;
    LogFileLevel fileLevel_ = LogFileLevel::Info;
    bool initialized_ = false;
    bool consoleEnabled_ = false;
    bool threadPoolInitialized_ = false;
    bool previousSessionAbnormal_ = false;
    bool sessionBoundaryWritten_ = false;
    std::atomic_bool fileBackendActive_ = false;
};

LoggingService& Service() {
    static LoggingService service;
    return service;
}

LogLevel LegacyLevel(std::string_view message) {
    std::string prefix(message.substr(0, std::min<std::size_t>(message.size(), 32)));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (prefix.find("[error]") != std::string::npos
        || prefix.find("error:") != std::string::npos) {
        return LogLevel::Error;
    }
    if (prefix.find("[warning]") != std::string::npos
        || prefix.find("[warn]") != std::string::npos
        || prefix.find("warning:") != std::string::npos) {
        return LogLevel::Warning;
    }
    return LogLevel::Info;
}

} // namespace

ScopedLogContext::ScopedLogContext(std::initializer_list<LogField> fields) noexcept
    : previousSize_(g_context.size()) {
    try {
        g_context.insert(g_context.end(), fields.begin(), fields.end());
    } catch (...) {
    }
}

ScopedLogContext::ScopedLogContext(std::vector<LogField> fields) noexcept
    : previousSize_(g_context.size()) {
    try {
        g_context.insert(g_context.end(),
            std::make_move_iterator(fields.begin()), std::make_move_iterator(fields.end()));
    } catch (...) {
    }
}

ScopedLogContext::~ScopedLogContext() {
    if (previousSize_ <= g_context.size()) g_context.resize(previousSize_);
}

void Initialize(const InitializeOptions& options) noexcept {
    Service().InitializeService(options);
}

void SetFileLevel(LogFileLevel level) noexcept {
    Service().SetFileLevelValue(level);
}

void SetConsoleEnabled(bool enabled) noexcept {
    Service().SetConsoleEnabledValue(enabled);
}

void Shutdown() noexcept {
    Service().ShutdownService();
}

LogReadResult ReadAfter(std::uint64_t sequence) noexcept {
    return Service().Read(sequence);
}

LoggingStatus GetStatus() noexcept {
    return Service().Status();
}

const char* ToString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    case LogLevel::Critical: return "critical";
    }
    return "info";
}

const char* ToString(LogCategory category) noexcept {
    switch (category) {
    case LogCategory::Application: return "application";
    case LogCategory::Backup: return "backup";
    case LogCategory::Restore: return "restore";
    case LogCategory::History: return "history";
    case LogCategory::Cloud: return "cloud";
    case LogCategory::Task: return "task";
    case LogCategory::Process: return "process";
    case LogCategory::Network: return "network";
    case LogCategory::KnotLink: return "knotlink";
    case LogCategory::Migration: return "migration";
    case LogCategory::Platform: return "platform";
    case LogCategory::Validation: return "validation";
    case LogCategory::Session: return "session";
    }
    return "application";
}

const char* ToString(LogFileLevel level) noexcept {
    switch (level) {
    case LogFileLevel::Off: return "off";
    case LogFileLevel::Info: return "info";
    case LogFileLevel::Debug: return "debug";
    }
    return "info";
}

LogLevel ParseLogLevel(std::string_view value, bool* valid) noexcept {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    const std::array<std::pair<std::string_view, LogLevel>, 6> levels{{
        {"trace", LogLevel::Trace},
        {"debug", LogLevel::Debug},
        {"info", LogLevel::Info},
        {"warning", LogLevel::Warning},
        {"error", LogLevel::Error},
        {"critical", LogLevel::Critical},
    }};
    for (const auto& [name, level] : levels) {
        if (normalized == name) {
            if (valid) *valid = true;
            return level;
        }
    }
    if (valid) *valid = false;
    return LogLevel::Info;
}

LogFileLevel ParseFileLevel(std::string_view value, bool* valid) noexcept {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (normalized == "off") {
        if (valid) *valid = true;
        return LogFileLevel::Off;
    }
    if (normalized == "info") {
        if (valid) *valid = true;
        return LogFileLevel::Info;
    }
    if (normalized == "debug") {
        if (valid) *valid = true;
        return LogFileLevel::Debug;
    }
    if (valid) *valid = false;
    return LogFileLevel::Info;
}

LogFileLevelResolution ResolveFileLevel(
    std::optional<std::string_view> configuredValue,
    std::optional<bool> legacyAutoLog) noexcept {
    LogFileLevelResolution result;
    if (configuredValue) {
        bool valid = false;
        result.level = ParseFileLevel(*configuredValue, &valid);
        result.invalidConfiguredValue = !valid;
        return result;
    }
    if (legacyAutoLog) {
        result.level = *legacyAutoLog
            ? LogFileLevel::Info : LogFileLevel::Off;
        result.usedLegacyAutoLog = true;
    }
    return result;
}

void Write(LogLevel level, LogCategory category, std::string_view eventId,
    std::string_view message, SourceLocation source) noexcept {
    Service().WriteRecord(level, category, eventId, message, source);
}

void LogRaw(LogCategory category, std::string_view eventId, std::string_view output,
    LogLevel level, SourceLocation source) noexcept {
    Write(level, category, eventId, output, source);
}

void LogLegacyMessage(std::string_view message, SourceLocation source) noexcept {
    Write(LegacyLevel(message), LogCategory::Application, "legacy.console", message, source);
}

void ReportFormatError(LogCategory category, std::string_view eventId,
    std::string_view error, SourceLocation source) noexcept {
    std::string message = "Failed to format log event '";
    message.append(eventId);
    message.append("': ");
    message.append(error);
    Write(LogLevel::Error, category, "logging.format_error", message, source);
}

} // namespace minebackup::logging
