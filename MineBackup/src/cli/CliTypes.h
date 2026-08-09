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
	Doctor,
	ConfigList,
	WorldList,
	HistoryList,
	Backup,
	RunSpecial
};

struct CliOptions {
	CliCommand command = CliCommand::Help;
	std::optional<std::filesystem::path> dataDirectory;
	bool json = false;
	bool noNetwork = false;
	bool nonInteractive = false;
	minebackup::logging::LogFileLevel logLevel =
		minebackup::logging::LogFileLevel::Info;
	std::wstring configId;
	std::wstring worldPath;
	std::wstring specialConfigId;
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
