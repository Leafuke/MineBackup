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
    std::wstring trimmed = value;
    while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\t' || trimmed.back() == L'\r' || trimmed.back() == L'\n')) {
        trimmed.pop_back();
    }
    size_t start = 0;
    while (start < trimmed.size() && (trimmed[start] == L' ' || trimmed[start] == L'\t')) {
        ++start;
    }
    if (start > 0) trimmed = trimmed.substr(start);
    if (trimmed.empty()) return false;
    try {
        std::size_t consumed = 0;
        const long long parsed = std::stoll(trimmed, &consumed, 10);
        if (consumed != trimmed.size()
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
    std::wstring trimmed = value;
    while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\t' || trimmed.back() == L'\r' || trimmed.back() == L'\n')) {
        trimmed.pop_back();
    }
    size_t start = 0;
    while (start < trimmed.size() && (trimmed[start] == L' ' || trimmed[start] == L'\t')) {
        ++start;
    }
    if (start > 0) trimmed = trimmed.substr(start);
    if (trimmed.empty()) return false;
    try {
        std::size_t consumed = 0;
        const float parsed = std::stof(trimmed, &consumed);
        if (consumed != trimmed.size()
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

