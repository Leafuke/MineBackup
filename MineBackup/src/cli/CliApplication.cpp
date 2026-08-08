#include "CliApplication.h"

#include "AppPaths.h"
#include "BackupService.h"
#include "CliSignalHandler.h"
#include "CliToolBootstrap.h"
#include "ExternalToolManager.h"
#include "HistoryRepository.h"
#include "Logging.h"
#include "MineBackupVersion.h"
#include "OperationResult.h"
#include "ProfileConfigCatalog.h"
#include "RuntimeIntegration.h"
#include "RuntimeCloudPostHook.h"
#include "RuntimeFileLock.h"
#include "RuntimeRetentionService.h"
#include "SingleInstanceService.h"
#include "SpecialTaskDocument.h"
#include "SpecialTaskRunner.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <utility>

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

void AppendTaskDiagnostics(
	vector<Diagnostic>& destination,
	const vector<SpecialTaskStorage::Diagnostic>& source) {
	for (const auto& diagnostic : source) {
		destination.push_back({
			diagnostic.eventId,
			diagnostic.severity == SpecialTaskStorage::DiagnosticSeverity::Fatal
				? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
			diagnostic.detail});
	}
}

class CliBackupRuntime {
public:
	CliBackupRuntime(
		const AppPaths& paths,
		const ProfileConfigCatalog& catalog,
		bool noNetwork,
		stop_token stopToken)
		: paths_(paths), catalog_(catalog), noNetwork_(noNetwork), stopToken_(stopToken) {
	}

	OperationCode Initialize(vector<Diagnostic>& diagnostics) {
		if (filesystem::exists(paths_.HistoryFile())
			&& !history_.Load(paths_.HistoryFile(), catalog_.configs)) {
			diagnostics.push_back({
				"history.load.invalid", DiagnosticSeverity::Error,
				wstring_to_utf8(paths_.HistoryFile().wstring())});
			return OperationCode::InvalidProfile;
		}
		retention_ = make_unique<RuntimeRetentionService>(
			history_, paths_.HistoryFile(), catalog_.configs);
		if (noNetwork_) {
			hotBackup_ = make_shared<NetworkDisabledKnotLinkBridge>();
			eventSink_ = make_shared<NoopRuntimeEventSink>();
			cloudPost_ = make_shared<NetworkDisabledCloudPostHook>();
		}
		else {
			auto bridge = make_shared<HeadlessKnotLinkBridge>();
			if (!bridge->Start()) {
				diagnostics.push_back({
					"knotlink.listener.unavailable", DiagnosticSeverity::Warning,
					"The local KnotLink ports are unavailable; locked worlds use the live-file fallback."});
			}
			hotBackup_ = bridge;
			eventSink_ = bridge;
			cloudPost_ = make_shared<SynchronousRcloneCloudPostHook>(
				paths_, history_, [this] { return catalog_.ConfigSnapshot(); });
		}

		BackupServiceDependencies dependencies;
		dependencies.paths = paths_;
		dependencies.ensureMigration = [](const BackupRequest&) {
			MigrationUnitResult result;
			result.status = MigrationStatus::NotNeeded;
			return result;
		};
		dependencies.isFileLocked = IsRuntimeFileLocked;
		dependencies.hotBackup = hotBackup_;
		dependencies.eventSink = eventSink_;
		dependencies.cloudPost = cloudPost_;
		dependencies.addHistory = [this](const HistoryEntry& entry) {
			const auto mutation = history_.Mutate(
				entry.configId,
				paths_.HistoryFile(),
				catalog_.configs,
				true,
				[&](vector<HistoryEntry>& entries) {
					for (auto& current : entries) {
						if (current.worldName == entry.worldName
							&& current.backupFile == entry.backupFile) {
							current = entry;
							return true;
						}
					}
					entries.push_back(entry);
					return true;
				});
			return mutation.changed && mutation.persisted;
		};
		dependencies.removeHistory = [this](
			const wstring& worldName,
			const wstring& backupFile) {
			bool persisted = true;
			for (const auto& [index, config] : catalog_.configs) {
				(void)index;
				const auto mutation = history_.Mutate(
					config.configId,
					paths_.HistoryFile(),
					catalog_.configs,
					true,
					[&](vector<HistoryEntry>& entries) {
						const auto before = entries.size();
						erase_if(entries, [&](const HistoryEntry& entry) {
							return entry.worldName == worldName
								&& entry.backupFile == backupFile;
						});
						return entries.size() != before;
					});
				persisted = mutation.persisted && persisted;
			}
			return persisted;
		};
		dependencies.enforceRetention = [this](
			const BackupRequest& request,
			const HistoryEntry& entry) {
			retention_->Enforce(request, entry);
		};
		service_ = make_unique<BackupService>(std::move(dependencies));
		return OperationCode::Success;
	}

	optional<BackupRequest> Resolve(const SpecialTaskTarget& target) const {
		const Config* config = catalog_.FindConfig(target.configId);
		wstring normalized;
		if (!config
			|| !SpecialTaskStorage::TryNormalizeWorldPath(target.worldPath, normalized)) {
			return nullopt;
		}
		const auto world = find_if(config->worlds.begin(), config->worlds.end(),
			[&](const auto& candidate) { return candidate.first == normalized; });
		if (world == config->worlds.end()) return nullopt;
		BackupRequest request;
		request.config = *config;
		request.world = {config->configId, normalized};
		request.sourcePath = filesystem::path(config->saveRoot) / normalized;
		request.displayName = world->second.empty() ? normalized : world->second;
		return request;
	}

	SpecialTaskPreflightResult Preflight(const BackupRequest& request) const {
		SpecialTaskPreflightResult result;
		if (!filesystem::is_directory(request.sourcePath)) {
			result.code = OperationCode::TargetNotFound;
			result.diagnostics.push_back({
				"world.path.missing", DiagnosticSeverity::Error,
				wstring_to_utf8(request.sourcePath.wstring())});
			return result;
		}
		if (request.config.pendingLocalBinding) {
			result.code = OperationCode::MigrationRequired;
			result.diagnostics.push_back({
				"backup.profile.binding_required", DiagnosticSeverity::Error, {}});
			return result;
		}
		const auto resolution = ExternalToolManager::ResolveSevenZip(
			request.config.zipPath, paths_, stopToken_);
		if (!resolution.available) {
			wstring bootstrapError;
			if (!EnsureCliSevenZip(paths_, stopToken_, bootstrapError)
				|| !ExternalToolManager::ResolveSevenZip(
					request.config.zipPath, paths_, stopToken_).available) {
				result.code = stopToken_.stop_requested()
					? OperationCode::Cancelled : OperationCode::ToolUnavailable;
				result.diagnostics.push_back({
					stopToken_.stop_requested() ? "backup.cancelled" : "backup.tool.unavailable",
					stopToken_.stop_requested() ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
					wstring_to_utf8(bootstrapError.empty()
						? resolution.diagnostic : bootstrapError)});
			}
		}
		return result;
	}

	BackupResult Run(const BackupRequest& request) const {
		return service_->Run(request, stopToken_);
	}

private:
	AppPaths paths_;
	const ProfileConfigCatalog& catalog_;
	bool noNetwork_ = false;
	stop_token stopToken_;
	HistoryRepository history_;
	unique_ptr<RuntimeRetentionService> retention_;
	shared_ptr<IHotBackupBridge> hotBackup_;
	shared_ptr<IRuntimeEventSink> eventSink_;
	shared_ptr<ICloudPostHook> cloudPost_;
	unique_ptr<BackupService> service_;
};

CliResult RenderBackupResult(BackupResult backup) {
	CliResult result{"backup", backup.code};
	result.diagnostics = std::move(backup.diagnostics);
	result.data["outcome"] = ToString(backup.outcome);
	result.data["archivePath"] = wstring_to_utf8(backup.archivePath.wstring());
	result.data["cloud"] = ToString(backup.cloud.status);
	if (backup.historyEntry) {
		result.data["history"] = {
			{"configId", wstring_to_utf8(backup.historyEntry->configId)},
			{"world", wstring_to_utf8(backup.historyEntry->worldName)},
			{"backupFile", wstring_to_utf8(backup.historyEntry->backupFile)},
			{"backupType", wstring_to_utf8(backup.historyEntry->backupType)},
			{"timestamp", wstring_to_utf8(backup.historyEntry->timestamp_str)}};
	}
	return result;
}

CliResult BackupCommand(
	const ProfileConfigCatalog& catalog,
	const AppPaths& paths,
	const CliOptions& options,
	stop_token stopToken) {
	vector<Diagnostic> initialization;
	CliBackupRuntime runtime(paths, catalog, options.noNetwork, stopToken);
	const OperationCode initialized = runtime.Initialize(initialization);
	if (!IsSuccessful(initialized)) {
		return {"backup", initialized, nlohmann::json::object(), std::move(initialization)};
	}
	SpecialTaskTarget target{options.configId, options.worldPath};
	auto request = runtime.Resolve(target);
	if (!request) {
		initialization.push_back({
			"world.not_found", DiagnosticSeverity::Error,
			wstring_to_utf8(options.worldPath)});
		return {"backup", OperationCode::TargetNotFound,
			nlohmann::json::object(), std::move(initialization)};
	}
	const auto preflight = runtime.Preflight(*request);
	if (!IsSuccessful(preflight.code)) {
		initialization.insert(initialization.end(),
			preflight.diagnostics.begin(), preflight.diagnostics.end());
		return {"backup", preflight.code,
			nlohmann::json::object(), std::move(initialization)};
	}
	CliResult result = RenderBackupResult(runtime.Run(*request));
	result.diagnostics.insert(result.diagnostics.begin(),
		initialization.begin(), initialization.end());
	return result;
}

CliResult RunSpecialCommand(
	ProfileConfigCatalog& catalog,
	const AppPaths& paths,
	const CliOptions& options,
	stop_token stopToken) {
	CliResult result{"run-special", OperationCode::Success};
	if (catalog.specialTaskMigrationPending) {
		auto migration = SpecialTaskStorage::MigrateLegacy(
			catalog.configs, catalog.specialConfigs);
		AppendTaskDiagnostics(result.diagnostics, migration.diagnostics);
		if (!migration.success) {
			result.code = OperationCode::InvalidProfile;
			return result;
		}
		wstring saveError;
		if (!SpecialTaskStorage::Save(paths.SpecialTasksFile(), migration.document, saveError)) {
			result.code = OperationCode::InvalidProfile;
			result.diagnostics.push_back({
				"special.migration.write_failed", DiagnosticSeverity::Error,
				wstring_to_utf8(saveError)});
			return result;
		}
		vector<SpecialTaskStorage::Diagnostic> validation;
		if (!SpecialTaskStorage::ApplyAndValidate(
				migration.document, catalog.configs, catalog.specialConfigs, validation)) {
			result.code = OperationCode::InvalidProfile;
			AppendTaskDiagnostics(result.diagnostics, validation);
			return result;
		}
		AppendTaskDiagnostics(result.diagnostics, validation);
		catalog.specialTaskMigrationPending = false;
	}
	const SpecialConfig* special = catalog.FindSpecialConfig(options.specialConfigId);
	if (!special) {
		result.code = OperationCode::TargetNotFound;
		result.diagnostics.push_back({
			"special.config.not_found", DiagnosticSeverity::Error,
			wstring_to_utf8(options.specialConfigId)});
		return result;
	}

	vector<Diagnostic> initialization;
	CliBackupRuntime runtime(paths, catalog, options.noNetwork, stopToken);
	const OperationCode initialized = runtime.Initialize(initialization);
	result.diagnostics.insert(result.diagnostics.end(),
		initialization.begin(), initialization.end());
	if (!IsSuccessful(initialized)) {
		result.code = initialized;
		return result;
	}
	SpecialTaskRunner runner({
		[&](const SpecialTaskTarget& target) { return runtime.Resolve(target); },
		[&](const BackupRequest& request) { return runtime.Preflight(request); },
		[&](const BackupRequest& request, stop_token) { return runtime.Run(request); },
		[](const ShellTaskSpec& spec, stop_token token) {
			return ProcessRunner::RunShellTask(spec, token);
		}});
	SpecialRunResult run = runner.Run(*special, stopToken);
	result.code = run.code;
	result.diagnostics.insert(result.diagnostics.end(),
		run.diagnostics.begin(), run.diagnostics.end());
	result.data["tasks"] = nlohmann::json::array();
	for (const auto& task : run.tasks) {
		nlohmann::json diagnostics = nlohmann::json::array();
		for (const auto& diagnostic : task.diagnostics) {
			diagnostics.push_back({
				{"eventId", diagnostic.eventId},
				{"severity", ToString(diagnostic.severity)},
				{"detail", diagnostic.detail}});
		}
		result.data["tasks"].push_back({
			{"taskId", wstring_to_utf8(task.taskId)},
			{"code", ToString(task.code)},
			{"diagnostics", diagnostics}});
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
	result.data["unsupportedScriptTaskCount"] = 0;
	for (const auto& [index, special] : loaded.catalog.specialConfigs) {
		(void)index;
		for (const auto& task : special.specialTasks) {
			if (task.type != SpecialTaskType::Script) continue;
			result.code = OperationCode::InvalidProfile;
			result.data["unsupportedScriptTaskCount"] =
				result.data["unsupportedScriptTaskCount"].get<size_t>() + 1;
			result.diagnostics.push_back({
				"special.script.unsupported", DiagnosticSeverity::Error,
				wstring_to_utf8(special.specialConfigId + L":" + task.taskId)});
		}
	}
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
		if (parsed.options.json) {
			CliResult help{"help", OperationCode::Success};
			help.data["usage"] = "minebackup-cli --help";
			Render(help, true);
		}
		else {
			PrintHelp();
		}
		return 0;
	}
	if (parsed.options.command == CliCommand::Version) {
		if (parsed.options.json) {
			CliResult version{"version", OperationCode::Success};
			version.data["version"] = MINEBACKUP_VERSION_STRING;
			Render(version, true);
		}
		else {
			cout << "minebackup-cli " MINEBACKUP_VERSION_STRING "\n";
		}
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

	auto loaded = ProfileConfigCatalogLoader::Load(
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
	else if (parsed.options.command == CliCommand::Backup) {
		CliSignalHandler signals;
		result = BackupCommand(
			loaded.catalog, paths, parsed.options, signals.Token());
		result.diagnostics.insert(result.diagnostics.begin(),
			loaded.diagnostics.begin(), loaded.diagnostics.end());
		if (signals.WasInterrupted()) result.code = OperationCode::Cancelled;
	}
	else if (parsed.options.command == CliCommand::RunSpecial) {
		CliSignalHandler signals;
		result = RunSpecialCommand(
			loaded.catalog, paths, parsed.options, signals.Token());
		result.diagnostics.insert(result.diagnostics.begin(),
			loaded.diagnostics.begin(), loaded.diagnostics.end());
		if (signals.WasInterrupted()) result.code = OperationCode::Cancelled;
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
