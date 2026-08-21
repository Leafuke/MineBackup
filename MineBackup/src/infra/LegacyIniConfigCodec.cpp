#include "LegacyIniConfigCodec.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace LegacyIniConfigCodec {

bool TryParseInt(
    const std::wstring& value,
    int minimum,
    int maximum,
    int& output) {
    if (minimum > maximum || value.empty()) return false;
    try {
        std::size_t consumed = 0;
        const long long parsed = std::stoll(value, &consumed, 10);
        if (consumed != value.size()
            || parsed < minimum
            || parsed > maximum) {
            return false;
        }
        output = static_cast<int>(parsed);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool TryParseFloat(
    const std::wstring& value,
    float minimum,
    float maximum,
    float& output) {
    if (minimum > maximum || value.empty()) return false;
    try {
        std::size_t consumed = 0;
        const float parsed = std::stof(value, &consumed);
        if (consumed != value.size()
            || !std::isfinite(parsed)
            || parsed < minimum
            || parsed > maximum) {
            return false;
        }
        output = parsed;
        return true;
    }
    catch (...) {
        return false;
    }
}

std::vector<std::wstring> Split(
    const std::wstring& value,
    wchar_t delimiter) {
    std::vector<std::wstring> tokens;
    std::wstringstream stream(value);
    for (std::wstring token; std::getline(stream, token, delimiter);) {
        tokens.push_back(std::move(token));
    }
    if (!value.empty() && value.back() == delimiter) tokens.emplace_back();
    return tokens;
}

bool HasFatalDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Fatal;
    });
}

} // namespace LegacyIniConfigCodec

