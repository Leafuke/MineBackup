#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace LegacyIniConfigCodec {

enum class DiagnosticSeverity {
    Warning,
    Fatal
};

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Warning;
    std::size_t line = 0;
    std::wstring section;
    std::wstring key;
    std::string eventId;
    std::string detail;
};

bool TryParseInt(
    const std::wstring& value,
    int minimum,
    int maximum,
    int& output);
bool TryParseFloat(
    const std::wstring& value,
    float minimum,
    float maximum,
    float& output);
std::vector<std::wstring> Split(
    const std::wstring& value,
    wchar_t delimiter);
bool HasFatalDiagnostics(const std::vector<Diagnostic>& diagnostics);

} // namespace LegacyIniConfigCodec

