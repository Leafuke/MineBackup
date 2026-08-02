#pragma once

#include <stop_token>
#include <string>

class NetworkService;

struct UpdateCheckResult {
    bool success = false;
    bool updateAvailable = false;
    std::string latestTag;
    std::string releaseNotes;
    std::wstring error;
};

struct MineBackupUpdateLinks {
    bool supported = false;
    std::string officialDownloadUrl;
    std::string acceleratedDownloadUrl;
    std::string changelogUrl;
};

struct NoticeCheckResult {
    bool success = false;
    bool noticeAvailable = false;
    std::string content;
    std::string contentId;
    std::wstring error;
};

std::string BuildMineBackupOfficialReleaseUrl(const std::string& versionTag);
MineBackupUpdateLinks BuildMineBackupUpdateLinks(const std::string& versionTag);

UpdateCheckResult CheckMineBackupUpdate(
    NetworkService& network,
    const std::string& currentVersion,
    const std::string& language,
    std::stop_token stopToken = {});

NoticeCheckResult CheckMineBackupNotice(
    NetworkService& network,
    const std::string& language,
    const std::string& lastSeenContentId,
    std::stop_token stopToken = {});
