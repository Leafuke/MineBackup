#include "LegacyLocationDiscovery.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <system_error>

using namespace std;

namespace {

filesystem::path Normalize(const filesystem::path& input) {
    error_code error;
    auto absolute = filesystem::absolute(input, error);
    if (error) absolute = input;
    auto normalized = filesystem::weakly_canonical(absolute, error);
    if (error) normalized = absolute.lexically_normal();
    return normalized;
}

wstring Identity(const filesystem::path& path) {
    wstring value = Normalize(path).wstring();
#ifdef _WIN32
    transform(value.begin(), value.end(), value.begin(), ::towlower);
#endif
    return value;
}

bool IsRegularFile(const filesystem::path& path) {
    error_code error;
    return filesystem::is_regular_file(path, error) && !error;
}

} // namespace

LegacyLocationDiscoveryResult DiscoverLegacyLocations(
    const filesystem::path& targetConfigFile,
    const filesystem::path& targetHistoryFile,
    const vector<LegacyLocationProbe>& probes) {
    LegacyLocationDiscoveryResult result;
    result.targetInitialized = IsRegularFile(targetConfigFile) || IsRegularFile(targetHistoryFile);

    const auto targetConfigIdentity = Identity(targetConfigFile);
    const auto targetHistoryIdentity = Identity(targetHistoryFile);
    map<wstring, size_t> candidateByRoot;
    for (const auto& probe : probes) {
        if (probe.root.empty()) continue;
        const auto root = Normalize(probe.root);
        const auto configFile = root / L"config.ini";
        const auto historyFile = root / L"history.json";
        const bool hasConfig = IsRegularFile(configFile);
        const bool hasHistory = IsRegularFile(historyFile);
        if (!hasConfig && !hasHistory) continue;

        const bool configIsTarget = Identity(configFile) == targetConfigIdentity;
        const bool historyIsTarget = Identity(historyFile) == targetHistoryIdentity;
        if ((!hasConfig || configIsTarget) && (!hasHistory || historyIsTarget)) continue;

        const auto rootIdentity = Identity(root);
        const auto existing = candidateByRoot.find(rootIdentity);
        if (existing != candidateByRoot.end()) {
            auto& origins = result.candidates[existing->second].origins;
            if (find(origins.begin(), origins.end(), probe.origin) == origins.end()) {
                origins.push_back(probe.origin);
            }
            continue;
        }

        LegacyLocationCandidate candidate;
        candidate.root = root;
        if (hasConfig && !configIsTarget) candidate.configFile = configFile;
        if (hasHistory && !historyIsTarget) candidate.historyFile = historyFile;
        candidate.origins.push_back(probe.origin);
        candidateByRoot.emplace(rootIdentity, result.candidates.size());
        result.candidates.push_back(std::move(candidate));
    }
    return result;
}
