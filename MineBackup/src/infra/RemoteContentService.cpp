#include "RemoteContentService.h"

#include "NetworkService.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <regex>
#include <tuple>
#include <vector>

using namespace std;

namespace {

string TrimAsciiWhitespace(const string& value) {
    size_t begin = 0;
    while (begin < value.size() && isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

string ToLowerAscii(string value) {
    transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(tolower(character));
    });
    return value;
}

bool IsLikely404Body(const string& body) {
    const string lowered = ToLowerAscii(TrimAsciiWhitespace(body));
    return lowered.empty() || lowered == "404" || lowered == "404: not found" || lowered == "not found"
        || lowered.find("<title>404") != string::npos
        || lowered.find("404 not found") != string::npos
        || lowered.find("error 404") != string::npos;
}

string NormalizeNoticeText(const string& value) {
    string normalized;
    normalized.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\r') {
            if (index + 1 < value.size() && value[index + 1] == '\n') ++index;
            normalized.push_back('\n');
        }
        else {
            normalized.push_back(value[index]);
        }
    }
    return TrimAsciiWhitespace(normalized);
}

string ExtractLocalizedContent(const string& content, const string& language) {
    const size_t separator = content.find("---");
    if (separator == string::npos) return content;
    size_t before = separator;
    while (before > 0 && isspace(static_cast<unsigned char>(content[before - 1]))) --before;
    size_t after = separator + 3;
    while (after < content.size() && (isspace(static_cast<unsigned char>(content[after])) || content[after] == '-')) ++after;
    const string chinese = TrimAsciiWhitespace(content.substr(0, before));
    const string english = TrimAsciiWhitespace(content.substr(after));
    return language == "zh_CN"
        ? (chinese.empty() ? content : chinese)
        : (english.empty() ? content : english);
}

tuple<int, int, int, int> ParseVersionTuple(string version) {
    try {
        if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) version.erase(version.begin());
        const size_t first = version.find('.');
        const size_t second = first == string::npos ? string::npos : version.find('.', first + 1);
        const size_t suffix = version.find('-');
        if (first == string::npos || second == string::npos) return {};
        const int major = stoi(version.substr(0, first));
        const int minor = stoi(version.substr(first + 1, second - first - 1));
        const int patch = stoi(version.substr(second + 1, suffix == string::npos ? string::npos : suffix - second - 1));
        int servicePack = 0;
        if (suffix != string::npos) {
            const size_t position = version.find("sp", suffix);
            if (position != string::npos) servicePack = stoi(version.substr(position + 2));
        }
        return {major, minor, patch, servicePack};
    }
    catch (...) {
        return {};
    }
}

string StableContentId(const string& text) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : text) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    ostringstream output;
    output << "notice-v1-" << hex << hash;
    return output.str();
}

NetworkTextResult FetchTextWithMirror(
    NetworkService& network, const string& directUrl, const string& userAgent, stop_token stopToken) {
    NetworkTextResult last;
    for (const string& candidate : vector<string>{directUrl, "https://gh-proxy.org/" + directUrl}) {
        NetworkRequest request;
        request.url = candidate;
        request.userAgent = userAgent;
        last = network.GetText(request, stopToken);
        if (last.status == NetworkStatus::Succeeded && !IsLikely404Body(last.text)) return last;
        if (stopToken.stop_requested()) break;
    }
    return last;
}

bool IsValidVersionTag(const string& tag) {
    static const regex versionPattern(R"(^[vV]?[0-9]+\.[0-9]+\.[0-9]+(?:-sp[0-9]+)?$)");
    return regex_match(tag, versionPattern);
}

string NormalizeVersionTag(const string& versionTag) {
    if (!IsValidVersionTag(versionTag)) return {};
    return versionTag.front() == 'v' || versionTag.front() == 'V'
        ? "v" + versionTag.substr(1) : "v" + versionTag;
}

string CurrentPlatformAssetName(const string& version) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "MineBackup-windows-x64.exe";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return "MineBackup-" + version + "-macos-arm64.dmg";
#elif defined(__linux__) && defined(__x86_64__)
    return "minebackup_" + version + "_amd64.deb";
#else
    (void)version;
    return {};
#endif
}

} // namespace

string BuildMineBackupOfficialReleaseUrl(const string& versionTag) {
    return BuildMineBackupUpdateLinks(versionTag).changelogUrl;
}

MineBackupUpdateLinks BuildMineBackupUpdateLinks(const string& versionTag) {
    MineBackupUpdateLinks output;
    const string normalized = NormalizeVersionTag(versionTag);
    if (normalized.empty()) return output;

    output.changelogUrl = "https://github.com/Leafuke/MineBackup/releases/tag/" + normalized;
    const string version = normalized.substr(1);
    const string assetName = CurrentPlatformAssetName(version);
    if (assetName.empty()) return output;

    output.supported = true;
    output.officialDownloadUrl =
        "https://github.com/Leafuke/MineBackup/releases/download/" + normalized + "/" + assetName;
    output.acceleratedDownloadUrl = "https://gh-proxy.org/" + output.officialDownloadUrl;
    return output;
}

UpdateCheckResult CheckMineBackupUpdate(
    NetworkService& network, const string& currentVersion, const string& language, stop_token stopToken) {
    UpdateCheckResult output;
    const auto response = FetchTextWithMirror(network,
        "https://api.github.com/repos/Leafuke/MineBackup/releases/latest",
        "MineBackup Update Checker/1.16", stopToken);
    if (response.status != NetworkStatus::Succeeded) {
        output.error = response.error;
        return output;
    }
    try {
        const auto parsed = nlohmann::json::parse(response.text);
        if (!parsed.contains("tag_name") || !parsed["tag_name"].is_string()) {
            output.error = L"The update response did not contain a version tag.";
            return output;
        }
        string tag = parsed["tag_name"].get<string>();
        if (!IsValidVersionTag(tag)) {
            output.error = L"The update response contained an invalid version tag.";
            return output;
        }
        if (tag.front() != 'v' && tag.front() != 'V') tag.insert(tag.begin(), 'v');
        output.success = true;
        output.latestTag = tag;
        output.updateAvailable = ParseVersionTuple(tag) > ParseVersionTuple(currentVersion);
        if (output.updateAvailable && parsed.contains("body") && parsed["body"].is_string()) {
            string notes = parsed["body"].get<string>();
            replace(notes.begin(), notes.end(), '#', ' ');
            output.releaseNotes = ExtractLocalizedContent(notes, language);
        }
    }
    catch (...) {
        output.error = L"The update response was not valid JSON.";
    }
    return output;
}

NoticeCheckResult CheckMineBackupNotice(
    NetworkService& network, const string& language, const string& lastSeenContentId, stop_token stopToken) {
    NoticeCheckResult output;
    const string localizedUrl = "https://raw.githubusercontent.com/Leafuke/MineBackup/develop/notice_"
        + string(language == "zh_CN" ? "zh" : "en");
    auto response = FetchTextWithMirror(network, localizedUrl, "MineBackup Notice Checker/1.16", stopToken);
    bool usedFallback = false;
    if (response.status != NetworkStatus::Succeeded) {
        response = FetchTextWithMirror(network,
            "https://raw.githubusercontent.com/Leafuke/MineBackup/develop/notice",
            "MineBackup Notice Checker/1.16", stopToken);
        usedFallback = true;
    }
    if (response.status != NetworkStatus::Succeeded) {
        output.error = response.error;
        return output;
    }
    const string content = NormalizeNoticeText(
        usedFallback ? ExtractLocalizedContent(response.text, language) : response.text);
    if (content.empty() || IsLikely404Body(content)) {
        output.error = L"The notice response was empty or invalid.";
        return output;
    }
    output.success = true;
    output.content = content;
    output.contentId = StableContentId(content);
    output.noticeAvailable = output.contentId != lastSeenContentId;
    return output;
}
