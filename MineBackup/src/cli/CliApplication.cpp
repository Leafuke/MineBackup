#include "CliApplication.h"

#include "AppPaths.h"
#include "AtomicFileWriter.h"
#include "BackupService.h"
#include "CliArguments.h"
#include "CliRenderer.h"
#include "CliSignalHandler.h"
#include "CliToolBootstrap.h"
#include "ExternalToolManager.h"
#include "FolderRewindFormat.h"
#include "HistoryRepository.h"
#include "JobDocument.h"
#include "JobRunner.h"
#include "Logging.h"
#include "MineBackupVersion.h"
#include "OperationResult.h"
#include "ProfileConfigCatalog.h"
#include "ProfileConfigRepository.h"
#include "ProfileManifest.h"
#include "RestoreService.h"
#include "RuntimeIntegration.h"
#include "RuntimeCloudPostHook.h"
#include "RuntimeFileLock.h"
#include "RuntimeRetentionService.h"
#include "SingleInstanceService.h"
#include "SpecialTaskDocument.h"
#include "SpecialTaskRunner.h"
#include "text_to_text.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <utility>

using namespace std;

namespace {

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

	optional<BackupRequest> Resolve(
		const wstring& configId,
		const wstring& worldPath,
		const wstring& comment = {}) const {
		const Config* config = catalog_.FindConfig(configId);
		wstring normalized;
		if (!config
			|| !JobStorage::TryNormalizeWorldPath(worldPath, normalized)) {
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
		request.comment = comment;
		return request;
	}

	optional<BackupRequest> Resolve(const SpecialTaskTarget& target) const {
		return Resolve(target.configId, target.worldPath);
	}

	optional<BackupRequest> Resolve(const JobBackupTarget& target) const {
		return Resolve(target.configId, target.worldPath, target.comment);
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

	BackupResult Run(const BackupRequest& request, stop_token stopToken) const {
		return service_->Run(request, stopToken);
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
			{"comment", wstring_to_utf8(backup.historyEntry->comment)},
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
	auto request = runtime.Resolve(options.configId, options.worldPath, options.comment);
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

struct RestoreCommandContext {
	OperationCode code = OperationCode::Success;
	RestoreRequest request;
	vector<Diagnostic> diagnostics;
};

bool MatchesWorldHistory(
	const HistoryEntry& entry,
	const Config& config,
	const wstring& normalizedWorld) {
	return entry.worldName == normalizedWorld
		|| filesystem::path(entry.worldPath).lexically_normal()
			== (filesystem::path(config.saveRoot) / normalizedWorld).lexically_normal();
}

RestoreCommandContext ResolveRestoreCommand(
	const ProfileConfigCatalog& catalog,
	const AppPaths& paths,
	const CliOptions& options) {
	RestoreCommandContext context;
	const Config* config = catalog.FindConfig(options.configId);
	wstring normalized;
	if (!config
		|| !JobStorage::TryNormalizeWorldPath(options.worldPath, normalized)
		|| none_of(config->worlds.begin(), config->worlds.end(), [&](const auto& world) {
			return world.first == normalized;
		})) {
		context.code = OperationCode::TargetNotFound;
		context.diagnostics.push_back({"world.not_found", DiagnosticSeverity::Error,
			wstring_to_utf8(options.worldPath)});
		return context;
	}

	context.request.config = *config;
	context.request.world = {config->configId, normalized};
	context.request.mode = options.restoreMode == L"overwrite"
		? RestoreMode::Overwrite : RestoreMode::Clean;
	context.request.archive = options.backupPath;

	const auto profile = ProfileConfigRepository(paths.ConfigFile()).Load();
	if (!profile.IsUsable()) {
		context.code = OperationCode::InvalidProfile;
		context.diagnostics = profile.diagnostics;
		return context;
	}
	context.request.restorePreserve = profile.restorePreserve;
	if (!options.latest) return context;

	HistoryRepository history;
	if (filesystem::exists(paths.HistoryFile())
		&& !history.Load(paths.HistoryFile(), catalog.configs)) {
		context.code = OperationCode::InvalidProfile;
		context.diagnostics.push_back({"history.load.invalid", DiagnosticSeverity::Error,
			wstring_to_utf8(paths.HistoryFile().wstring())});
		return context;
	}
	FolderRewindFormat::StoragePaths storagePaths;
	if (!FolderRewindFormat::TryResolveStoragePaths(
			config->backupPath,
			normalized,
			(filesystem::path(config->saveRoot) / normalized).wstring(),
			storagePaths)) {
		context.code = OperationCode::InvalidArguments;
		context.diagnostics.push_back({"restore.storage.invalid", DiagnosticSeverity::Error,
			wstring_to_utf8(normalized)});
		return context;
	}
	const auto entries = history.EntriesForConfig(config->configId);
	for (auto current = entries->rbegin(); current != entries->rend(); ++current) {
		if (!MatchesWorldHistory(*current, *config, normalized)) continue;
		const auto candidate = storagePaths.backupSubDir / current->backupFile;
		error_code error;
		if (!filesystem::is_regular_file(candidate, error) || error) continue;
		context.request.archive = current->backupFile;
		return context;
	}
	context.code = OperationCode::TargetNotFound;
	context.diagnostics.push_back({"restore.latest.local_not_found", DiagnosticSeverity::Error,
		wstring_to_utf8(normalized)});
	return context;
}

void WriteRestorePlan(nlohmann::json& data, const RestorePlan& plan) {
	data["targetWorld"] = wstring_to_utf8(plan.targetWorld.wstring());
	data["selectedBackup"] = wstring_to_utf8(plan.selectedArchive.wstring());
	data["archiveChain"] = nlohmann::json::array();
	for (const auto& archive : plan.archiveChain) {
		data["archiveChain"].push_back(wstring_to_utf8(archive.wstring()));
	}
	data["mode"] = ToString(plan.mode);
	data["checkedArchiveCount"] = plan.checkedArchiveCount;
	data["usesExactSmartPlan"] = plan.usesExactSmartPlan;
}

CliResult RestoreOrVerifyCommand(
	const ProfileConfigCatalog& catalog,
	const AppPaths& paths,
	const CliOptions& options,
	stop_token stopToken) {
	const bool verify = options.command == CliCommand::Verify;
	CliResult result{verify ? "verify" : "restore", OperationCode::Success};
	auto context = ResolveRestoreCommand(catalog, paths, options);
	result.code = context.code;
	result.diagnostics = std::move(context.diagnostics);
	if (!IsSuccessful(result.code)) return result;

	const auto sevenZip = ExternalToolManager::ResolveSevenZip(
		context.request.config.zipPath, paths, stopToken);
	if (!sevenZip.available) {
		wstring bootstrapError;
		if (!EnsureCliSevenZip(paths, stopToken, bootstrapError)
			|| !ExternalToolManager::ResolveSevenZip(
				context.request.config.zipPath, paths, stopToken).available) {
			result.code = stopToken.stop_requested()
				? OperationCode::Cancelled : OperationCode::ToolUnavailable;
			result.diagnostics.push_back({
				stopToken.stop_requested() ? "restore.cancelled" : "restore.tool.unavailable",
				stopToken.stop_requested() ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
				wstring_to_utf8(bootstrapError.empty() ? sevenZip.diagnostic : bootstrapError)});
			return result;
		}
	}

	unique_ptr<CliBackupRuntime> backupRuntime;
	RestoreServiceDependencies dependencies;
	dependencies.paths = paths;
	dependencies.isWorldOccupied = [](const filesystem::path& world) {
		return IsRuntimeFileLocked(world / L"session.lock")
			|| IsRuntimeFileLocked(world / L"level.dat");
	};
	if (!verify && !options.dryRun && context.request.config.backupBefore) {
		backupRuntime = make_unique<CliBackupRuntime>(
			paths, catalog, options.noNetwork, stopToken);
		vector<Diagnostic> initialization;
		const auto initialized = backupRuntime->Initialize(initialization);
		result.diagnostics.insert(result.diagnostics.end(),
			initialization.begin(), initialization.end());
		if (!IsSuccessful(initialized)) {
			result.code = OperationCode::RestoreFailed;
			return result;
		}
		dependencies.backupBeforeRestore = [&](const BackupRequest& request, stop_token token) {
			return backupRuntime->Run(request, token);
		};
	}

	RestoreService service(std::move(dependencies));
	if (verify) {
		const auto plan = service.Verify(context.request, stopToken);
		result.code = plan.code;
		result.diagnostics.insert(result.diagnostics.end(),
			plan.diagnostics.begin(), plan.diagnostics.end());
		WriteRestorePlan(result.data, plan);
		result.data["rollbackAttempted"] = false;
		result.data["rollbackSucceeded"] = false;
		return result;
	}

	const auto restored = service.Run(context.request, options.dryRun, stopToken);
	result.code = restored.code;
	result.diagnostics.insert(result.diagnostics.end(),
		restored.diagnostics.begin(), restored.diagnostics.end());
	WriteRestorePlan(result.data, restored.plan);
	result.data["dryRun"] = restored.dryRun;
	result.data["rollbackAttempted"] = restored.rollbackAttempted;
	result.data["rollbackSucceeded"] = restored.rollbackSucceeded;
	if (restored.safetyBackup) {
		result.data["safetyBackup"] = {
			{"code", ToString(restored.safetyBackup->code)},
			{"archivePath", wstring_to_utf8(restored.safetyBackup->archivePath.wstring())}};
	}
	return result;
}

CliResult JobListCommand(const AppPaths& paths) {
	CliResult result{"job.list", OperationCode::Success};
	const auto loaded = JobStorage::Load(paths.JobsFile());
	result.data["jobs"] = nlohmann::json::array();
	if (loaded.status == JobStorage::LoadStatus::Missing) return result;
	if (!loaded.IsLoaded()) {
		result.code = OperationCode::InvalidProfile;
		result.diagnostics = loaded.diagnostics;
		return result;
	}
	for (const auto& job : loaded.document.jobs) {
		size_t stepCount = 0;
		for (const auto& stage : job.stages) stepCount += stage.steps.size();
		result.data["jobs"].push_back({
			{"jobId", wstring_to_utf8(job.jobId)},
			{"name", job.name},
			{"stageCount", job.stages.size()},
			{"stepCount", stepCount}});
	}
	return result;
}

CliResult JobShowCommand(const AppPaths& paths, const CliOptions& options) {
	CliResult result{"job.show", OperationCode::Success};
	const auto loaded = JobStorage::Load(paths.JobsFile());
	if (!loaded.IsLoaded()) {
		result.code = loaded.status == JobStorage::LoadStatus::Missing
			? OperationCode::TargetNotFound : OperationCode::InvalidProfile;
		result.diagnostics = loaded.diagnostics;
		if (loaded.status == JobStorage::LoadStatus::Missing) {
			result.diagnostics.push_back({"job.document.missing", DiagnosticSeverity::Error,
				wstring_to_utf8(paths.JobsFile().wstring())});
		}
		return result;
	}
	const Job* job = JobStorage::Find(loaded.document, options.jobId);
	if (!job) {
		result.code = OperationCode::TargetNotFound;
		result.diagnostics.push_back({"job.not_found", DiagnosticSeverity::Error,
			wstring_to_utf8(options.jobId)});
		return result;
	}
	JobDocument document;
	document.jobs.push_back(*job);
	const auto value = nlohmann::json::parse(JobStorage::Serialize(document));
	result.data["job"] = value.at("jobs").at(0);
	return result;
}

CliResult JobRunCommand(
	const ProfileConfigCatalog& catalog,
	const AppPaths& paths,
	const CliOptions& options,
	stop_token stopToken) {
	CliResult result{"job.run", OperationCode::Success};
	const auto loaded = JobStorage::Load(paths.JobsFile());
	if (!loaded.IsLoaded()) {
		result.code = loaded.status == JobStorage::LoadStatus::Missing
			? OperationCode::TargetNotFound : OperationCode::InvalidProfile;
		result.diagnostics = loaded.diagnostics;
		return result;
	}
	const Job* job = JobStorage::Find(loaded.document, options.jobId);
	if (!job) {
		result.code = OperationCode::TargetNotFound;
		result.diagnostics.push_back({"job.not_found", DiagnosticSeverity::Error,
			wstring_to_utf8(options.jobId)});
		return result;
	}
	if (!JobStorage::ValidateReferences(loaded.document, catalog.configs, result.diagnostics)) {
		result.code = OperationCode::InvalidProfile;
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
	JobRunner runner({
		[&](const JobBackupTarget& target) { return runtime.Resolve(target); },
		[&](const BackupRequest& request) {
			const auto current = runtime.Preflight(request);
			return JobPreflightResult{current.code, current.diagnostics};
		},
		[&](const BackupRequest& request, stop_token token) {
			return runtime.Run(request, token);
		},
		[](const ProcessSpec& spec, stop_token token) {
			return ProcessRunner::Run(spec, token);
		}});
	const auto run = runner.Run(*job, stopToken);
	result.code = run.code;
	result.diagnostics.insert(result.diagnostics.end(),
		run.diagnostics.begin(), run.diagnostics.end());
	result.data["jobId"] = wstring_to_utf8(run.jobId);
	result.data["stages"] = nlohmann::json::array();
	for (const auto& stage : run.stages) {
		nlohmann::json stageValue{
			{"stageId", wstring_to_utf8(stage.stageId)},
			{"code", ToString(stage.code)},
			{"skipped", stage.skipped},
			{"steps", nlohmann::json::array()}};
		for (const auto& step : stage.steps) {
			nlohmann::json diagnostics = nlohmann::json::array();
			for (const auto& item : step.diagnostics) {
				diagnostics.push_back({{"eventId", item.eventId},
					{"severity", ToString(item.severity)}, {"detail", item.detail}});
			}
			stageValue["steps"].push_back({
				{"stepId", wstring_to_utf8(step.stepId)},
				{"code", ToString(step.code)},
				{"diagnostics", std::move(diagnostics)}});
		}
		result.data["stages"].push_back(std::move(stageValue));
	}
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

CliResult ConfigShow(const ProfileConfigCatalog& catalog, const CliOptions& options) {
	CliResult result{"config.show", OperationCode::Success};
	const Config* config = catalog.FindConfig(options.configId);
	if (!config) {
		result.code = OperationCode::TargetNotFound;
		result.diagnostics.push_back({"config.not_found", DiagnosticSeverity::Error,
			wstring_to_utf8(options.configId)});
		return result;
	}
	ServerProfileManifest manifest;
	manifest.configs.push_back(*config);
	const auto value = nlohmann::json::parse(ProfileManifest::Serialize(manifest));
	result.data["config"] = value.at("configs").at(0);
	return result;
}

OperationCode ManifestCode(ProfileManifestStatus status) {
	switch (status) {
	case ProfileManifestStatus::Loaded: return OperationCode::Success;
	case ProfileManifestStatus::Missing: return OperationCode::TargetNotFound;
	case ProfileManifestStatus::Invalid:
	case ProfileManifestStatus::UnsupportedSchema:
	case ProfileManifestStatus::IoError:
		return OperationCode::InvalidProfile;
	}
	return OperationCode::InvalidProfile;
}

void AppendDiff(nlohmann::json& data, const vector<ProfileDiffItem>& diff) {
	data["changes"] = nlohmann::json::array();
	for (const auto& item : diff) {
		data["changes"].push_back({
			{"kind", item.kind},
			{"id", wstring_to_utf8(item.stableId)},
			{"action", ProfileManifest::ToString(item.action)},
			{"orphanHistoryCount", item.orphanHistoryCount}});
	}
	data["changeCount"] = diff.size();
}

CliResult ProfileInitCommand(const CliOptions& options) {
	CliResult result{"profile.init", OperationCode::Success};
	error_code error;
	const auto output = filesystem::absolute(options.outputPath, error).lexically_normal();
	if (error) {
		result.code = OperationCode::InvalidArguments;
		result.diagnostics.push_back({"profile.output.invalid", DiagnosticSeverity::Error,
			wstring_to_utf8(options.outputPath.wstring())});
		return result;
	}
	if (filesystem::exists(output) && !options.force) {
		result.code = OperationCode::InvalidArguments;
		result.diagnostics.push_back({"profile.output.exists", DiagnosticSeverity::Error,
			wstring_to_utf8(output.wstring())});
		return result;
	}
	const auto manifest = ProfileManifest::CreateTemplate();
	const auto write = AtomicFileWriter::WriteText(
		output, ProfileManifest::Serialize(manifest) + "\n");
	if (!write.success) {
		result.code = OperationCode::InvalidProfile;
		result.diagnostics.push_back({"profile.output.write_failed",
			DiagnosticSeverity::Error, wstring_to_utf8(write.error)});
		return result;
	}
	result.data = {
		{"output", wstring_to_utf8(output.wstring())},
		{"configCount", manifest.configs.size()},
		{"jobCount", manifest.jobs.jobs.size()}};
	return result;
}

CliResult ProfileValidateCommand(const CliOptions& options) {
	const auto loaded = ProfileManifest::Load(options.filePath);
	CliResult result{"profile.validate", ManifestCode(loaded.status)};
	result.diagnostics = loaded.diagnostics;
	result.data = {
		{"file", wstring_to_utf8(filesystem::absolute(options.filePath).wstring())},
		{"schemaVersion", loaded.manifest.schemaVersion},
		{"configCount", loaded.manifest.configs.size()},
		{"jobCount", loaded.manifest.jobs.jobs.size()}};
	return result;
}

CliResult ProfilePlanCommand(
	const AppPaths& paths,
	const CliOptions& options,
	bool apply) {
	const auto loaded = ProfileManifest::Load(options.filePath);
	CliResult result{apply ? "profile.apply" : "profile.diff", ManifestCode(loaded.status)};
	result.diagnostics = loaded.diagnostics;
	if (!loaded.IsLoaded()) return result;
	auto plan = ProfileManifest::Plan(paths, loaded.manifest, options.prune);
	result.code = plan.code;
	result.diagnostics.insert(result.diagnostics.end(),
		plan.diagnostics.begin(), plan.diagnostics.end());
	AppendDiff(result.data, plan.diff);
	result.data["prune"] = options.prune;
	result.data["dryRun"] = !apply || options.dryRun;
	if (!apply || options.dryRun || !IsSuccessful(plan.code)) return result;
	const auto applied = ProfileManifest::Apply(paths, plan);
	result.code = applied.code;
	result.diagnostics = std::move(applied.diagnostics);
	result.data["dryRun"] = false;
	return result;
}

CliResult ProfileExportCommand(const AppPaths& paths, const CliOptions& options) {
	CliResult result{"profile.export", OperationCode::Success};
	error_code error;
	const auto output = filesystem::absolute(options.outputPath, error).lexically_normal();
	if (error) {
		result.code = OperationCode::InvalidArguments;
		result.diagnostics.push_back({"profile.output.invalid", DiagnosticSeverity::Error,
			wstring_to_utf8(options.outputPath.wstring())});
		return result;
	}
	if (filesystem::exists(output) && !options.force) {
		result.code = OperationCode::InvalidArguments;
		result.diagnostics.push_back({"profile.output.exists", DiagnosticSeverity::Error,
			wstring_to_utf8(output.wstring())});
		return result;
	}
	const auto exported = ProfileManifest::Export(paths);
	result.code = ManifestCode(exported.status);
	result.diagnostics = exported.diagnostics;
	if (!exported.IsLoaded()) return result;
	const auto write = AtomicFileWriter::WriteText(
		output, ProfileManifest::Serialize(exported.manifest) + "\n");
	if (!write.success) {
		result.code = OperationCode::InvalidProfile;
		result.diagnostics.push_back({"profile.output.write_failed",
			DiagnosticSeverity::Error, wstring_to_utf8(write.error)});
		return result;
	}
	result.data = {
		{"output", wstring_to_utf8(output.wstring())},
		{"configCount", exported.manifest.configs.size()},
		{"jobCount", exported.manifest.jobs.jobs.size()}};
	return result;
}

} // namespace

int RunMineBackupCli(const vector<wstring>& arguments) {
	CliParseResult parsed = ParseCliArguments(arguments);
	if (!parsed.success) {
		CliResult error{"parse", OperationCode::InvalidArguments};
		error.diagnostics = std::move(parsed.diagnostics);
		RenderCliResult(error, parsed.options.json);
		return ToExitCode(error.code);
	}
	if (parsed.options.command == CliCommand::Help) {
		if (parsed.options.json) {
			CliResult help{"help", OperationCode::Success};
			help.data["usage"] = "minebackup-cli --help";
			RenderCliResult(help, true);
		}
		else {
			PrintCliHelp();
		}
		return 0;
	}
	if (parsed.options.command == CliCommand::Version) {
		if (parsed.options.json) {
			CliResult version{"version", OperationCode::Success};
			version.data["version"] = MINEBACKUP_VERSION_STRING;
			RenderCliResult(version, true);
		}
		else {
			cout << "minebackup-cli " MINEBACKUP_VERSION_STRING "\n";
		}
		return 0;
	}
	if (parsed.options.command == CliCommand::ProfileInit
		|| parsed.options.command == CliCommand::ProfileValidate) {
		CliResult result = parsed.options.command == CliCommand::ProfileInit
			? ProfileInitCommand(parsed.options)
			: ProfileValidateCommand(parsed.options);
		RenderCliResult(result, parsed.options.json);
		return ToExitCode(result.code);
	}

	AppPaths paths;
	wstring pathError;
	if (!ResolveAppPaths(
			AppPathRequest{parsed.options.dataDirectory},
			GetExecutablePath(),
			paths,
			pathError)) {
		CliResult error{CliCommandName(parsed.options.command), OperationCode::InvalidProfile};
		error.diagnostics.push_back({
			"profile.path.invalid", DiagnosticSeverity::Error, wstring_to_utf8(pathError)});
		RenderCliResult(error, parsed.options.json);
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
		CliResult error{CliCommandName(parsed.options.command),
			lock == InstanceAcquireResult::AlreadyRunning
				? OperationCode::ProfileBusy : OperationCode::InvalidProfile};
		error.diagnostics.push_back({
			lock == InstanceAcquireResult::AlreadyRunning
				? "profile.lock.busy" : "profile.lock.failed",
			DiagnosticSeverity::Error,
			wstring_to_utf8(lockError)});
		RenderCliResult(error, parsed.options.json);
		minebackup::logging::Shutdown();
		return ToExitCode(error.code);
	}

	CliResult result;
	if (parsed.options.command == CliCommand::ProfileDiff) {
		result = ProfilePlanCommand(paths, parsed.options, false);
	}
	else if (parsed.options.command == CliCommand::ProfileApply) {
		result = ProfilePlanCommand(paths, parsed.options, true);
	}
	else if (parsed.options.command == CliCommand::ProfileExport) {
		result = ProfileExportCommand(paths, parsed.options);
	}
	else {
		auto loaded = ProfileConfigCatalogLoader::Load(
			paths.ConfigFile(), paths.SpecialTasksFile());
	if (parsed.options.command == CliCommand::Doctor) {
		result = Doctor(loaded, paths, parsed.options);
	}
	else if (!loaded.IsLoaded()) {
		result.command = CliCommandName(parsed.options.command);
		result.code = CatalogCode(loaded.status);
		result.diagnostics = loaded.diagnostics;
	}
	else if (parsed.options.command == CliCommand::ConfigList) {
		result = ConfigList(loaded.catalog);
		result.diagnostics = loaded.diagnostics;
	}
	else if (parsed.options.command == CliCommand::ConfigShow) {
		result = ConfigShow(loaded.catalog, parsed.options);
		result.diagnostics.insert(result.diagnostics.begin(),
			loaded.diagnostics.begin(), loaded.diagnostics.end());
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
	else if (parsed.options.command == CliCommand::JobList) {
		result = JobListCommand(paths);
		result.diagnostics.insert(result.diagnostics.begin(),
			loaded.diagnostics.begin(), loaded.diagnostics.end());
	}
	else if (parsed.options.command == CliCommand::JobShow) {
		result = JobShowCommand(paths, parsed.options);
		result.diagnostics.insert(result.diagnostics.begin(),
			loaded.diagnostics.begin(), loaded.diagnostics.end());
	}
	else if (parsed.options.command == CliCommand::JobRun) {
		CliSignalHandler signals;
		result = JobRunCommand(loaded.catalog, paths, parsed.options, signals.Token());
		result.diagnostics.insert(result.diagnostics.begin(),
			loaded.diagnostics.begin(), loaded.diagnostics.end());
		if (signals.WasInterrupted()) result.code = OperationCode::Cancelled;
	}
	else if (parsed.options.command == CliCommand::Backup) {
		CliSignalHandler signals;
		result = BackupCommand(
			loaded.catalog, paths, parsed.options, signals.Token());
		result.diagnostics.insert(result.diagnostics.begin(),
			loaded.diagnostics.begin(), loaded.diagnostics.end());
		if (signals.WasInterrupted()) result.code = OperationCode::Cancelled;
	}
	else if (parsed.options.command == CliCommand::Verify
		|| parsed.options.command == CliCommand::Restore) {
		CliSignalHandler signals;
		result = RestoreOrVerifyCommand(
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
		result.command = CliCommandName(parsed.options.command);
		result.code = OperationCode::InvalidArguments;
		result.diagnostics.push_back({
			"cli.command.not_implemented", DiagnosticSeverity::Error,
			"Execution commands are added in the next implementation stage."});
	}
	}
	RenderCliResult(result, parsed.options.json);
	minebackup::logging::Shutdown();
	return ToExitCode(result.code);
}
