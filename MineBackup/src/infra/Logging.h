#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/fmt/bundled/format.h>
#include <spdlog/fmt/bundled/printf.h>

namespace minebackup::logging {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

enum class LogCategory {
    Application,
    Backup,
    Restore,
    History,
    Cloud,
    Task,
    Process,
    Network,
    KnotLink,
    Migration,
    Platform,
    Validation,
};

enum class LogFileLevel {
    Off,
    Info,
    Debug,
};

struct LogField {
    std::string key;
    std::string value;
};

struct SourceLocation {
    const char* file = "";
    std::uint_least32_t line = 0;
};

struct LogRecord {
    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point timestamp;
    LogLevel level = LogLevel::Info;
    LogCategory category = LogCategory::Application;
    std::string eventId;
    std::string message;
    std::string sessionId;
    std::string threadId;
    std::string sourceFile;
    std::uint_least32_t sourceLine = 0;
    std::vector<LogField> context;
};

struct InitializeOptions {
    std::filesystem::path logsDirectory;
    LogFileLevel fileLevel = LogFileLevel::Info;
    bool consoleEnabled = false;
};

struct LogReadResult {
    std::vector<std::shared_ptr<const LogRecord>> records;
    std::uint64_t oldestAvailableSequence = 0;
    std::uint64_t latestSequence = 0;
    std::uint64_t evictedCount = 0;
    bool requestedSequenceWasEvicted = false;
};

struct LoggingStatus {
    LogFileLevel fileLevel = LogFileLevel::Off;
    bool initialized = false;
    bool fileBackendActive = false;
    bool consoleEnabled = false;
    bool previousSessionAbnormal = false;
    std::string lastBackendError;
    std::uint64_t retainedCount = 0;
    std::uint64_t evictedCount = 0;
    std::uint64_t oldestAvailableSequence = 0;
    std::uint64_t latestSequence = 0;
    std::string sessionId;
    std::filesystem::path logsDirectory;
};

struct LogFileLevelResolution {
    LogFileLevel level = LogFileLevel::Info;
    bool usedLegacyAutoLog = false;
    bool invalidConfiguredValue = false;
};

class ScopedLogContext {
public:
    ScopedLogContext(std::initializer_list<LogField> fields) noexcept;
    explicit ScopedLogContext(std::vector<LogField> fields) noexcept;
    ~ScopedLogContext();

    ScopedLogContext(const ScopedLogContext&) = delete;
    ScopedLogContext& operator=(const ScopedLogContext&) = delete;
    ScopedLogContext(ScopedLogContext&&) = delete;
    ScopedLogContext& operator=(ScopedLogContext&&) = delete;

private:
    std::size_t previousSize_ = 0;
};

void Initialize(const InitializeOptions& options) noexcept;
void SetFileLevel(LogFileLevel level) noexcept;
void SetConsoleEnabled(bool enabled) noexcept;
void Shutdown() noexcept;

LogReadResult ReadAfter(std::uint64_t sequence) noexcept;
LoggingStatus GetStatus() noexcept;

const char* ToString(LogLevel level) noexcept;
const char* ToString(LogCategory category) noexcept;
const char* ToString(LogFileLevel level) noexcept;
LogFileLevel ParseFileLevel(std::string_view value, bool* valid = nullptr) noexcept;
LogFileLevelResolution ResolveFileLevel(
    std::optional<std::string_view> configuredValue,
    std::optional<bool> legacyAutoLog) noexcept;

void Write(
    LogLevel level,
    LogCategory category,
    std::string_view eventId,
    std::string_view message,
    SourceLocation source = {}) noexcept;
void LogRaw(
    LogCategory category,
    std::string_view eventId,
    std::string_view output,
    LogLevel level = LogLevel::Debug,
    SourceLocation source = {}) noexcept;
void LogLegacyMessage(std::string_view message, SourceLocation source = {}) noexcept;
void ReportFormatError(
    LogCategory category,
    std::string_view eventId,
    std::string_view error,
    SourceLocation source) noexcept;

template <typename... Args>
void LogFormat(
    LogLevel level,
    LogCategory category,
    std::string_view eventId,
    SourceLocation source,
    fmt::format_string<Args...> format,
    Args&&... args) noexcept {
    try {
        Write(level, category, eventId,
            fmt::format(format, std::forward<Args>(args)...), source);
    } catch (const std::exception& error) {
        ReportFormatError(category, eventId, error.what(), source);
    } catch (...) {
        ReportFormatError(category, eventId, "unknown formatting failure", source);
    }
}

template <typename... Args>
void LogPrintf(
    LogLevel level,
    LogCategory category,
    std::string_view eventId,
    SourceLocation source,
    std::string_view format,
    Args&&... args) noexcept {
    try {
        Write(level, category, eventId,
            fmt::sprintf(fmt::string_view(format.data(), format.size()),
                std::forward<Args>(args)...),
            source);
    } catch (const std::exception& error) {
        ReportFormatError(category, eventId, error.what(), source);
    } catch (...) {
        ReportFormatError(category, eventId, "unknown printf formatting failure", source);
    }
}

} // namespace minebackup::logging

#define MB_LOG_SOURCE ::minebackup::logging::SourceLocation{__FILE__, __LINE__}

#ifndef NDEBUG
#define MB_LOG_TRACE(category, eventId, ...) \
    ::minebackup::logging::LogFormat(::minebackup::logging::LogLevel::Trace, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)
#define MB_LOG_I18N_TRACE(category, eventId, key, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Trace, category, eventId, MB_LOG_SOURCE, L(key) __VA_OPT__(,) __VA_ARGS__)
#else
#define MB_LOG_TRACE(category, eventId, ...) do { } while (false)
#define MB_LOG_I18N_TRACE(category, eventId, key, ...) do { } while (false)
#endif

#define MB_LOG_DEBUG(category, eventId, ...) \
    ::minebackup::logging::LogFormat(::minebackup::logging::LogLevel::Debug, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)
#define MB_LOG_INFO(category, eventId, ...) \
    ::minebackup::logging::LogFormat(::minebackup::logging::LogLevel::Info, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)
#define MB_LOG_WARNING(category, eventId, ...) \
    ::minebackup::logging::LogFormat(::minebackup::logging::LogLevel::Warning, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)
#define MB_LOG_ERROR(category, eventId, ...) \
    ::minebackup::logging::LogFormat(::minebackup::logging::LogLevel::Error, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)
#define MB_LOG_CRITICAL(category, eventId, ...) \
    ::minebackup::logging::LogFormat(::minebackup::logging::LogLevel::Critical, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)

#define MB_LOG_I18N_DEBUG(category, eventId, key, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Debug, category, eventId, MB_LOG_SOURCE, L(key) __VA_OPT__(,) __VA_ARGS__)
#define MB_LOG_I18N_INFO(category, eventId, key, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Info, category, eventId, MB_LOG_SOURCE, L(key) __VA_OPT__(,) __VA_ARGS__)
#define MB_LOG_I18N_WARNING(category, eventId, key, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Warning, category, eventId, MB_LOG_SOURCE, L(key) __VA_OPT__(,) __VA_ARGS__)
#define MB_LOG_I18N_ERROR(category, eventId, key, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Error, category, eventId, MB_LOG_SOURCE, L(key) __VA_OPT__(,) __VA_ARGS__)
#define MB_LOG_I18N_CRITICAL(category, eventId, key, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Critical, category, eventId, MB_LOG_SOURCE, L(key) __VA_OPT__(,) __VA_ARGS__)

#define MB_LOG_PRINTF_DEBUG(category, eventId, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Debug, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)
#define MB_LOG_PRINTF_INFO(category, eventId, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Info, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)
#define MB_LOG_PRINTF_WARNING(category, eventId, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Warning, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)
#define MB_LOG_PRINTF_ERROR(category, eventId, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Error, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)
#define MB_LOG_PRINTF_CRITICAL(category, eventId, ...) \
    ::minebackup::logging::LogPrintf(::minebackup::logging::LogLevel::Critical, category, eventId, MB_LOG_SOURCE, __VA_ARGS__)
