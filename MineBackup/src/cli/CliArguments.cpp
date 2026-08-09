#include "CliArguments.h"

#include "text_to_text.h"

#include <iostream>
#include <utility>

using namespace std;

namespace {

void AddParseError(CliParseResult& result, string eventId, string detail) {
	result.diagnostics.push_back({
		std::move(eventId), DiagnosticSeverity::Error, std::move(detail)});
}

} // namespace

string CliCommandName(CliCommand command) {
	switch (command) {
	case CliCommand::Help: return "help";
	case CliCommand::Version: return "version";
	case CliCommand::ProfileInit: return "profile.init";
	case CliCommand::ProfileValidate: return "profile.validate";
	case CliCommand::ProfileDiff: return "profile.diff";
	case CliCommand::ProfileApply: return "profile.apply";
	case CliCommand::ProfileExport: return "profile.export";
	case CliCommand::Doctor: return "doctor";
	case CliCommand::ConfigList: return "config.list";
	case CliCommand::ConfigShow: return "config.show";
	case CliCommand::WorldList: return "world.list";
	case CliCommand::HistoryList: return "history.list";
	case CliCommand::JobList: return "job.list";
	case CliCommand::JobShow: return "job.show";
	case CliCommand::JobRun: return "job.run";
	case CliCommand::Backup: return "backup";
	case CliCommand::Verify: return "verify";
	case CliCommand::Restore: return "restore";
	}
	return "unknown";
}

CliParseResult ParseCliArguments(const vector<wstring>& arguments) {
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
		if (argument == L"--force") { result.options.force = true; continue; }
		if (argument == L"--prune") { result.options.prune = true; continue; }
		if (argument == L"--confirm-prune") { result.options.confirmPrune = true; continue; }
		if (argument == L"--dry-run") { result.options.dryRun = true; continue; }
		if (argument == L"--latest") { result.options.latest = true; continue; }
		if (argument == L"--confirm") { result.options.confirm = true; continue; }
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
		if (argument == L"--job") {
			if (++index >= arguments.size()) {
				AddParseError(result, "cli.argument.missing_value", "--job requires a JobId.");
				return result;
			}
			result.options.jobId = arguments[index];
			continue;
		}
		if (argument == L"--comment") {
			if (++index >= arguments.size()) {
				AddParseError(result, "cli.argument.missing_value", "--comment requires text.");
				return result;
			}
			result.options.comment = arguments[index];
			continue;
		}
		if (argument == L"--backup") {
			if (++index >= arguments.size()) {
				AddParseError(result, "cli.argument.missing_value", "--backup requires an archive file.");
				return result;
			}
			result.options.backupPath = filesystem::path(arguments[index]);
			continue;
		}
		if (argument == L"--mode") {
			if (++index >= arguments.size()) {
				AddParseError(result, "cli.argument.missing_value", "--mode requires clean or overwrite.");
				return result;
			}
			result.options.restoreMode = arguments[index];
			if (result.options.restoreMode != L"clean"
				&& result.options.restoreMode != L"overwrite") {
				AddParseError(result, "cli.restore.mode.invalid", "--mode requires clean or overwrite.");
				return result;
			}
			continue;
		}
		if (argument == L"--file") {
			if (++index >= arguments.size()) {
				AddParseError(result, "cli.argument.missing_value", "--file requires a manifest path.");
				return result;
			}
			result.options.filePath = filesystem::path(arguments[index]);
			continue;
		}
		if (argument == L"--output") {
			if (++index >= arguments.size()) {
				AddParseError(result, "cli.argument.missing_value", "--output requires a path.");
				return result;
			}
			result.options.outputPath = filesystem::path(arguments[index]);
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
	if (positional == vector<wstring>{L"profile", L"init"}) result.options.command = CliCommand::ProfileInit;
	else if (positional == vector<wstring>{L"profile", L"validate"}) result.options.command = CliCommand::ProfileValidate;
	else if (positional == vector<wstring>{L"profile", L"diff"}) result.options.command = CliCommand::ProfileDiff;
	else if (positional == vector<wstring>{L"profile", L"apply"}) result.options.command = CliCommand::ProfileApply;
	else if (positional == vector<wstring>{L"profile", L"export"}) result.options.command = CliCommand::ProfileExport;
	else if (positional == vector<wstring>{L"doctor"}) result.options.command = CliCommand::Doctor;
	else if (positional == vector<wstring>{L"config", L"list"}) result.options.command = CliCommand::ConfigList;
	else if (positional == vector<wstring>{L"config", L"show"}) result.options.command = CliCommand::ConfigShow;
	else if (positional == vector<wstring>{L"world", L"list"}) result.options.command = CliCommand::WorldList;
	else if (positional == vector<wstring>{L"history", L"list"}) result.options.command = CliCommand::HistoryList;
	else if (positional == vector<wstring>{L"job", L"list"}) result.options.command = CliCommand::JobList;
	else if (positional == vector<wstring>{L"job", L"show"}) result.options.command = CliCommand::JobShow;
	else if (positional == vector<wstring>{L"job", L"run"}) result.options.command = CliCommand::JobRun;
	else if (positional == vector<wstring>{L"backup"}) result.options.command = CliCommand::Backup;
	else if (positional == vector<wstring>{L"verify"}) result.options.command = CliCommand::Verify;
	else if (positional == vector<wstring>{L"restore"}) result.options.command = CliCommand::Restore;
	else {
		AddParseError(result, "cli.command.invalid", "Unknown command or extra positional arguments.");
		return result;
	}
	if ((result.options.command == CliCommand::WorldList
			|| result.options.command == CliCommand::ConfigShow
			|| result.options.command == CliCommand::HistoryList
			|| result.options.command == CliCommand::Backup
			|| result.options.command == CliCommand::Verify
			|| result.options.command == CliCommand::Restore)
		&& result.options.configId.empty()) {
		AddParseError(result, "cli.config.required", "The command requires --config <ConfigId>.");
		return result;
	}
	if ((result.options.command == CliCommand::JobShow
			|| result.options.command == CliCommand::JobRun)
		&& result.options.jobId.empty()) {
		AddParseError(result, "cli.job.required", "The command requires --job <JobId>.");
		return result;
	}
	if ((result.options.command == CliCommand::HistoryList
			|| result.options.command == CliCommand::Backup
			|| result.options.command == CliCommand::Verify
			|| result.options.command == CliCommand::Restore)
		&& result.options.worldPath.empty()) {
		AddParseError(result, "cli.world.required", "The command requires --world <relative-path>.");
		return result;
	}
	if ((result.options.command == CliCommand::ProfileValidate
			|| result.options.command == CliCommand::ProfileDiff
			|| result.options.command == CliCommand::ProfileApply)
		&& result.options.filePath.empty()) {
		AddParseError(result, "cli.manifest.required", "The command requires --file <manifest.json>.");
		return result;
	}
	if ((result.options.command == CliCommand::ProfileInit
			|| result.options.command == CliCommand::ProfileExport)
		&& result.options.outputPath.empty()) {
		AddParseError(result, "cli.output.required", "The command requires --output <manifest.json>.");
		return result;
	}
	if (result.options.command == CliCommand::ProfileApply
		&& result.options.prune && !result.options.dryRun && !result.options.confirmPrune) {
		AddParseError(result, "cli.prune.confirmation_required",
			"profile apply --prune requires --confirm-prune unless --dry-run is used.");
		return result;
	}
	if (result.options.command == CliCommand::Verify
		|| result.options.command == CliCommand::Restore) {
		const bool explicitBackup = !result.options.backupPath.empty();
		if (explicitBackup == result.options.latest) {
			AddParseError(result, "cli.restore.backup_selection_required",
				"The command requires exactly one of --backup <file> or --latest.");
			return result;
		}
	}
	if (result.options.command == CliCommand::Restore) {
		if (result.options.dryRun && result.options.confirm) {
			AddParseError(result, "cli.restore.confirmation_conflict",
				"restore accepts --dry-run or --confirm, not both.");
			return result;
		}
		if (!result.options.dryRun && !result.options.confirm) {
			AddParseError(result, "cli.restore.confirmation_required",
				"restore requires --confirm for writes; use --dry-run to inspect the plan.");
			return result;
		}
	}
	result.success = true;
	return result;
}

void PrintCliHelp() {
	cout
		<< "MineBackup headless command line interface\n\n"
		<< "Usage:\n"
		<< "  minebackup-cli profile init --output <manifest.json> [--force]\n"
		<< "  minebackup-cli profile validate --file <manifest.json>\n"
		<< "  minebackup-cli [global options] profile diff --file <manifest.json> [--prune]\n"
		<< "  minebackup-cli [global options] profile apply --file <manifest.json> [--dry-run] [--prune --confirm-prune]\n"
		<< "  minebackup-cli [global options] profile export --output <manifest.json> [--force]\n"
		<< "  minebackup-cli [global options] doctor\n"
		<< "  minebackup-cli [global options] config list\n"
		<< "  minebackup-cli [global options] config show --config <ConfigId>\n"
		<< "  minebackup-cli [global options] world list --config <ConfigId>\n"
		<< "  minebackup-cli [global options] history list --config <ConfigId> --world <relative-path>\n"
		<< "  minebackup-cli [global options] job list\n"
		<< "  minebackup-cli [global options] job show --job <JobId>\n"
		<< "  minebackup-cli [global options] job run --job <JobId>\n"
		<< "  minebackup-cli [global options] backup --config <ConfigId> --world <relative-path> [--comment <text>]\n"
		<< "  minebackup-cli [global options] verify --config <ConfigId> --world <relative-path> (--backup <file> | --latest)\n"
		<< "  minebackup-cli [global options] restore --config <ConfigId> --world <relative-path> (--backup <file> | --latest) [--mode clean|overwrite] (--dry-run | --confirm)\n"
		<< "\n"
		<< "Global options:\n"
		<< "  --data-dir <path>  --json  --log-level <off|info|debug>\n"
		<< "  --no-network  --non-interactive  --help  --version\n";
}
