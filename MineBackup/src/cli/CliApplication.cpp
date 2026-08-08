#include "CliApplication.h"

#include "AppPaths.h"
#include "ExternalToolManager.h"
#include "HistoryRepository.h"
#include "Logging.h"
#include "MineBackupVersion.h"
#include "OperationResult.h"
#include "ProfileConfigCatalog.h"
#include "RuntimeIntegration.h"
#include "SingleInstanceService.h"
#include "SpecialTaskDocument.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>

using namespace std;

namespace {

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
	optional<filesystem::path> dataDirectory;
	bool json = false;
	bool noNetwork = false;
	bool nonInteractive = false;
	minebackup::logging::LogFileLevel logLevel = minebackup::logging::LogFileLevel::Info;
	wstring configId;
	wstring worldPath;
	wstring specialConfigId;
};

struct CliParseResult {
	bool success = false;
	CliOptions options;
	vector<Diagnostic> diagnostics;
};

struct CliResult {
	string command;
	OperationCode code = OperationCode::InvalidArguments;
	nlohmann::json data = nlohmann::json::object();
	vector<Diagnostic> diagnostics;
};

string CommandName(CliCommand command) {
	switch (command) {
	case CliCommand::Help: return "help";
	case CliCommand::Version: return "version";
	case CliCommand::Doctor: return "doctor";
	case CliCommand::ConfigList: return "config.list";
	case CliCommand::WorldList: return "world.list";
	case CliCommand::HistoryList: return "history.list";
	case CliCommand::Backup: return "backup";
	case CliCommand::RunSpecial: return "run-special";
	}
	return "unknown";
}

void AddParseError(CliParseResult& result, string eventId, string detail) {
	result.diagnostics.push_back({
		std::move(eventId), DiagnosticSeverity::Error, std::move(detail)});
}

CliParseResult ParseArguments(const vector<wstring>& arguments) {
	CliParseResult result;
	for (const auto& argument : arguments) {
		if (argument == L"--json") result.options.json = true;
	}
	vector<wstring> positional;
	for (size_t index = 0; index < arguments.size(); ++index) {
		const wstring& argument = arguments[index];
		if (argument == L"--json") continue;
		if (argument == L"--no-network") { result.options.noNetwork = true; continue; }
		if (argument == L"--non-interactive") { result.options.nonInteractive = true; continue; }
		if (argument == L"--help" || argument == L"-h") {
			result.options.command = CliCommand::Help;
			result.success = true;
			return result;
		}
		if (argument == L"--version") {
			result.options.command = CliCommand::Version;
			result.success = true;
			return result;
		}
		if (argument == L"--data-dir") {
			if (++index >= arguments.size()) {
				AddParseError(result, "cli.argument.missing_value", "--data-dir requires a path.");
				return result;
			}
			result.options.dataDirectory = filesystem::path(arguments[index]);
			continue;
		}
		if (argument == L"--log-level") {
			if (++index >= arguments.size()) {
				AddParseError(result, "cli.argument.missing_value", "--log-level requires off, info, or debug.");
				return result;
			}
			const wstring& level = arguments[index];
			if (level == L"off") result.options.logLevel = minebackup::logging::LogFileLevel::Off;
			else if (level == L"info") result.options.logLevel = minebackup::logging::LogFileLevel::Info;
			else if (level == L"debug") result.options.logLevel = minebackup::logging::LogFileLevel::Debug;
			else {
				AddParseError(result, "cli.log_level.invalid", "--log-level requires off, info, or debug.");
				return result;
			}
			continue;
		}
		if (argument == L"--config") {
			if (++index >= arguments.size()) {
				AddParseError(result, "cli.argument.missing_value", "--config requires a ConfigId.");
				return result;
			}
			result.options.configId = arguments[index];
			continue;
		}
		if (argument == L"--world") {
			if (++index >= arguments.size()) {
				AddParseError(result, "cli.argument.missing_value", "--world requires a relative path.");
				return result;
			}
			result.options.worldPath = arguments[index];
			continue;
		}
		if (!argument.empty() && argument.front() == L'-') {
			AddParseError(result, "cli.argument.unknown", wstring_to_utf8(argument));
			return result;
		}
		positional.push_back(argument);
	}

	if (positional.empty()) {
		AddParseError(result, "cli.command.missing", "A command is required.");
		return result;
	}
	if (positional == vector<wstring>{L"doctor"}) result.options.command = CliCommand::Doctor;
	else if (positional == vector<wstring>{L"config", L"list"}) result.options.command = CliCommand::ConfigList;
	else if (positional == vector<wstring>{L"world", L"list"}) result.options.command = CliCommand::WorldList;
	else if (positional == vector<wstring>{L"history", L"list"}) result.options.command = CliCommand::HistoryList;
	else if (positional == vector<wstring>{L"backup"}) result.options.command = CliCommand::Backup;
	else if (positional.size() == 2 && positional[0] == L"run-special") {
		result.options.command = CliCommand::RunSpecial;
		result.options.specialConfigId = positional[1];
	}
	else {
		AddParseError(result, "cli.command.invalid", "Unknown command or extra positional arguments.");
		return result;
	}
	if ((result.options.command == CliCommand::WorldList
			|| result.options.command == CliCommand::HistoryList
			|| result.options.command == CliCommand::Backup)
		&& result.options.configId.empty()) {
		AddParseError(result, "cli.config.required", "The command requires --config <ConfigId>.");
		return result;
	}
	if ((result.options.command == CliCommand::HistoryList
			|| result.options.command == CliCommand::Backup)
		&& result.options.worldPath.empty()) {
		AddParseError(result, "cli.world.required", "The command requires --world <relative-path>.");
		return result;
	}
	result.success = true;
	return result;
}

void PrintHelp() {
	cout
		<< "MineBackup headless command line interface\n\n"
		<< "Usage:\n"
		<< "  minebackup-cli [global options] doctor\n"
		<< "  minebackup-cli [global options] config list\n"
		<< "  minebackup-cli [global options] world list --config <ConfigId>\n"
		<< "  minebackup-cli [global options] history list --config <ConfigId> --world <relative-path>\n"
		<< "  minebackup-cli [global options] backup --config <ConfigId> --world <relative-path>\n"
		<< "  minebackup-cli [global options] run-special <SpecialConfigId>\n\n"
		<< "Global options:\n"
		<< "  --data-dir <path>  --json  --log-level <off|info|debug>\n"
		<< "  --no-network  --non-interactive  --help  --version\n";
}

void Render(const CliResult& result, bool jsonOutput) {
	if (jsonOutput) {
		nlohmann::json diagnostics = nlohmann::json::array();
		for (const auto& item : result.diagnostics) {
			diagnostics.push_back({
				{"eventId", item.eventId},
				{"severity", ToString(item.severity)},
				{"detail", item.detail}});
		}
		nlohmann::json envelope{
			{"schemaVersion", 1},
			{"command", result.command},
			{"ok", IsSuccessful(result.code)},
			{"code", ToString(result.code)},
			{"data", result.data},
			{"diagnostics", diagnostics}};
		cout << envelope.dump() << '\n';
		return;
	}
	for (const auto& item : result.diagnostics) {
		ostream& stream = item.severity == DiagnosticSeverity::Error ? cerr : cout;
		stream << '[' << ToString(item.severity) << "] " << item.eventId;
		if (!item.detail.empty()) stream << ": " << item.detail;
		stream << '\n';
	}
	if (!result.data.empty()) cout << result.data.dump(2) << '\n';
}

OperationCode CatalogCode(ProfileCatalogStatus status) {
	switch (status) {
	case ProfileCatalogStatus::Loaded: return OperationCode::Success;
	case ProfileCatalogStatus::MigrationRequired: return OperationCode::MigrationRequired;
	case ProfileCatalogStatus::Missing:
	case ProfileCatalogStatus::Invalid: return OperationCode::InvalidProfile;
	}
	return OperationCode::InvalidProfile;
}

CliResult ConfigList(const ProfileConfigCatalog& catalog) {
	CliResult result{"config.list", OperationCode::Success};
	result.data["configs"] = nlohmann::json::array();
	for (const auto& [index, config] : catalog.configs) {
		(void)index;
		result.data["configs"].push_back({
			{"configId", wstring_to_utf8(config.configId)},
			{"name", config.name},
			{"saveRoot", wstring_to_utf8(config.saveRoot)},
			{"backupPath", wstring_to_utf8(config.backupPath)},
			{"worldCount", config.worlds.size()}});
	}
	return result;
}

CliResult WorldList(const ProfileConfigCatalog& catalog, const CliOptions& options) {
	CliResult result{"world.list", OperationCode::Success};
	const Config* config = catalog.FindConfig(options.configId);
	if (!config) {
		result.code = OperationCode::TargetNotFound;
		result.diagnostics.push_back({
			"config.not_found", DiagnosticSeverity::Error, wstring_to_utf8(options.configId)});
		return result;
	}
	result.data["configId"] = wstring_to_utf8(config->configId);
	result.data["worlds"] = nlohmann::json::array();
	for (const auto& [path, description] : config->worlds) {
		result.data["worlds"].push_back({
			{"path", wstring_to_utf8(path)},
			{"description", wstring_to_utf8(description)},
			{"exists", filesystem::is_directory(filesystem::path(config->saveRoot) / path)}});
	}
	return result;
}

CliResult HistoryList(
	const ProfileConfigCatalog& catalog,
	const AppPaths& paths,
	const CliOptions& options) {
	CliResult result{"history.list", OperationCode::Success};
	const Config* config = catalog.FindConfig(options.configId);
	if (!config) {
		result.code = OperationCode::TargetNotFound;
		result.diagnostics.push_back({
			"config.not_found", DiagnosticSeverity::Error, wstring_to_utf8(options.configId)});
		return result;
	}
	wstring normalized;
	if (!SpecialTaskStorage::TryNormalizeWorldPath(options.worldPath, normalized)
		|| none_of(config->worlds.begin(), config->worlds.end(), [&](const auto& world) {
			return world.first == normalized;
		})) {
		result.code = OperationCode::TargetNotFound;
		result.diagnostics.push_back({
			"world.not_found", DiagnosticSeverity::Error, wstring_to_utf8(options.worldPath)});
		return result;
	}
	HistoryRepository history;
	if (filesystem::exists(paths.HistoryFile())
		&& !history.Load(paths.HistoryFile(), catalog.configs)) {
		result.code = OperationCode::InvalidProfile;
		result.diagnostics.push_back({
			"history.load.invalid", DiagnosticSeverity::Error,
			wstring_to_utf8(paths.HistoryFile().wstring())});
		return result;
	}
	result.data["configId"] = wstring_to_utf8(config->configId);
	result.data["world"] = wstring_to_utf8(normalized);
	result.data["history"] = nlohmann::json::array();
	for (const auto& entry : *history.EntriesForConfig(config->configId)) {
		if (entry.worldName != normalized
			&& filesystem::path(entry.worldPath).lexically_normal()
				!= (filesystem::path(config->saveRoot) / normalized).lexically_normal()) continue;
		result.data["history"].push_back({
			{"timestamp", wstring_to_utf8(entry.timestamp_str)},
			{"backupFile", wstring_to_utf8(entry.backupFile)},
			{"backupType", wstring_to_utf8(entry.backupType)},
			{"comment", wstring_to_utf8(entry.comment)},
			{"important", entry.isImportant},
			{"cloudArchived", entry.isCloudArchived}});
	}
	return result;
}

CliResult Doctor(
	const ProfileCatalogLoadResult& loaded,
	const AppPaths& paths,
	const CliOptions& options) {
	CliResult result{"doctor", CatalogCode(loaded.status)};
	result.diagnostics = loaded.diagnostics;
	result.data["profileIdentity"] = wstring_to_utf8(paths.profileIdentity);
	result.data["configFile"] = wstring_to_utf8(paths.ConfigFile().wstring());
	result.data["specialTasksFile"] = wstring_to_utf8(paths.SpecialTasksFile().wstring());
	result.data["historyFile"] = wstring_to_utf8(paths.HistoryFile().wstring());
	result.data["configCount"] = loaded.catalog.configs.size();
	result.data["specialConfigCount"] = loaded.catalog.specialConfigs.size();
	result.data["specialTaskMigrationPending"] = loaded.catalog.specialTaskMigrationPending;
	result.data["paths"] = nlohmann::json::array();
	if (!loaded.IsLoaded()) return result;

	bool missingTool = false;
	result.data["tools"] = nlohmann::json::array();
	for (const auto& [index, config] : loaded.catalog.configs) {
		(void)index;
		const filesystem::path saveRoot(config.saveRoot);
		const filesystem::path backupRoot(config.backupPath);
		const bool saveRootReady = saveRoot.is_absolute()
			&& filesystem::is_directory(saveRoot);
		const bool backupRootValid = backupRoot.is_absolute();
		result.data["paths"].push_back({
			{"configId", wstring_to_utf8(config.configId)},
			{"saveRoot", wstring_to_utf8(config.saveRoot)},
			{"saveRootReady", saveRootReady},
			{"backupRoot", wstring_to_utf8(config.backupPath)},
			{"backupRootAbsolute", backupRootValid},
			{"pendingLocalBinding", config.pendingLocalBinding}});
		if (!saveRootReady || !backupRootValid || config.pendingLocalBinding) {
			result.code = OperationCode::InvalidProfile;
			result.diagnostics.push_back({
				"profile.path.not_ready", DiagnosticSeverity::Error,
				wstring_to_utf8(config.configId)});
		}
		const auto sevenZip = ExternalToolManager::ResolveSevenZip(
			config.zipPath, paths);
		result.data["tools"].push_back({
			{"configId", wstring_to_utf8(config.configId)},
			{"tool", "7zip"},
			{"available", sevenZip.available},
			{"path", wstring_to_utf8(sevenZip.executable.wstring())},
			{"detail", wstring_to_utf8(sevenZip.diagnostic)}});
		missingTool |= !sevenZip.available;
		if (config.cloudSyncEnabled && !options.noNetwork) {
			const auto rclone = ExternalToolManager::ResolveRclone(
				config.rclonePath, paths);
			result.data["tools"].push_back({
				{"configId", wstring_to_utf8(config.configId)},
				{"tool", "rclone"},
				{"available", rclone.available},
				{"path", wstring_to_utf8(rclone.executable.wstring())},
				{"detail", wstring_to_utf8(rclone.diagnostic)}});
			missingTool |= !rclone.available;
		}
	}
	if (options.noNetwork) {
		result.data["knotLink"] = "disabled";
	}
	else {
		HeadlessKnotLinkBridge bridge;
		const bool available = bridge.Start();
		bridge.Stop();
		result.data["knotLink"] = available ? "ready" : "unavailable";
		if (!available) result.diagnostics.push_back({
			"knotlink.listener.unavailable", DiagnosticSeverity::Warning,
			"The local KnotLink ports are not available."});
	}
	if (missingTool && IsSuccessful(result.code)) {
		result.code = OperationCode::ToolUnavailable;
	}
	return result;
}

} // namespace

int RunMineBackupCli(const vector<wstring>& arguments) {
	CliParseResult parsed = ParseArguments(arguments);
	if (!parsed.success) {
		CliResult error{"parse", OperationCode::InvalidArguments};
		error.diagnostics = std::move(parsed.diagnostics);
		Render(error, parsed.options.json);
		return ToExitCode(error.code);
	}
	if (parsed.options.command == CliCommand::Help) {
		PrintHelp();
		return 0;
	}
	if (parsed.options.command == CliCommand::Version) {
		cout << "minebackup-cli " MINEBACKUP_VERSION_STRING "\n";
		return 0;
	}

	AppPaths paths;
	wstring pathError;
	if (!ResolveAppPaths(
			AppPathRequest{parsed.options.dataDirectory},
			GetExecutablePath(),
			paths,
			pathError)) {
		CliResult error{CommandName(parsed.options.command), OperationCode::InvalidProfile};
		error.diagnostics.push_back({
			"profile.path.invalid", DiagnosticSeverity::Error, wstring_to_utf8(pathError)});
		Render(error, parsed.options.json);
		return ToExitCode(error.code);
	}
	SetCurrentAppPaths(paths);
	minebackup::logging::Initialize({
		paths.logsRoot,
		parsed.options.logLevel,
		false,
		MINEBACKUP_VERSION_STRING});

	SingleInstanceService instance;
	wstring lockError;
	const InstanceAcquireResult lock = instance.Acquire(
		paths.profileIdentity, paths.runtimeRoot, lockError);
	if (lock != InstanceAcquireResult::Acquired) {
		CliResult error{CommandName(parsed.options.command),
			lock == InstanceAcquireResult::AlreadyRunning
				? OperationCode::ProfileBusy : OperationCode::InvalidProfile};
		error.diagnostics.push_back({
			lock == InstanceAcquireResult::AlreadyRunning
				? "profile.lock.busy" : "profile.lock.failed",
			DiagnosticSeverity::Error,
			wstring_to_utf8(lockError)});
		Render(error, parsed.options.json);
		minebackup::logging::Shutdown();
		return ToExitCode(error.code);
	}

	const auto loaded = ProfileConfigCatalogLoader::Load(
		paths.ConfigFile(), paths.SpecialTasksFile());
	CliResult result;
	if (parsed.options.command == CliCommand::Doctor) {
		result = Doctor(loaded, paths, parsed.options);
	}
	else if (!loaded.IsLoaded()) {
		result.command = CommandName(parsed.options.command);
		result.code = CatalogCode(loaded.status);
		result.diagnostics = loaded.diagnostics;
	}
	else if (parsed.options.command == CliCommand::ConfigList) {
		result = ConfigList(loaded.catalog);
		result.diagnostics = loaded.diagnostics;
	}
	else if (parsed.options.command == CliCommand::WorldList) {
		result = WorldList(loaded.catalog, parsed.options);
		result.diagnostics.insert(result.diagnostics.begin(),
			loaded.diagnostics.begin(), loaded.diagnostics.end());
	}
	else if (parsed.options.command == CliCommand::HistoryList) {
		result = HistoryList(loaded.catalog, paths, parsed.options);
		result.diagnostics.insert(result.diagnostics.begin(),
			loaded.diagnostics.begin(), loaded.diagnostics.end());
	}
	else {
		result.command = CommandName(parsed.options.command);
		result.code = OperationCode::InvalidArguments;
		result.diagnostics.push_back({
			"cli.command.not_implemented", DiagnosticSeverity::Error,
			"Execution commands are added in the next implementation stage."});
	}
	Render(result, parsed.options.json);
	minebackup::logging::Shutdown();
	return ToExitCode(result.code);
}
