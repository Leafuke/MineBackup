#pragma once
#include <algorithm>
#include <string_view>
namespace CompressionPolicy {
inline int NormalizeLevel(std::wstring_view method, int level) {
    const bool zstd = method.size() == 4
        && (method[0] == L'z' || method[0] == L'Z')
        && (method[1] == L's' || method[1] == L'S')
        && (method[2] == L't' || method[2] == L'T')
        && (method[3] == L'd' || method[3] == L'D');
    return std::clamp(level, 1, zstd ? 22 : 9);
}
} // namespace CompressionPolicy
