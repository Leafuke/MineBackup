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
#include "Logging.h"
#include "MineBackupVersion.h"
#include "OperationResult.h"
#include "ProfileConfigCatalog.h"
#include "ProfileConfigRepository.h"
#include "ProfileManifest.h"
#include "ProfileRuntime.h"
#include "ProcessRunner.h"
#include "RestoreService.h"
#include "RuntimeIntegration.h"
#include "RuntimeFileLock.h"
#include "SingleInstanceService.h"
#include "text_to_text.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <utility>

using namespace std;

namespace {

struct DirectoryProbe {
	bool writable = false;
	string detail;
};

DirectoryProbe ProbeWritableDirectory(const filesystem::path& directory) {
	DirectoryProbe result;
	error_code error;
	filesystem::create_directories(directory, error);
	if (error || !filesystem::is_directory(directory, error) || error) {
		result.detail = error ? error.message() : "not a directory";
		return result;
	}
	const auto nonce = chrono::steady_clock::now().time_since_epoch().count();
	const filesystem::path probe = directory
		/ (L".minebackup-doctor-write-probe-" + to_wstring(nonce));
	{
		ofstream output(probe, ios::binary | ios::trunc);
		if (!output.is_open()) {
			result.detail = "unable to create a probe file";
			return result;
		}
		output << "minebackup-doctor\n";
		output.flush();
		if (!output.good()) {
			result.detail = "unable to flush a probe file";
			output.close();
			filesystem::remove(probe, error);
			return result;
		}
	}
	filesystem::remove(probe, error);
	if (error) {
		result.detail = "probe file could not be removed: " + error.message();
		return result;
	}
	result.writable = true;
	result.detail = "ready";
	return result;
}

const char* JobLoadStatusName(JobStorage::LoadStatus status) {
	switch (status) {
	case JobStorage::LoadStatus::Missing: return "missing";
	case JobStorage::LoadStatus::Loaded: return "loaded";
	case JobStorage::LoadStatus::Invalid: return "invalid";
	case JobStorage::LoadStatus::UnsupportedSchema: return "unsupported_schema";
	case JobStorage::LoadStatus::IoError: return "io_error";
	}
	return "invalid";
}

wstring RcloneRemoteName(const wstring& remotePath) {
	const auto separator = remotePath.find(L':');
	if (separator == wstring::npos || separator == 0) return {};
	return remotePath.substr(0, separator) + L":";
}

bool RcloneRemoteConfigured(
	const filesystem::path& executable,
	const wstring& remoteName,
	string& detail) {
	ProcessSpec spec;
	spec.executable = executable;
	spec.arguments = {L"listremotes"};
	spec.timeout = chrono::seconds(15);
	spec.maximumCapturedBytes = 512u * 1024u;
	const auto process = ProcessRunner::Run(spec);
	if (process.status != ProcessStatus::Succeeded) {
		detail = wstring_to_utf8(process.error);
		if (detail.empty()) detail = "rclone listremotes failed";
		return false;
	}
	const string expected = wstring_to_utf8(remoteName);
	istringstream lines(process.standardOutput);
	for (string line; getline(lines, line);) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line == expected) {
			detail = "configured";
			return true;
		}
	}
	detail = "configured rclone remote was not found";
	return false;
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
	if (!JobStorage::TryNormalizeWorldPath(options.worldPath, normalized)
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

size_t CountIgnoredSpecialSections(const filesystem::path& configFile) {
	ifstream input(configFile, ios::binary);
	size_t count = 0;
	for (string line; getline(input, line);) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.rfind("[SpCfg", 0) == 0 && line.ends_with(']')) ++count;
	}
	return count;
}


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
	ProfileRuntime& runtime,
	const CliOptions& options,
	stop_token stopToken) {
	return RenderBackupResult(runtime.RunBackup(
		options.configId, options.worldPath, options.comment, stopToken));
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
	const ProfileRuntime& runtime,
	const CliOptions& options) {
	RestoreCommandContext context;
	const auto& catalog = runtime.Catalog();
	const auto& paths = runtime.Paths();
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
	const auto entries = runtime.History().EntriesForConfig(config->configId);
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
	ProfileRuntime& runtime,
	const CliOptions& options,
	stop_token stopToken) {
	const bool verify = options.command == CliCommand::Verify;
	CliResult result{verify ? "verify" : "restore", OperationCode::Success};
	auto context = ResolveRestoreCommand(runtime, options);
	result.code = context.code;
	result.diagnostics = std::move(context.diagnostics);
	if (!IsSuccessful(result.code)) return result;

	if (verify) {
		const auto plan = runtime.Verify(context.request, stopToken);
		result.code = plan.code;
		result.diagnostics.insert(result.diagnostics.end(),
			plan.diagnostics.begin(), plan.diagnostics.end());
		WriteRestorePlan(result.data, plan);
		result.data["rollbackAttempted"] = false;
		result.data["rollbackSucceeded"] = false;
		return result;
	}

	const auto restored = runtime.Restore(context.request, options.dryRun, stopToken);
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
	ProfileRuntime& runtime,
	const CliOptions& options,
	stop_token stopToken) {
	CliResult result{"job.run", OperationCode::Success};
	const auto run = runtime.RunJob(options.jobId, stopToken);
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

CliResult Doctor(
	const ProfileCatalogLoadResult& loaded,
	const AppPaths& paths,
	const CliOptions& options) {
	CliResult result{"doctor", CatalogCode(loaded.status)};
	result.diagnostics = loaded.diagnostics;
	result.data["profileIdentity"] = wstring_to_utf8(paths.profileIdentity);
	result.data["configFile"] = wstring_to_utf8(paths.ConfigFile().wstring());
	result.data["jobsFile"] = wstring_to_utf8(paths.JobsFile().wstring());
	result.data["historyFile"] = wstring_to_utf8(paths.HistoryFile().wstring());
	result.data["configCount"] = loaded.catalog.configs.size();
	const auto jobs = JobStorage::Load(paths.JobsFile());
	result.data["jobs"] = {
		{"status", JobLoadStatusName(jobs.status)},
		{"count", jobs.document.jobs.size()},
		{"referencesValid", false}};
	result.diagnostics.insert(result.diagnostics.end(),
		jobs.diagnostics.begin(), jobs.diagnostics.end());
	if (jobs.status == JobStorage::LoadStatus::Loaded) {
		vector<Diagnostic> referenceDiagnostics;
		const bool referencesValid = JobStorage::ValidateReferences(
			jobs.document, loaded.catalog.configs, referenceDiagnostics);
		result.data["jobs"]["referencesValid"] = referencesValid;
		result.diagnostics.insert(result.diagnostics.end(),
			referenceDiagnostics.begin(), referenceDiagnostics.end());
		if (!referencesValid) result.code = OperationCode::InvalidProfile;
	}
	else if (jobs.status == JobStorage::LoadStatus::Missing) {
		result.data["jobs"]["referencesValid"] = true;
	}
	else {
		result.code = OperationCode::InvalidProfile;
	}

	const auto profile = ProfileConfigRepository(paths.ConfigFile()).Load();
	vector<wstring> effectiveRestorePreserve = profile.restorePreserve;
	if (none_of(effectiveRestorePreserve.begin(), effectiveRestorePreserve.end(),
		[](const wstring& item) { return item == L"session.lock"; })) {
		effectiveRestorePreserve.push_back(L"session.lock");
	}
	result.data["restorePreserve"] = nlohmann::json::array();
	for (const auto& item : effectiveRestorePreserve) {
		result.data["restorePreserve"].push_back(wstring_to_utf8(item));
	}
	result.data["sessionLockImplicitlyPreserved"] =
		find(profile.restorePreserve.begin(), profile.restorePreserve.end(), L"session.lock")
			== profile.restorePreserve.end();
	const size_t ignoredSections = CountIgnoredSpecialSections(paths.ConfigFile());
	const bool ignoredDocument = filesystem::is_regular_file(paths.SpecialTasksFile());
	result.data["legacySpecialConfigSectionsIgnored"] = ignoredSections;
	result.data["legacySpecialTasksFileIgnored"] = ignoredDocument;
	if (ignoredSections > 0 || ignoredDocument) {
		result.diagnostics.push_back({"special.legacy.ignored", DiagnosticSeverity::Warning,
			"Legacy SpecialConfig data is retained on disk but is not read or executed."});
	}
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
		const auto backupProbe = backupRootValid
			? ProbeWritableDirectory(backupRoot)
			: DirectoryProbe{false, "path is not absolute"};
		nlohmann::json pathStatus{
			{"configId", wstring_to_utf8(config.configId)},
			{"saveRoot", wstring_to_utf8(config.saveRoot)},
			{"saveRootReady", saveRootReady},
			{"backupRoot", wstring_to_utf8(config.backupPath)},
			{"backupRootAbsolute", backupRootValid},
			{"backupRootWritable", backupProbe.writable},
			{"backupRootDetail", backupProbe.detail},
			{"pendingLocalBinding", config.pendingLocalBinding},
			{"worlds", nlohmann::json::array()}};
		bool worldsReady = true;
		for (const auto& [relativePath, description] : config.worlds) {
			(void)description;
			const filesystem::path world = saveRoot / relativePath;
			error_code worldError;
			const bool exists = filesystem::is_directory(world, worldError) && !worldError;
			const bool occupied = exists && IsRuntimeWorldOccupied(world);
			pathStatus["worlds"].push_back({
				{"relativePath", wstring_to_utf8(relativePath)},
				{"path", wstring_to_utf8(world.wstring())},
				{"exists", exists},
				{"occupied", occupied},
				{"coldRestoreReady", exists && !occupied}});
			worldsReady &= exists;
			if (occupied) result.diagnostics.push_back({
				"profile.world.occupied", DiagnosticSeverity::Warning,
				wstring_to_utf8(config.configId + L":" + relativePath)});
		}
		pathStatus["worldsReady"] = worldsReady;
		result.data["paths"].push_back(std::move(pathStatus));
		if (!saveRootReady || !backupRootValid || !backupProbe.writable
			|| !worldsReady || config.pendingLocalBinding) {
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
		if (config.cloudSyncEnabled) {
			const auto rclone = ExternalToolManager::ResolveRclone(
				config.rclonePath, paths);
			const wstring remoteName = RcloneRemoteName(config.rcloneRemotePath);
			string remoteDetail;
			const bool remoteConfigured = rclone.available && !remoteName.empty()
				&& RcloneRemoteConfigured(rclone.executable, remoteName, remoteDetail);
			if (remoteName.empty()) remoteDetail = "remote must use the name:path form";
			result.data["tools"].push_back({
				{"configId", wstring_to_utf8(config.configId)},
				{"tool", "rclone"},
				{"available", rclone.available},
				{"path", wstring_to_utf8(rclone.executable.wstring())},
				{"detail", wstring_to_utf8(rclone.diagnostic)},
				{"remoteName", wstring_to_utf8(remoteName)},
				{"remoteConfigured", remoteConfigured},
				{"remoteDetail", remoteDetail}});
			missingTool |= !rclone.available;
			if (rclone.available && !remoteConfigured) {
				result.code = OperationCode::InvalidProfile;
				result.diagnostics.push_back({
					"cloud.remote.not_configured", DiagnosticSeverity::Error,
					wstring_to_utf8(config.configId)});
			}
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
		auto loaded = ProfileConfigCatalogLoader::Load(paths.ConfigFile());
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
	else if (parsed.options.command == CliCommand::JobRun
		|| parsed.options.command == CliCommand::Backup
		|| parsed.options.command == CliCommand::Verify
		|| parsed.options.command == CliCommand::Restore) {
		CliSignalHandler signals;
		ProfileRuntime runtime(paths, {
			parsed.options.noNetwork,
			[&paths](stop_token token, wstring& error) {
				return EnsureCliSevenZip(paths, token, error);
			}});
		const auto initialized = runtime.Reload();
		if (!IsSuccessful(initialized.code)) {
			result.command = CliCommandName(parsed.options.command);
			result.code = initialized.code;
			result.diagnostics = initialized.diagnostics;
		}
		else if (parsed.options.command == CliCommand::JobRun) {
			result = JobRunCommand(runtime, parsed.options, signals.Token());
			result.diagnostics.insert(result.diagnostics.begin(),
				initialized.diagnostics.begin(), initialized.diagnostics.end());
		}
		else if (parsed.options.command == CliCommand::Backup) {
			result = BackupCommand(runtime, parsed.options, signals.Token());
			result.diagnostics.insert(result.diagnostics.begin(),
				initialized.diagnostics.begin(), initialized.diagnostics.end());
		}
		else {
			result = RestoreOrVerifyCommand(runtime, parsed.options, signals.Token());
			result.diagnostics.insert(result.diagnostics.begin(),
				initialized.diagnostics.begin(), initialized.diagnostics.end());
		}
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
