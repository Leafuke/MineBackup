#pragma once

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

// Exact world-relative file/subtree preservation. All writes target an isolated
// staging directory. The caller must discard staging on any failure.
namespace RestorePreservedPaths {
bool Normalize(const std::vector<std::wstring>& input,
    std::vector<std::wstring>& output, std::string& error);
bool Apply(const std::filesystem::path& current,
    const std::filesystem::path& staging,
    const std::vector<std::wstring>& selectors,
    std::string& error, std::stop_token stopToken = {});
}
