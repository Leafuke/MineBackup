#include "RestoreService.h"

#include "BackupManagerInternal.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "JobDocument.h"
#include "RestoreWorkspace.h"
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
		if (!record.fullFileList.empty()) {
			if (owners.size() != record.fullFileList.size()) return false;
			auto owner = owners.begin();
			auto expected = record.fullFileList.begin();
			for (; owner != owners.end(); ++owner, ++expected) {
				if (owner->first != *expected) return false;
			}
		}
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
	FolderRewindFormat::StoragePaths storagePaths;
	if (!FolderRewindFormat::TryResolveStoragePaths(
			request.config.backupPath,
			normalized,
			plan.targetWorld.wstring(),
			storagePaths)) {
		plan.diagnostics.push_back(Failure("restore.storage.invalid", wstring_to_utf8(normalized)));
		return plan;
	}
	const filesystem::path backupRoot = storagePaths.backupSubDir;
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
	auto plan = BuildAndVerify(request, false, stopToken);
	if (plan.code == OperationCode::RestoreFailed) {
		plan.code = OperationCode::VerificationFailed;
	}
	return plan;
}

RestoreResult RestoreService::Run(
	const RestoreRequest& request,
	bool dryRun,
	stop_token stopToken) const {
	RestoreResult result;
	optional<BackupRequest> safetyBackupRequest;
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
		safetyBackupRequest = backup;
		result.safetyBackup = dependencies_.backupBeforeRestore(
			backup, stopToken, BackupExecutionOptions{.deferRetention = true});
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
	RestoreWorkspace::State workspace;
	string errorText;
	const auto workspaceMode = request.mode == RestoreMode::Clean
		? RestoreWorkspace::Mode::Clean : RestoreWorkspace::Mode::Overlay;
	if (!RestoreWorkspace::Prepare(
			result.plan.targetWorld, workspace, errorText, workspaceMode)) {
		result.code = OperationCode::RestoreFailed;
		result.diagnostics.push_back(Failure("restore.snapshot.prepare_failed", errorText));
		if (workspace.prepared) {
			result.rollbackAttempted = true;
			result.rollbackSucceeded = RestoreWorkspace::Rollback(workspace, errorText);
			if (!result.rollbackSucceeded) {
				result.diagnostics.push_back(Failure("restore.rollback.failed", errorText));
			}
		}
		return result;
	}
	const bool extracted = ExtractChain(
		result.plan,
		GetMetadataDirectory(request.config, request.world.relativePath),
		result.plan.targetWorld,
		runner,
		dependencies_.paths,
		request.config.useLowPriority);
	bool committed = extracted;
	if (extracted) {
		CleanupMarkers(result.plan.targetWorld);
		const auto preserve = request.mode == RestoreMode::Clean
			? request.restorePreserve : vector<wstring>{};
		committed = RestoreWorkspace::Commit(workspace, preserve, errorText);
	}
	if (!committed) {
		result.code = stopToken.stop_requested()
			? OperationCode::Cancelled : OperationCode::RestoreFailed;
		result.diagnostics.push_back(Failure("restore.extract.failed", errorText));
		if (workspace.prepared) {
			result.rollbackAttempted = true;
			result.rollbackSucceeded = RestoreWorkspace::Rollback(workspace, errorText);
			if (!result.rollbackSucceeded) {
				result.diagnostics.push_back(Failure("restore.rollback.failed", errorText));
			}
		}
		return result;
	}
	if (safetyBackupRequest && result.safetyBackup->historyEntry
		&& dependencies_.enforceRetention && !stopToken.stop_requested()) {
		try {
			// 先完成已锁定的 restore plan，再收敛安全备份的保留数量，避免 --latest 被新备份改写。
			dependencies_.enforceRetention(
				*safetyBackupRequest,
				*result.safetyBackup->historyEntry,
				stopToken);
		}
		catch (const exception& exception) {
			result.diagnostics.push_back({
				"restore.safety_backup.retention_failed",
				DiagnosticSeverity::Warning,
				SanitizeUtf8(exception.what(), 256u * 1024u).value});
		}
		catch (...) {
			result.diagnostics.push_back({
				"restore.safety_backup.retention_failed",
				DiagnosticSeverity::Warning,
				"unknown exception"});
		}
	}
	result.code = OperationCode::Success;
	return result;
}

const char* ToString(RestoreMode mode) noexcept {
	return mode == RestoreMode::Clean ? "clean" : "overwrite";
}
