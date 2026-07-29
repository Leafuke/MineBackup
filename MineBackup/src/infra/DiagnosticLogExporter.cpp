#include "DiagnosticLogExporter.h"

#include "Logging.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace minebackup::diagnostics {
namespace {

constexpr std::size_t kMaximumExportedRecords = 20'000;

std::string FormatTimestamp(
    std::chrono::system_clock::time_point timestamp,
    const char* format) {
    const auto time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[64]{};
    std::strftime(buffer, sizeof(buffer), format, &local);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()) % 1000;
    std::ostringstream output;
    output << buffer << '.' << std::setfill('0') << std::setw(3)
           << milliseconds.count();
    return output.str();
}

std::string LocalTimezoneOffset(
    std::chrono::system_clock::time_point timestamp) {
    const auto time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
    std::tm utc{};
#ifdef _WIN32
    localtime_s(&local, &time);
    gmtime_s(&utc, &time);
    const auto localSeconds = _mkgmtime(&local);
    const auto utcSeconds = _mkgmtime(&utc);
#else
    localtime_r(&time, &local);
    gmtime_r(&time, &utc);
    const auto localSeconds = timegm(&local);
    const auto utcSeconds = timegm(&utc);
#endif
    const auto differenceMinutes =
        static_cast<long long>(std::difftime(localSeconds, utcSeconds) / 60);
    const char sign = differenceMinutes < 0 ? '-' : '+';
    const auto absoluteMinutes =
        differenceMinutes < 0 ? -differenceMinutes : differenceMinutes;
    std::ostringstream output;
    output << sign << std::setfill('0') << std::setw(2)
           << absoluteMinutes / 60 << ':' << std::setw(2)
           << absoluteMinutes % 60;
    return output.str();
}

bool EqualsAsciiCaseInsensitive(char left, char right) {
    return std::tolower(static_cast<unsigned char>(left))
        == std::tolower(static_cast<unsigned char>(right));
}

void ReplaceAll(
    std::string& text,
    std::string_view value,
    std::string_view replacement) {
    if (value.empty()) return;
    std::size_t offset = 0;
    while (offset + value.size() <= text.size()) {
#ifdef _WIN32
        const auto match = std::search(
            text.begin() + static_cast<std::ptrdiff_t>(offset), text.end(),
            value.begin(), value.end(), EqualsAsciiCaseInsensitive);
#else
        const auto match = std::search(
            text.begin() + static_cast<std::ptrdiff_t>(offset), text.end(),
            value.begin(), value.end());
#endif
        if (match == text.end()) break;
        const auto position =
            static_cast<std::size_t>(std::distance(text.begin(), match));
        text.replace(position, value.size(), replacement);
        offset = position + replacement.size();
    }
}

std::vector<RedactionRule> NormalizedRules(
    const std::vector<RedactionRule>& input) {
    std::vector<RedactionRule> rules;
    for (const auto& rule : input) {
        if (rule.value.empty()) continue;
        rules.push_back(rule);
        std::string slashVariant = rule.value;
        std::replace(slashVariant.begin(), slashVariant.end(), '\\', '/');
        if (slashVariant != rule.value) {
            rules.push_back({std::move(slashVariant), rule.replacement});
        }
        std::string backslashVariant = rule.value;
        std::replace(backslashVariant.begin(), backslashVariant.end(), '/', '\\');
        if (backslashVariant != rule.value) {
            rules.push_back({std::move(backslashVariant), rule.replacement});
        }
    }
    std::sort(rules.begin(), rules.end(),
        [](const RedactionRule& left, const RedactionRule& right) {
            return left.value.size() > right.value.size();
        });
    rules.erase(std::unique(rules.begin(), rules.end(),
        [](const RedactionRule& left, const RedactionRule& right) {
            return left.value == right.value;
        }), rules.end());
    return rules;
}

bool StartsWithAt(
    std::string_view text,
    std::size_t offset,
    std::string_view prefix) {
    return offset + prefix.size() <= text.size()
        && text.substr(offset, prefix.size()) == prefix;
}

void RedactUrls(std::string& text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::size_t schemeLength = 0;
        if (StartsWithAt(text, offset, "https://")) schemeLength = 8;
        else if (StartsWithAt(text, offset, "http://")) schemeLength = 7;
        else if (StartsWithAt(text, offset, "ftp://")) schemeLength = 6;
        if (schemeLength == 0) {
            ++offset;
            continue;
        }

        const std::size_t authorityStart = offset + schemeLength;
        std::size_t tokenEnd = authorityStart;
        while (tokenEnd < text.size()
            && !std::isspace(static_cast<unsigned char>(text[tokenEnd]))
            && text[tokenEnd] != '"' && text[tokenEnd] != '\''
            && text[tokenEnd] != '<' && text[tokenEnd] != '>'
            && text[tokenEnd] != ')' && text[tokenEnd] != ']'
            && text[tokenEnd] != '}' && text[tokenEnd] != ',') {
            ++tokenEnd;
        }
        const std::size_t authorityEnd =
            text.find_first_of("/?#", authorityStart);
        const std::size_t boundedAuthorityEnd =
            authorityEnd == std::string::npos || authorityEnd > tokenEnd
                ? tokenEnd : authorityEnd;
        const std::size_t at = text.rfind('@', boundedAuthorityEnd);
        if (at != std::string::npos && at >= authorityStart
            && at < boundedAuthorityEnd) {
            text.replace(authorityStart, at - authorityStart, "<userinfo>");
            tokenEnd -= (at - authorityStart) - std::string_view("<userinfo>").size();
        }

        const std::size_t query = text.find('?', authorityStart);
        if (query != std::string::npos && query < tokenEnd) {
            const std::size_t fragment = text.find('#', query + 1);
            const std::size_t queryEnd =
                fragment != std::string::npos && fragment < tokenEnd
                    ? fragment : tokenEnd;
            constexpr std::string_view replacement = "?<query>";
            text.replace(query, queryEnd - query, replacement);
            tokenEnd -= (queryEnd - query) - replacement.size();
        }
        offset = tokenEnd;
    }
}

std::string ContextText(const logging::LogRecord& record) {
    std::string result;
    for (const auto& field : record.context) {
        if (!result.empty()) result.append(";");
        result.append(field.key).append("=").append(field.value);
    }
    return result;
}

std::string RenderRecord(const logging::LogRecord& record) {
    std::ostringstream output;
    output << FormatTimestamp(record.timestamp, "%Y-%m-%dT%H:%M:%S")
           << LocalTimezoneOffset(record.timestamp)
           << " sequence=" << record.sequence
           << " session=" << record.sessionId
           << " level=" << logging::ToString(record.level)
           << " category=" << logging::ToString(record.category)
           << " thread=" << record.threadId
           << " event=" << record.eventId;
    const auto context = ContextText(record);
    if (!context.empty()) output << " context={" << context << '}';
    if (!record.sourceFile.empty()) {
        output << " source=" << record.sourceFile << ':' << record.sourceLine;
    }
    output << " message=" << record.message << '\n';
    return output.str();
}

} // namespace

std::string RedactText(
    std::string_view input,
    const std::vector<RedactionRule>& rules) {
    std::string result(input);
    RedactUrls(result);
    for (const auto& rule : NormalizedRules(rules)) {
        ReplaceAll(result, rule.value, rule.replacement);
    }
    return result;
}

DiagnosticExportResult ExportDiagnostics(
    const DiagnosticExportOptions& options) noexcept {
    DiagnosticExportResult result;
    try {
        std::error_code error;
        std::filesystem::create_directories(options.logsDirectory, error);
        if (error) {
            result.error = "Unable to create diagnostics directory: "
                + error.message();
            return result;
        }

        const auto now = std::chrono::system_clock::now();
        const auto filename =
            "minebackup-diagnostics-" + FormatTimestamp(now, "%Y%m%d-%H%M%S")
                .substr(0, 15) + ".txt";
        result.path = options.logsDirectory / filename;

        const auto status = logging::GetStatus();
        const auto read = logging::ReadAfter(0);
        const std::size_t start =
            read.records.size() > kMaximumExportedRecords
                ? read.records.size() - kMaximumExportedRecords : 0;

        std::ostringstream output;
        output << "MineBackup diagnostics\n"
               << "version=" << options.applicationVersion << '\n'
               << "platform=" << options.platform << '\n'
               << "profile_mode=" << options.profileMode << '\n'
               << "session_id=" << status.sessionId << '\n'
               << "session_initialized=" << (status.initialized ? "true" : "false") << '\n'
               << "previous_session_abnormal="
               << (status.previousSessionAbnormal ? "true" : "false") << '\n'
               << "file_level=" << logging::ToString(status.fileLevel) << '\n'
               << "file_backend_active="
               << (status.fileBackendActive ? "true" : "false") << '\n'
               << "retained_records=" << status.retainedCount << '\n'
               << "evicted_records=" << status.evictedCount << '\n'
               << "exported_records=" << read.records.size() - start << "\n\n";
        for (std::size_t index = start; index < read.records.size(); ++index) {
            output << RenderRecord(*read.records[index]);
        }

        const std::string redacted = RedactText(output.str(), options.redactions);
        std::ofstream file(result.path, std::ios::binary | std::ios::trunc);
        if (!file) {
            result.error = "Unable to create diagnostics file";
            return result;
        }
        file.write(redacted.data(), static_cast<std::streamsize>(redacted.size()));
        file.flush();
        if (!file) {
            result.error = "Unable to write diagnostics file";
            return result;
        }
        result.success = true;
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    } catch (...) {
        result.error = "Unknown diagnostics export failure";
    }
    return result;
}

} // namespace minebackup::diagnostics
