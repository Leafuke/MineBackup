#pragma once

#include "Logging.h"
#include "OperationResult.h"
#include "json.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class CliCommand {
	Help,
	Version,
	ProfileInit,
	ProfileValidate,
	ProfileDiff,
	ProfileApply,
	ProfileExport,
	Doctor,
	ConfigList,
	ConfigShow,
	WorldList,
	HistoryList,
	JobList,
	JobShow,
	JobRun,
	Backup,
	RunSpecial
};

struct CliOptions {
	CliCommand command = CliCommand::Help;
	std::optional<std::filesystem::path> dataDirectory;
	bool json = false;
	bool noNetwork = false;
	bool nonInteractive = false;
	bool force = false;
	bool prune = false;
	bool confirmPrune = false;
	bool dryRun = false;
	minebackup::logging::LogFileLevel logLevel =
		minebackup::logging::LogFileLevel::Info;
	std::wstring configId;
	std::wstring worldPath;
	std::wstring specialConfigId;
	std::wstring jobId;
	std::wstring comment;
	std::filesystem::path filePath;
	std::filesystem::path outputPath;
};

struct CliParseResult {
	bool success = false;
	CliOptions options;
	std::vector<Diagnostic> diagnostics;
};

struct CliResult {
	std::string command;
	OperationCode code = OperationCode::InvalidArguments;
	nlohmann::json data = nlohmann::json::object();
	std::vector<Diagnostic> diagnostics;
};
