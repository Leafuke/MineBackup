#pragma once

#include "AppPaths.h"
#include "ConfigFactory.h"
#include "ExternalToolManager.h"

#include <filesystem>
#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

enum class ReadinessSeverity {
	Info,
	Warning,
	Blocking
};

struct ReadinessIssue {
	std::string code;
	ReadinessSeverity severity = ReadinessSeverity::Info;
	std::filesystem::path relatedPath;
	std::wstring detail;
};

struct ReadinessReport {
	bool ready = false;
	std::vector<ReadinessIssue> issues;
};

struct BatchReadinessResult {
	ReadinessReport report;
	std::filesystem::path resolvedSevenZip;
};

struct BackupWriteProbeResult {
	bool success = false;
	bool cancelled = false;
	std::wstring detail;
};

struct BatchReadinessDependencies {
	std::function<ExternalToolResolution(std::stop_token)> resolveSevenZip;
	std::function<BackupWriteProbeResult(
		const std::filesystem::path&, std::stop_token)> probeBackupDirectory;
};

class BatchReadinessService {
public:
	explicit BatchReadinessService(
		AppPaths appPaths,
		BatchReadinessDependencies dependencies = {});

	BatchReadinessResult CheckBatch(
		const std::vector<ConfigDraft>& drafts,
		const std::map<int, Config>& existingConfigs,
		std::stop_token stopToken = {}) const;

private:
	AppPaths appPaths_;
	BatchReadinessDependencies dependencies_;
};
