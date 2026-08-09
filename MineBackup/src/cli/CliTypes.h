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
	Serve,
	ServeStatus,
	ServeStop,
	Doctor,
	ConfigList,
	ConfigShow,
	WorldList,
	HistoryList,
	JobList,
	JobShow,
	JobRun,
	Backup,
	Verify,
	Restore
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
	bool latest = false;
	bool confirm = false;
	minebackup::logging::LogFileLevel logLevel =
		minebackup::logging::LogFileLevel::Info;
	std::wstring configId;
	std::wstring worldPath;
	std::wstring jobId;
	std::wstring comment;
	std::wstring restoreMode = L"clean";
	std::filesystem::path filePath;
	std::filesystem::path outputPath;
	std::filesystem::path backupPath;
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
