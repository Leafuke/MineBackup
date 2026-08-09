#include "RestoreService.h"

#include "BackupManagerInternal.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "JobDocument.h"
#include "PathRuleSet.h"
#include "text_to_text.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace std;
using namespace BackupManagerInternal;

namespace {

struct SmartRestoreArchiveGroup {
	filesystem::path archive;
	vector<wstring> files;
};

struct SmartFilePlan {
	vector<SmartRestoreArchiveGroup> archiveGroups;
};

Diagnostic Failure(string eventId, string detail = {}) {
	return {std::move(eventId), DiagnosticSeverity::Error, std::move(detail)};
}

bool IsWithin(const filesystem::path& root, const filesystem::path& candidate) {
	const auto normalizedRoot = filesystem::absolute(root).lexically_normal();
	const auto normalizedCandidate = filesystem::absolute(candidate).lexically_normal();
	auto rootPart = normalizedRoot.begin();
	auto candidatePart = normalizedCandidate.begin();
	for (; rootPart != normalizedRoot.end() && candidatePart != normalizedCandidate.end();
		++rootPart, ++candidatePart) {
#ifdef _WIN32
		wstring left = rootPart->wstring();
		wstring right = candidatePart->wstring();
		transform(left.begin(), left.end(), left.begin(), ::towlower);
		transform(right.begin(), right.end(), right.begin(), ::towlower);
		if (left != right) return false;
#else
		if (*rootPart != *candidatePart) return false;
#endif
	}
	return rootPart == normalizedRoot.end();
}

filesystem::path SafeWorkspacePath(const filesystem::path& target) {
	const filesystem::path base = target.parent_path()
		/ (target.filename().wstring() + L"-MineBackup-Restore-Snapshot");
	filesystem::path candidate = base;
	error_code error;
	for (int suffix = 1; filesystem::exists(candidate, error); ++suffix) {
		candidate = filesystem::path(base.wstring() + L"-" + to_wstring(suffix));
	}
	return candidate;
}

bool PrepareCleanWorkspace(
	const filesystem::path& target,
	filesystem::path& snapshot,
	string& errorText) {
	error_code error;
	if (!filesystem::exists(target, error)) {
		filesystem::create_directories(target, error);
		if (error) errorText = error.message();
		return !error;
	}
	if (error) {
		errorText = error.message();
		return false;
	}
	snapshot = SafeWorkspacePath(target);
	filesystem::rename(target, snapshot, error);
	if (error) {
		errorText = error.message();
		snapshot.clear();
		return false;
	}
	filesystem::create_directories(target, error);
	if (!error) return true;
	const string createError = error.message();
	error.clear();
	filesystem::rename(snapshot, target, error);
	if (!error) snapshot.clear();
	errorText = error ? createError + ";rollback=" + error.message() : createError;
	return false;
}

void CleanupMarkers(const filesystem::path& target) {
	for (const wchar_t* marker : {
		FolderRewindFormat::kInternalRestoreMarkerDirectoryName,
		L"__MineBackup_Internal"}) {
		error_code error;
		const filesystem::path path = target / marker;
		if (!filesystem::exists(path, error) || error) continue;
		ClearReadonlyAttributesRecursively(path);
		filesystem::remove_all(path, error);
	}
}

void CopyPreserved(
	const filesystem::path& snapshot,
	const filesystem::path& target,
	vector<wstring> rules) {
	if (none_of(rules.begin(), rules.end(), [](const wstring& item) {
		return item == L"session.lock";
	})) rules.push_back(L"session.lock");
	const PathRuleSet matcher(rules);
	error_code error;
	for (const auto& entry : filesystem::recursive_directory_iterator(
		snapshot, filesystem::directory_options::skip_permission_denied, error)) {
		if (error) break;
		if (!entry.is_directory()
			|| !matcher.MatchesSelfOrAncestor(entry.path(), snapshot)) continue;
		const auto relative = filesystem::relative(entry.path(), snapshot, error);
		if (!error) filesystem::create_directories(target / relative, error);
	}
	error.clear();
	for (const auto& entry : filesystem::recursive_directory_iterator(
		snapshot, filesystem::directory_options::skip_permission_denied, error)) {
		if (error) break;
		if (!entry.is_regular_file()
			|| !matcher.MatchesSelfOrAncestor(entry.path(), snapshot)) continue;
		const auto relative = filesystem::relative(entry.path(), snapshot, error);
		if (error) continue;
		const auto destination = target / relative;
		filesystem::create_directories(destination.parent_path(), error);
		if (!filesystem::exists(destination, error) || error) {
			error.clear();
			filesystem::copy_file(entry.path(), destination,
				filesystem::copy_options::overwrite_existing, error);
		}
	}
}

bool CommitCleanWorkspace(
	const filesystem::path& target,
	const filesystem::path& snapshot,
	const vector<wstring>& preserve,
	string& errorText) {
	try {
		CleanupMarkers(target);
		if (!snapshot.empty() && filesystem::exists(snapshot)) {
			CopyPreserved(snapshot, target, preserve);
			ClearReadonlyAttributesRecursively(snapshot);
			filesystem::remove_all(snapshot);
		}
		return true;
	}
	catch (const exception& error) {
		errorText = error.what();
		return false;
	}
}

bool RollbackCleanWorkspace(
	const filesystem::path& target,
	const filesystem::path& snapshot,
	string& errorText) {
	if (snapshot.empty()) {
		errorText = "snapshot path is empty";
		return false;
	}
	error_code error;
	if (!filesystem::exists(snapshot, error) || error) {
		errorText = "snapshot is missing";
		return false;
	}
	if (filesystem::exists(target, error) && !error) {
		ClearReadonlyAttributesRecursively(target);
		filesystem::remove_all(target, error);
	}
	if (error) {
		errorText = error.message();
		return false;
	}
	filesystem::rename(snapshot, target, error);
	if (error) errorText = error.message();
	return !error;
}

bool BuildMetadataChain(
	const filesystem::path& metadata,
	const filesystem::path& backupRoot,
	const filesystem::path& selected,
	vector<filesystem::path>& chain,
	string& eventId) {
	set<wstring> visited;
	wstring current = selected.filename().wstring();
	for (;;) {
		if (!visited.insert(current).second) {
			eventId = "restore.metadata.cycle";
			return false;
		}
		FolderRewindFormat::ChangeRecord record;
		if (!FolderRewindMetadataStore::LoadRecord(metadata, current, record)) {
			eventId = "restore.metadata.missing";
			return false;
		}
		const filesystem::path archive = backupRoot / current;
		if (!filesystem::is_regular_file(archive)) {
			eventId = "restore.chain.archive_missing";
			return false;
		}
		chain.push_back(archive);
		const wstring type = record.backupType.empty() ? current : record.backupType;
		if (!FolderRewindFormat::IsSmartBackupType(type)) break;
		if (record.previousBackupFileName.empty()) {
			eventId = "restore.chain.full_base_missing";
			return false;
		}
		current = record.previousBackupFileName;
	}
	reverse(chain.begin(), chain.end());
	if (chain.empty()
		|| !FolderRewindFormat::IsFullLikeBackupType(chain.front().filename().wstring())) {
		eventId = "restore.chain.full_base_missing";
		return false;
	}
	return true;
}

bool BuildSmartFilePlan(
	const filesystem::path& metadata,
	const vector<filesystem::path>& chain,
	SmartFilePlan& plan) {
	if (chain.empty()) return false;
	FolderRewindFormat::ChangeRecord base;
	if (!FolderRewindMetadataStore::LoadRecord(
			metadata, chain.front().filename().wstring(), base)
		|| base.fullFileList.empty()) return false;
	map<wstring, wstring> owners;
	for (const auto& file : base.fullFileList) {
		if (!file.empty()) owners[file] = chain.front().filename().wstring();
	}
	for (size_t index = 1; index < chain.size(); ++index) {
		FolderRewindFormat::ChangeRecord record;
		if (!FolderRewindMetadataStore::LoadRecord(
				metadata, chain[index].filename().wstring(), record)) return false;
		for (const auto& file : record.deletedFiles) owners.erase(file);
		for (const auto& file : record.addedFiles) owners[file] = record.archiveFileName;
		for (const auto& file : record.modifiedFiles) owners[file] = record.archiveFileName;
		const set<wstring> expected(record.fullFileList.begin(), record.fullFileList.end());
		if (owners.size() != expected.size()
			|| any_of(owners.begin(), owners.end(), [&](const auto& owner) {
				return !expected.contains(owner.first);
			})) return false;
	}
	map<wstring, filesystem::path> archives;
	map<wstring, size_t> order;
	for (size_t index = 0; index < chain.size(); ++index) {
		archives[chain[index].filename().wstring()] = chain[index];
		order[chain[index].filename().wstring()] = index;
	}
	map<wstring, vector<wstring>> grouped;
	for (const auto& [file, owner] : owners) grouped[owner].push_back(file);
	for (auto& [owner, files] : grouped) {
		if (!archives.contains(owner)) return false;
		sort(files.begin(), files.end());
		plan.archiveGroups.push_back({archives[owner], std::move(files)});
	}
	sort(plan.archiveGroups.begin(), plan.archiveGroups.end(), [&](const auto& left, const auto& right) {
		return order[left.archive.filename().wstring()]
			< order[right.archive.filename().wstring()];
	});
	return true;
}

bool ExtractChain(
	const RestorePlan& plan,
	const filesystem::path& metadata,
	const filesystem::path& target,
	const ArchiveRunner& runner,
	const AppPaths& paths,
	bool lowPriority) {
	if (!plan.usesExactSmartPlan) {
		for (const auto& archive : plan.archiveChain) {
			if (runner.Execute({L"x", archive.wstring(), L"-o" + target.wstring(), L"-y"},
				{}, lowPriority).status != ProcessStatus::Succeeded) return false;
		}
		return true;
	}
	SmartFilePlan smart;
	if (!BuildSmartFilePlan(metadata, plan.archiveChain, smart)) return false;
	error_code error;
	filesystem::create_directories(paths.runtimeRoot, error);
	for (size_t index = 0; index < smart.archiveGroups.size(); ++index) {
		const auto& group = smart.archiveGroups[index];
		const filesystem::path list = paths.runtimeRoot
			/ (L"restore-" + to_wstring(chrono::steady_clock::now().time_since_epoch().count())
				+ L"-" + to_wstring(index) + L".txt");
		ScopedRuntimeArtifact cleanup(list);
		ofstream output(list, ios::binary | ios::trunc);
		if (!output.is_open()) return false;
		for (const auto& file : group.files) output << wstring_to_utf8(file) << '\n';
		output.close();
		if (runner.Execute({L"x", group.archive.wstring(), L"@" + list.wstring(),
			L"-o" + target.wstring(), L"-y"}, {}, lowPriority).status
			!= ProcessStatus::Succeeded) return false;
	}
	return true;
}

} // namespace

RestoreService::RestoreService(RestoreServiceDependencies dependencies)
	: dependencies_(std::move(dependencies)) {
	if (!dependencies_.archiveRunnerFactory) {
		dependencies_.archiveRunnerFactory = [](
			const filesystem::path& executable,
			const AppPaths& paths,
			stop_token stopToken) {
			return ArchiveRunner::Resolve(executable, paths, stopToken);
		};
	}
}

RestorePlan RestoreService::BuildAndVerify(
	const RestoreRequest& request,
	bool requireColdWorld,
	stop_token stopToken) const {
	RestorePlan plan;
	plan.mode = request.mode;
	plan.targetWorld = filesystem::path(request.config.saveRoot) / request.world.relativePath;
	if (request.config.pendingLocalBinding) {
		plan.diagnostics.push_back(Failure("restore.profile.binding_required"));
		return plan;
	}
	if (request.world.configId != request.config.configId) {
		plan.diagnostics.push_back(Failure("restore.config.identity_mismatch"));
		return plan;
	}
	wstring normalized;
	if (!JobStorage::TryNormalizeWorldPath(request.world.relativePath, normalized)
		|| normalized != request.world.relativePath) {
		plan.diagnostics.push_back(Failure("restore.world.invalid"));
		return plan;
	}
	const filesystem::path backupRoot =
		filesystem::path(request.config.backupPath) / normalized;
	plan.selectedArchive = request.archive.is_absolute()
		? request.archive.lexically_normal()
		: (backupRoot / request.archive).lexically_normal();
	if (!IsWithin(backupRoot, plan.selectedArchive)
		|| !filesystem::is_regular_file(plan.selectedArchive)) {
		plan.diagnostics.push_back(Failure("restore.backup.not_found",
			wstring_to_utf8(plan.selectedArchive.wstring())));
		return plan;
	}
	const wstring selectedName = plan.selectedArchive.filename().wstring();
	if (FolderRewindFormat::IsSmartBackupType(selectedName)) {
		string eventId;
		if (!BuildMetadataChain(GetMetadataDirectory(request.config, normalized),
				backupRoot, plan.selectedArchive, plan.archiveChain, eventId)) {
			plan.diagnostics.push_back(Failure(eventId, wstring_to_utf8(selectedName)));
			return plan;
		}
		plan.usesExactSmartPlan = request.mode == RestoreMode::Clean;
		if (plan.usesExactSmartPlan) {
			SmartFilePlan smart;
			if (!BuildSmartFilePlan(GetMetadataDirectory(request.config, normalized),
					plan.archiveChain, smart)) {
				plan.diagnostics.push_back(Failure("restore.smart.plan_invalid",
					wstring_to_utf8(selectedName)));
				return plan;
			}
		}
	}
	else if (FolderRewindFormat::IsFullLikeBackupType(selectedName)) {
		plan.archiveChain.push_back(plan.selectedArchive);
	}
	else {
		plan.diagnostics.push_back(Failure("restore.backup.type_invalid",
			wstring_to_utf8(selectedName)));
		return plan;
	}
	const auto runner = dependencies_.archiveRunnerFactory(
		request.config.zipPath, dependencies_.paths, stopToken);
	if (!runner.IsAvailable()) {
		plan.code = OperationCode::ToolUnavailable;
		plan.diagnostics.push_back(Failure("restore.tool.unavailable",
			wstring_to_utf8(runner.Resolution().diagnostic)));
		return plan;
	}
	for (const auto& archive : plan.archiveChain) {
		if (stopToken.stop_requested()) {
			plan.code = OperationCode::Cancelled;
			plan.diagnostics.push_back({"restore.cancelled", DiagnosticSeverity::Warning, {}});
			return plan;
		}
		const auto tested = runner.Execute(
			{L"t", archive.wstring(), L"-y"}, {}, request.config.useLowPriority);
		if (tested.status != ProcessStatus::Succeeded) {
			plan.code = tested.status == ProcessStatus::Cancelled
				? OperationCode::Cancelled : OperationCode::VerificationFailed;
			plan.diagnostics.push_back(Failure("restore.archive.corrupt",
				wstring_to_utf8(archive.wstring())));
			return plan;
		}
		++plan.checkedArchiveCount;
	}
	if (requireColdWorld && dependencies_.isWorldOccupied
		&& dependencies_.isWorldOccupied(plan.targetWorld)) {
		plan.diagnostics.push_back(Failure("restore.world.occupied",
			wstring_to_utf8(plan.targetWorld.wstring())));
		return plan;
	}
	plan.code = OperationCode::Success;
	return plan;
}

RestorePlan RestoreService::Verify(
	const RestoreRequest& request,
	stop_token stopToken) const {
	return BuildAndVerify(request, false, stopToken);
}

RestoreResult RestoreService::Run(
	const RestoreRequest& request,
	bool dryRun,
	stop_token stopToken) const {
	RestoreResult result;
	result.dryRun = dryRun;
	result.plan = BuildAndVerify(request, true, stopToken);
	result.code = result.plan.code;
	result.diagnostics = result.plan.diagnostics;
	if (!IsSuccessful(result.plan.code) || dryRun) return result;
	if (request.config.backupBefore) {
		if (!dependencies_.backupBeforeRestore) {
			result.code = OperationCode::RestoreFailed;
			result.diagnostics.push_back(Failure("restore.safety_backup.runtime_missing"));
			return result;
		}
		BackupRequest backup;
		backup.config = request.config;
		backup.world = request.world;
		backup.sourcePath = result.plan.targetWorld;
		backup.displayName = request.world.relativePath;
		backup.comment = L"Automatic backup before restore";
		result.safetyBackup = dependencies_.backupBeforeRestore(backup, stopToken);
		if (!IsSuccessful(result.safetyBackup->code)) {
			result.code = OperationCode::RestoreFailed;
			result.diagnostics.push_back(Failure("restore.safety_backup.failed"));
			return result;
		}
	}
	if (dependencies_.isWorldOccupied
		&& dependencies_.isWorldOccupied(result.plan.targetWorld)) {
		result.code = OperationCode::RestoreFailed;
		result.diagnostics.push_back(Failure("restore.world.occupied"));
		return result;
	}
	WorldOperationGuard guard(result.plan.targetWorld, FolderState::RESTORE);
	if (!guard.Acquired()) {
		result.code = OperationCode::RestoreFailed;
		result.diagnostics.push_back(Failure("restore.world.busy"));
		return result;
	}
	const auto runner = dependencies_.archiveRunnerFactory(
		request.config.zipPath, dependencies_.paths, stopToken);
	filesystem::path snapshot;
	string errorText;
	if (request.mode == RestoreMode::Clean) {
		if (!PrepareCleanWorkspace(result.plan.targetWorld, snapshot, errorText)) {
			result.code = OperationCode::RestoreFailed;
			result.diagnostics.push_back(Failure("restore.snapshot.prepare_failed", errorText));
			return result;
		}
	}
	else {
		error_code error;
		filesystem::create_directories(result.plan.targetWorld, error);
		if (error) {
			result.code = OperationCode::RestoreFailed;
			result.diagnostics.push_back(Failure("restore.target.create_failed", error.message()));
			return result;
		}
	}
	const bool extracted = ExtractChain(
		result.plan,
		GetMetadataDirectory(request.config, request.world.relativePath),
		result.plan.targetWorld,
		runner,
		dependencies_.paths,
		request.config.useLowPriority);
	bool committed = extracted;
	if (extracted && request.mode == RestoreMode::Clean) {
		committed = CommitCleanWorkspace(result.plan.targetWorld, snapshot,
			request.restorePreserve, errorText);
	}
	else if (extracted) {
		CleanupMarkers(result.plan.targetWorld);
	}
	if (!committed) {
		result.code = stopToken.stop_requested()
			? OperationCode::Cancelled : OperationCode::RestoreFailed;
		result.diagnostics.push_back(Failure("restore.extract.failed", errorText));
		if (request.mode == RestoreMode::Clean && !snapshot.empty()) {
			result.rollbackAttempted = true;
			result.rollbackSucceeded = RollbackCleanWorkspace(
				result.plan.targetWorld, snapshot, errorText);
			if (!result.rollbackSucceeded) {
				result.diagnostics.push_back(Failure("restore.rollback.failed", errorText));
			}
		}
		return result;
	}
	result.code = OperationCode::Success;
	return result;
}

const char* ToString(RestoreMode mode) noexcept {
	return mode == RestoreMode::Clean ? "clean" : "overwrite";
}
