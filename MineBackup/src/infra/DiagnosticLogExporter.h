#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace minebackup::diagnostics {

struct RedactionRule {
    std::string value;
    std::string replacement;
};

struct DiagnosticExportOptions {
    std::filesystem::path logsDirectory;
    std::string applicationVersion;
    std::string platform;
    std::string profileMode;
    std::vector<RedactionRule> redactions;
};

struct DiagnosticExportResult {
    bool success = false;
    std::filesystem::path path;
    std::string error;
};

std::string RedactText(
    std::string_view input,
    const std::vector<RedactionRule>& rules);

DiagnosticExportResult ExportDiagnostics(
    const DiagnosticExportOptions& options) noexcept;

} // namespace minebackup::diagnostics
