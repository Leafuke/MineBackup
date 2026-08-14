#include "ProfileRuntime.h"

#include "ExternalToolManager.h"
#include "JobDocument.h"
#include "JobRunner.h"
#include "ProcessRunner.h"
#include "ProfileConfigRepository.h"
#include "ProfileKnotLinkCommands.h"
#include "RuntimeCloudPostHook.h"
#include "RuntimeFileLock.h"
#include "RuntimeIntegration.h"
#include "RuntimeRetentionService.h"
#include "WorldIdentity.h"
#include "text_to_text.h"

#include <algorithm>
#include <filesystem>
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

BackupResult FailedBackup(OperationCode code, string eventId, string detail = {}) {
	BackupResult result;
	result.code = code;
	result.outcome = BackupOutcome::Rejected;
	result.diagnostics.push_back({std::move(eventId), DiagnosticSeverity::Error,
		std::move(detail)});
	return result;
}

} // namespace

struct ProfileRuntime::Implementation {
	ProfileConfigCatalog catalog;
	JobDocument jobs;
	HistoryRepository history;
	unique_ptr<RuntimeRetentionService> retention;
	shared_ptr<IHotBackupBridge> hotBackup;
	shared_ptr<HeadlessKnotLinkBridge> knotLink;
	shared_ptr<IRuntimeEventSink> eventSink;
	shared_ptr<ICloudPostHook> cloudPost;
	unique_ptr<BackupService> onlineBackup;
	unique_ptr<BackupService> offlineBackup;
	unique_ptr<RestoreService> onlineRestore;
	unique_ptr<RestoreService> offlineRestore;
};

ProfileRuntime::ProfileRuntime(
	AppPaths paths,
	ProfileRuntimeDependencies dependencies)
	: paths_(std::move(paths)), dependencies_(std::move(dependencies)) {
}

ProfileRuntime::~ProfileRuntime() {
	knotLinkRunning_.store(false, memory_order_release);
	if (knotLinkCommands_) knotLinkCommands_->Stop();
	if (implementation_ && implementation_->knotLink) {
		implementation_->knotLink->SetCommandHandler({});
		implementation_->knotLink->Stop();
	}
	knotLinkCommands_.reset();
	implementation_.reset();
}

ProfileRuntimeInitialization ProfileRuntime::Reload() {
	scoped_lock runtimeLock(operationMutex_, stateMutex_);
	ProfileRuntimeInitialization result;
	auto next = make_unique<Implementation>();
	auto catalog = ProfileConfigCatalogLoader::Load(paths_.ConfigFile());
	result.code = CatalogCode(catalog.status);
	result.diagnostics = std::move(catalog.diagnostics);
	if (!catalog.IsLoaded()) return result;
	next->catalog = std::move(catalog.catalog);

	const auto jobs = JobStorage::Load(paths_.JobsFile());
	result.diagnostics.insert(result.diagnostics.end(),
		jobs.diagnostics.begin(), jobs.diagnostics.end());
	if (jobs.status == JobStorage::LoadStatus::Loaded) {
		next->jobs = jobs.document;
		if (!JobStorage::ValidateReferences(
				next->jobs, next->catalog.configs, result.diagnostics)) {
			result.code = OperationCode::InvalidProfile;
			return result;
		}
	}
	else if (jobs.status != JobStorage::LoadStatus::Missing) {
		result.code = OperationCode::InvalidProfile;
		return result;
	}

	if (filesystem::exists(paths_.HistoryFile())
		&& !next->history.Load(paths_.HistoryFile(), next->catalog.configs)) {
		result.code = OperationCode::InvalidProfile;
		result.diagnostics.push_back({
			"history.load.invalid", DiagnosticSeverity::Error,
			wstring_to_utf8(paths_.HistoryFile().wstring())});
		return result;
	}
	next->retention = make_unique<RuntimeRetentionService>(
		next->history, paths_.HistoryFile(), next->catalog.configs);
	if (!dependencies_.noNetwork) {
		if (implementation_ && implementation_->knotLink && implementation_->eventSink) {
			next->knotLink = implementation_->knotLink;
			next->hotBackup = next->knotLink;
			next->eventSink = implementation_->eventSink;
		}
		else {
			auto bridge = make_shared<HeadlessKnotLinkBridge>();
			if (!bridge->Start()) {
				result.diagnostics.push_back({
					"knotlink.listener.unavailable", DiagnosticSeverity::Warning,
					"The local KnotLink ports are unavailable; locked worlds use the live-file fallback."});
			}
			next->hotBackup = bridge;
			next->knotLink = bridge;
			next->eventSink = bridge;
		}
		next->cloudPost = make_shared<SynchronousRcloneCloudPostHook>(
			paths_, next->history, [&catalog = next->catalog] {
				return catalog.ConfigSnapshot();
			});
	}

	Implementation* state = next.get();
	auto createBackup = [this, state](
		shared_ptr<IHotBackupBridge> hotBackup,
		shared_ptr<IRuntimeEventSink> eventSink,
		shared_ptr<ICloudPostHook> cloudPost) {
		BackupServiceDependencies backupDependencies;
		backupDependencies.paths = paths_;
		backupDependencies.ensureMigration = [](const BackupRequest&) {
			MigrationUnitResult migration;
			migration.status = MigrationStatus::NotNeeded;
			return migration;
		};
		backupDependencies.isFileLocked = IsRuntimeFileLocked;
		backupDependencies.hotBackup = std::move(hotBackup);
		backupDependencies.eventSink = std::move(eventSink);
		backupDependencies.cloudPost = std::move(cloudPost);
		backupDependencies.addHistory = [this, state](const HistoryEntry& entry) {
			const Config* config = state->catalog.FindConfig(entry.configId);
			const auto mutation = state->history.Mutate(
				entry.configId, paths_.HistoryFile(), state->catalog.configs, true,
				[&](vector<HistoryEntry>& entries) {
					for (auto& current : entries) {
						if (config && WorldIdentity::SameHistoryEntry(*config, current, entry)) {
							current = entry;
							return true;
						}
					}
					entries.push_back(entry);
					return true;
				});
			return mutation.changed && mutation.persisted;
		};
		backupDependencies.removeHistory = [this, state](
			const wstring& worldName,
			const wstring& backupFile) {
			bool persisted = true;
			for (const auto& [index, config] : state->catalog.configs) {
				(void)index;
				const auto mutation = state->history.Mutate(
					config.configId, paths_.HistoryFile(), state->catalog.configs, true,
					[&](vector<HistoryEntry>& entries) {
						const auto before = entries.size();
						erase_if(entries, [&](const HistoryEntry& entry) {
							return WorldIdentity::Matches(
								config, worldName, entry, backupFile);
						});
						return entries.size() != before;
					});
				persisted = mutation.persisted && persisted;
			}
			return persisted;
		};
		backupDependencies.enforceRetention = [state](
			const BackupRequest& request,
			const HistoryEntry& entry) {
			state->retention->Enforce(request, entry);
		};
		return make_unique<BackupService>(std::move(backupDependencies));
	};
	const auto disabledHotBackup = make_shared<NetworkDisabledKnotLinkBridge>();
	const auto disabledEventSink = make_shared<NoopRuntimeEventSink>();
	const auto disabledCloudPost = make_shared<NetworkDisabledCloudPostHook>();
	next->offlineBackup = createBackup(
		disabledHotBackup, disabledEventSink, disabledCloudPost);
	if (!dependencies_.noNetwork) {
		next->onlineBackup = createBackup(
			next->hotBackup, next->eventSink, next->cloudPost);
	}

	auto createRestore = [this](BackupService* backup) {
		RestoreServiceDependencies restoreDependencies;
		restoreDependencies.paths = paths_;
		restoreDependencies.isWorldOccupied = IsRuntimeWorldOccupied;
		restoreDependencies.backupBeforeRestore = [backup](
			const BackupRequest& request,
			stop_token stopToken) {
			return backup->Run(request, stopToken);
		};
		return make_unique<RestoreService>(std::move(restoreDependencies));
	};
	next->offlineRestore = createRestore(next->offlineBackup.get());
	if (next->onlineBackup) {
		next->onlineRestore = createRestore(next->onlineBackup.get());
	}
	implementation_ = std::move(next);
	if (implementation_->knotLink) {
		if (!knotLinkCommands_) {
			knotLinkCommands_ = make_unique<ProfileKnotLinkCommands>(
				*this, implementation_->knotLink);
		}
		else {
			knotLinkCommands_->SetBridge(implementation_->knotLink);
		}
		implementation_->knotLink->SetCommandHandler(
			[this](const auto& context) {
				return knotLinkCommands_->Handle(context);
			});
	}
	knotLinkRunning_.store(
		implementation_->knotLink && implementation_->knotLink->IsRunning(),
		memory_order_release);
	result.code = OperationCode::Success;
	return result;
}

bool ProfileRuntime::IsReady() const noexcept {
	return implementation_ != nullptr;
}

bool ProfileRuntime::NetworkEnabled() const noexcept {
	return !dependencies_.noNetwork;
}

bool ProfileRuntime::KnotLinkRunning() const {
	return knotLinkRunning_.load(memory_order_acquire);
}

size_t ProfileRuntime::ActiveKnotLinkOperationCount() {
	return knotLinkCommands_ ? knotLinkCommands_->ActiveOperationCount() : 0;
}

const AppPaths& ProfileRuntime::Paths() const noexcept { return paths_; }
const ProfileConfigCatalog& ProfileRuntime::Catalog() const { return implementation_->catalog; }
const JobDocument& ProfileRuntime::Jobs() const { return implementation_->jobs; }
const HistoryRepository& ProfileRuntime::History() const { return implementation_->history; }

ProfileConfigCatalog ProfileRuntime::CatalogSnapshot() const {
	lock_guard lock(stateMutex_);
	return implementation_ ? implementation_->catalog : ProfileConfigCatalog{};
}

vector<HistoryEntry> ProfileRuntime::HistorySnapshot(const wstring& configId) const {
	lock_guard lock(stateMutex_);
	if (!implementation_) return {};
	return *implementation_->history.EntriesForConfig(configId);
}

vector<wstring> ProfileRuntime::RestorePreserveSnapshot() const {
	lock_guard lock(stateMutex_);
	const auto profile = ProfileConfigRepository(paths_.ConfigFile()).Load();
	return profile.IsUsable() ? profile.restorePreserve : vector<wstring>{};
}

bool ProfileRuntime::SetBackupImportant(
	const wstring& configId,
	const wstring& worldPath,
	const wstring& backupFile,
	bool important) {
	lock_guard lock(stateMutex_);
	if (!implementation_ || !implementation_->catalog.FindConfig(configId)) return false;
	const auto mutation = implementation_->history.Mutate(
		configId, paths_.HistoryFile(), implementation_->catalog.configs, true,
		[&](vector<HistoryEntry>& entries) {
			for (auto& entry : entries) {
				if (WorldIdentity::Matches(
						*implementation_->catalog.FindConfig(configId),
						worldPath, entry, backupFile)) {
					entry.isImportant = important;
					return true;
				}
			}
			return false;
		});
	return mutation.changed && mutation.persisted;
}

optional<BackupRequest> ProfileRuntime::ResolveBackup(
	const wstring& configId,
	const wstring& worldPath,
	const wstring& comment) const {
	lock_guard lock(stateMutex_);
	return ResolveBackupUnlocked(configId, worldPath, comment);
}

optional<BackupRequest> ProfileRuntime::ResolveBackupUnlocked(
	const wstring& configId,
	const wstring& worldPath,
	const wstring& comment) const {
	if (!implementation_) return nullopt;
	const Config* config = implementation_->catalog.FindConfig(configId);
	wstring normalized;
	if (!config || !JobStorage::TryNormalizeWorldPath(worldPath, normalized)) {
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

ProfileRuntimePreflight ProfileRuntime::PreflightBackup(
	const BackupRequest& request,
	stop_token stopToken) const {
	ProfileRuntimePreflight result;
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
		request.config.zipPath, paths_, stopToken);
	if (!resolution.available) {
		wstring bootstrapError;
		if (!dependencies_.ensureSevenZip
			|| !dependencies_.ensureSevenZip(stopToken, bootstrapError)
			|| !ExternalToolManager::ResolveSevenZip(
				request.config.zipPath, paths_, stopToken).available) {
			result.code = stopToken.stop_requested()
				? OperationCode::Cancelled : OperationCode::ToolUnavailable;
			result.diagnostics.push_back({
				stopToken.stop_requested() ? "backup.cancelled" : "backup.tool.unavailable",
				stopToken.stop_requested() ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
				wstring_to_utf8(bootstrapError.empty()
					? resolution.diagnostic : bootstrapError)});
		}
	}
	return result;
}

BackupResult ProfileRuntime::RunBackup(
	const wstring& configId,
	const wstring& worldPath,
	const wstring& comment,
	stop_token stopToken,
	bool noNetwork) const {
	scoped_lock lock(operationMutex_, stateMutex_);
	const auto request = ResolveBackupUnlocked(configId, worldPath, comment);
	if (!request) {
		return FailedBackup(OperationCode::TargetNotFound,
			"world.not_found", wstring_to_utf8(worldPath));
	}
	return RunBackupRequestUnlocked(*request, stopToken, noNetwork);
}

BackupResult ProfileRuntime::RunBackupRequest(
	const BackupRequest& request,
	stop_token stopToken,
	bool noNetwork) const {
	scoped_lock lock(operationMutex_, stateMutex_);
	return RunBackupRequestUnlocked(request, stopToken, noNetwork);
}

BackupResult ProfileRuntime::RunBackupRequestUnlocked(
	const BackupRequest& request,
	stop_token stopToken,
	bool noNetwork) const {
	const auto preflight = PreflightBackup(request, stopToken);
	if (!IsSuccessful(preflight.code)) {
		BackupResult result;
		result.code = preflight.code;
		result.outcome = BackupOutcome::Rejected;
		result.diagnostics = preflight.diagnostics;
		return result;
	}
	BackupService* backup = !noNetwork && implementation_->onlineBackup
		? implementation_->onlineBackup.get() : implementation_->offlineBackup.get();
	return backup->Run(request, stopToken);
}

JobRunResult ProfileRuntime::RunJob(
	const wstring& jobId,
	stop_token stopToken,
	bool noNetwork) const {
	scoped_lock lock(operationMutex_, stateMutex_);
	JobRunResult missing;
	missing.jobId = jobId;
	const Job* job = implementation_ ? JobStorage::Find(implementation_->jobs, jobId) : nullptr;
	if (!job) {
		missing.code = OperationCode::TargetNotFound;
		missing.diagnostics.push_back({
			"job.not_found", DiagnosticSeverity::Error, wstring_to_utf8(jobId)});
		return missing;
	}
	BackupService* backup = !noNetwork && implementation_->onlineBackup
		? implementation_->onlineBackup.get() : implementation_->offlineBackup.get();
	JobRunner runner({
		[this](const JobBackupTarget& target) {
			return ResolveBackupUnlocked(
				target.configId, target.worldPath, target.comment);
		},
		[this, stopToken](const BackupRequest& request) {
			const auto current = PreflightBackup(request, stopToken);
			return JobPreflightResult{current.code, current.diagnostics};
		},
		[backup](const BackupRequest& request, stop_token token) {
			return backup->Run(request, token);
		},
		[](const ProcessSpec& process, stop_token token) {
			return ProcessRunner::Run(process, token);
		}});
	return runner.Run(*job, stopToken);
}

RestorePlan ProfileRuntime::Verify(
	const RestoreRequest& request,
	stop_token stopToken) const {
	lock_guard lock(stateMutex_);
	const auto resolution = ExternalToolManager::ResolveSevenZip(
		request.config.zipPath, paths_, stopToken);
	if (!resolution.available) {
		wstring error;
		if (!dependencies_.ensureSevenZip
			|| !dependencies_.ensureSevenZip(stopToken, error)
			|| !ExternalToolManager::ResolveSevenZip(
				request.config.zipPath, paths_, stopToken).available) {
			RestorePlan plan;
			plan.code = stopToken.stop_requested()
				? OperationCode::Cancelled : OperationCode::ToolUnavailable;
			plan.diagnostics.push_back({
				"restore.tool.unavailable", DiagnosticSeverity::Error,
				wstring_to_utf8(error.empty() ? resolution.diagnostic : error)});
			return plan;
		}
	}
	return implementation_->offlineRestore->Verify(request, stopToken);
}

RestoreResult ProfileRuntime::Restore(
	const RestoreRequest& request,
	bool dryRun,
	stop_token stopToken,
	bool noNetwork) const {
	scoped_lock lock(operationMutex_, stateMutex_);
	const auto verified = Verify(request, stopToken);
	if (!IsSuccessful(verified.code)) {
		RestoreResult result;
		result.code = verified.code == OperationCode::VerificationFailed
			? OperationCode::RestoreFailed : verified.code;
		result.plan = verified;
		result.diagnostics = verified.diagnostics;
		return result;
	}
	RestoreService* restore = !noNetwork && implementation_->onlineRestore
		? implementation_->onlineRestore.get() : implementation_->offlineRestore.get();
	return restore->Run(request, dryRun, stopToken);
}

HotRestoreResult ProfileRuntime::RunHotRestore(
	const HotRestoreRequest& hotRequest,
	const RestoreRequest& restoreRequest,
	stop_token stopToken) {
	lock_guard operationLock(operationMutex_);
	const auto verified = Verify(restoreRequest, stopToken);
	if (!IsSuccessful(verified.code)) {
		HotRestoreResult result;
		result.code = verified.code == OperationCode::Cancelled
			? OperationCode::Cancelled : OperationCode::RestoreFailed;
		result.restore.code = verified.code;
		result.restore.plan = verified;
		result.restore.diagnostics = verified.diagnostics;
		result.diagnostics = verified.diagnostics;
		return result;
	}
	shared_ptr<HeadlessKnotLinkBridge> bridge;
	{
		lock_guard stateLock(stateMutex_);
		if (implementation_) bridge = implementation_->knotLink;
	}
	if (!bridge || !bridge->IsRunning()) {
		HotRestoreResult result;
		result.code = OperationCode::RestoreFailed;
		result.diagnostics.push_back({
			"restore.hot.knotlink_unavailable", DiagnosticSeverity::Error, {}});
		return result;
	}
	return bridge->CoordinateRestore(
		hotRequest,
		[this, restoreRequest](stop_token restoreToken) {
			return Restore(restoreRequest, false, restoreToken);
		}, stopToken);
}
