#include "ChainSafeRetention.h"

#include "BackupManagerInternal.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "WorldIdentity.h"
#include "text_to_text.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <system_error>

using namespace std;
using namespace BackupManagerInternal;

namespace ChainSafeRetention {
namespace {

bool IsCancelled(const Request& request) {
	return request.stopToken.stop_requested();
}

struct MetadataBackup {
	filesystem::path source;
	filesystem::path copy;
	bool existed = false;
};

vector<wstring> Sorted(const set<wstring>& values) {
	return {values.begin(), values.end()};
}

enum class DeltaKind {
	Added,
	Modified,
	Deleted,
};

optional<DeltaKind> ComposeDelta(DeltaKind current, DeltaKind next) {
	switch (current) {
	case DeltaKind::Added:
		if (next == DeltaKind::Deleted) return nullopt;
		return DeltaKind::Added;
	case DeltaKind::Modified:
		return next == DeltaKind::Deleted ? DeltaKind::Deleted : DeltaKind::Modified;
	case DeltaKind::Deleted:
		return next == DeltaKind::Deleted ? DeltaKind::Deleted : DeltaKind::Modified;
	}
	return next;
}

void ApplyDelta(map<wstring, DeltaKind>& deltas, const wstring& path, DeltaKind next) {
	const auto current = deltas.find(path);
	if (current == deltas.end()) {
		deltas[path] = next;
		return;
	}
	const auto composed = ComposeDelta(current->second, next);
	if (composed) current->second = *composed;
	else deltas.erase(current);
}

void ApplyRecordDelta(
	map<wstring, DeltaKind>& deltas,
	const FolderRewindFormat::ChangeRecord& record) {
	for (const auto& path : record.deletedFiles) ApplyDelta(deltas, path, DeltaKind::Deleted);
	for (const auto& path : record.addedFiles) ApplyDelta(deltas, path, DeltaKind::Added);
	for (const auto& path : record.modifiedFiles) ApplyDelta(deltas, path, DeltaKind::Modified);
}

void BuildDeltaLists(
	const map<wstring, DeltaKind>& deltas,
	FolderRewindFormat::ChangeRecord& record) {
	record.addedFiles.clear();
	record.modifiedFiles.clear();
	record.deletedFiles.clear();
	for (const auto& [path, kind] : deltas) {
		switch (kind) {
		case DeltaKind::Added: record.addedFiles.push_back(path); break;
		case DeltaKind::Modified: record.modifiedFiles.push_back(path); break;
		case DeltaKind::Deleted: record.deletedFiles.push_back(path); break;
		}
	}
}

bool SameArchiveReference(const wstring& left, const wstring& right) {
	return !left.empty() && !right.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
}

void ReleaseFullFileList(FolderRewindFormat::ChangeRecord& record) {
	vector<wstring>().swap(record.fullFileList);
}

string SaveFailureDetail(
	const char* operation,
	const FolderRewindMetadataStore::SaveResult& result) {
	string detail(operation);
	if (!result.error.empty()) {
		detail += ": ";
		detail += wstring_to_utf8(result.error);
	}
	return detail;
}

bool BackupMetadata(const filesystem::path& source, const filesystem::path& copy, MetadataBackup& backup) {
	error_code error;
	backup.source = source;
	backup.copy = copy;
	backup.existed = filesystem::exists(source, error);
	if (error) return false;
	if (!backup.existed) return true;
	filesystem::copy(source, copy,
		filesystem::copy_options::recursive | filesystem::copy_options::overwrite_existing,
		error);
	return !error;
}

bool RestoreMetadata(const MetadataBackup& backup) {
	error_code error;
	filesystem::remove_all(backup.source, error);
	if (error) return false;
	if (!backup.existed) return true;
	filesystem::copy(backup.copy, backup.source,
		filesystem::copy_options::recursive | filesystem::copy_options::overwrite_existing,
		error);
	return !error;
}

bool RepairMetadata(
	const filesystem::path& metadataDirectory,
	const wstring& deletedFile,
	const wstring& mergedOldFile,
	const wstring& mergedFinalFile,
	const wstring& mergedBackupType,
	const FolderRewindFormat::ChangeRecord& deletedRecord,
	const FolderRewindFormat::ChangeRecord& mergedRecord,
	string& errorText) {
	FolderRewindFormat::ChangeRecord repaired = mergedRecord;
	repaired.archiveFileName = mergedFinalFile;
	repaired.backupType = mergedBackupType;
	repaired.createdAtUtc = mergedRecord.createdAtUtc.empty()
		? FolderRewindFormat::MakeUtcTimestampString() : mergedRecord.createdAtUtc;
	if (FolderRewindFormat::IsSmartBackupType(mergedBackupType)) {
		map<wstring, DeltaKind> deltas;
		ApplyRecordDelta(deltas, deletedRecord);
		ApplyRecordDelta(deltas, mergedRecord);
		repaired.previousBackupFileName = deletedRecord.previousBackupFileName;
		repaired.basedOnFullBackup = mergedRecord.basedOnFullBackup.empty()
			? deletedRecord.basedOnFullBackup : mergedRecord.basedOnFullBackup;
		BuildDeltaLists(deltas, repaired);
		repaired.fullFileList.clear();
	}
	else {
		if (deletedRecord.fullFileList.empty()) {
			errorText = "full checkpoint metadata has no complete file list";
			return false;
		}
		set<wstring> fullAfterMerged(
			deletedRecord.fullFileList.begin(), deletedRecord.fullFileList.end());
		for (const auto& path : mergedRecord.deletedFiles) fullAfterMerged.erase(path);
		for (const auto& path : mergedRecord.addedFiles) fullAfterMerged.insert(path);
		for (const auto& path : mergedRecord.modifiedFiles) fullAfterMerged.insert(path);
		repaired.previousBackupFileName.clear();
		repaired.basedOnFullBackup = mergedFinalFile;
		repaired.addedFiles = Sorted(fullAfterMerged);
		repaired.deletedFiles.clear();
		repaired.modifiedFiles.clear();
		repaired.fullFileList = repaired.addedFiles;
	}
	const auto repairedSave = FolderRewindMetadataStore::SaveRecordDetailed(
		metadataDirectory, repaired);
	if (!repairedSave.success) {
		errorText = SaveFailureDetail(
			"failed to save repaired chain metadata", repairedSave);
		return false;
	}
	if (!FolderRewindMetadataStore::DeleteRecord(metadataDirectory, deletedFile)) {
		errorText = "failed to remove deleted chain metadata";
		return false;
	}
	if (mergedOldFile != mergedFinalFile
		&& !FolderRewindMetadataStore::DeleteRecord(metadataDirectory, mergedOldFile)) {
		errorText = "failed to remove replaced chain metadata";
		return false;
	}
	vector<wstring> recordNames;
	if (!FolderRewindMetadataStore::ListRecordArchiveFileNames(
			metadataDirectory, recordNames)) {
		errorText = "failed to enumerate chain metadata after repair";
		return false;
	}
	for (const auto& recordName : recordNames) {
		FolderRewindFormat::ChangeRecord record;
		if (!FolderRewindMetadataStore::LoadRecord(metadataDirectory, recordName, record)) {
			errorText = "failed to load chain metadata record during reference repair";
			return false;
		}
		bool changed = false;
		if (SameArchiveReference(record.previousBackupFileName, deletedFile)
			|| (mergedOldFile != mergedFinalFile
				&& SameArchiveReference(record.previousBackupFileName, mergedOldFile))) {
			record.previousBackupFileName = mergedFinalFile;
			changed = true;
		}
		if (SameArchiveReference(record.basedOnFullBackup, deletedFile)
			|| (mergedOldFile != mergedFinalFile
				&& SameArchiveReference(record.basedOnFullBackup, mergedOldFile))) {
			record.basedOnFullBackup = mergedFinalFile;
			changed = true;
		}
		if (FolderRewindFormat::IsSmartBackupType(record.backupType)
			&& !record.fullFileList.empty()) {
			ReleaseFullFileList(record);
			changed = true;
		}
		if (!changed) continue;
		const auto referenceSave = FolderRewindMetadataStore::SaveRecordDetailed(
			metadataDirectory, record);
		if (!referenceSave.success) {
			errorText = SaveFailureDetail(
				"failed to repair references in chain metadata", referenceSave);
			return false;
		}
	}
	FolderRewindFormat::MetadataState state;
	if (FolderRewindMetadataStore::LoadState(metadataDirectory, state)) {
		bool changed = false;
		if (SameArchiveReference(state.lastBackupFileName, deletedFile)
			|| SameArchiveReference(state.lastBackupFileName, mergedOldFile)) {
			state.lastBackupFileName = mergedFinalFile;
			changed = true;
		}
		if (SameArchiveReference(state.basedOnFullBackup, deletedFile)
			|| (mergedOldFile != mergedFinalFile
				&& SameArchiveReference(state.basedOnFullBackup, mergedOldFile))) {
			state.basedOnFullBackup = mergedFinalFile;
			changed = true;
		}
		if (!changed) return true;
		const auto stateSave = FolderRewindMetadataStore::SaveStateDetailed(
			metadataDirectory, state);
		if (!stateSave.success) {
			errorText = SaveFailureDetail(
				"failed to repair chain metadata state", stateSave);
			return false;
		}
	}
	return true;
}

bool IsSame(const Config& config, const HistoryEntry& left, const HistoryEntry& right) {
	return WorldIdentity::SameHistoryEntry(config, left, right);
}

vector<HistoryEntry> WorldHistory(const Config& config, const vector<HistoryEntry>& history,
	const HistoryEntry& target) {
	vector<HistoryEntry> result;
	for (const auto& entry : history) {
		if (entry.configId == config.configId
			&& entry.backupFile.size()
			&& WorldIdentity::Matches(config, target.worldName, entry)) {
			result.push_back(entry);
		}
	}
	sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
		return left.timestamp_str < right.timestamp_str;
	});
	return result;
}

vector<HistoryEntry> RemoveHistoryEntry(
	const Config& config,
	const vector<HistoryEntry>& history,
	const HistoryEntry& target) {
	vector<HistoryEntry> updated;
	updated.reserve(history.size());
	for (const auto& entry : history) {
		if (!IsSame(config, entry, target)) updated.push_back(entry);
	}
	return updated;
}

Result DirectRemove(Request& request) {
	Result result;
	if (!request.commitHistory) {
		result.warning = true;
		result.detail = "history commit callback is unavailable";
		return result;
	}
	const filesystem::path archive = request.backupDirectory / request.entry.backupFile;
	error_code error;
	if (!filesystem::is_regular_file(archive, error) || error) {
		result.warning = true;
		result.detail = "archive is missing";
		return result;
	}
	const filesystem::path tempRoot = request.paths.runtimeRoot
		/ (L"MineBackup_Retention_" + FolderRewindFormat::GenerateGuidString());
	const filesystem::path archiveBackup = tempRoot / archive.filename();
	const MetadataBackup metadataBackup{
		request.metadataDirectory,
		tempRoot / L"metadata",
		false};
	MetadataBackup savedMetadata;
	try {
		if (IsCancelled(request)) {
			result.warning = true;
			result.detail = "retention was cancelled before archive removal";
			return result;
		}
		filesystem::create_directories(tempRoot);
		filesystem::copy_file(archive, archiveBackup,
			filesystem::copy_options::overwrite_existing);
		if (!BackupMetadata(metadataBackup.source, metadataBackup.copy, savedMetadata)) {
			throw runtime_error("failed to snapshot retention metadata");
		}
		error.clear();
		if (!filesystem::remove(archive, error) || error) {
			throw runtime_error("failed to remove retained archive");
		}
		if (!FolderRewindMetadataStore::DeleteRecord(
				request.metadataDirectory, request.entry.backupFile)) {
			throw runtime_error("failed to remove retention metadata");
		}
		const auto updated = RemoveHistoryEntry(request.config, request.history, request.entry);
		if (!request.commitHistory(updated)) {
			throw runtime_error("failed to persist retention history");
		}
		// history 已成功持久化，此处是事务提交点；提交后的清理不得重新进入回滚路径。
	}
	catch (const exception& exception) {
		error_code restoreError;
		filesystem::copy_file(archiveBackup, archive,
			filesystem::copy_options::overwrite_existing, restoreError);
		if (savedMetadata.existed && !RestoreMetadata(savedMetadata)) {
			result.detail = "retention rollback also failed after: " + string(exception.what());
		}
		else {
			result.detail = exception.what();
		}
		filesystem::remove_all(tempRoot, restoreError);
		result.warning = true;
		return result;
	}

	result.changed = true;
	error_code cleanupError;
	filesystem::remove_all(tempRoot, cleanupError);
	return result;
}

} // namespace

Result Remove(Request request) {
	Result result;
	if (IsCancelled(request)) {
		result.warning = true;
		result.detail = "retention was cancelled before archive mutation";
		return result;
	}
	if (request.entry.isImportant) {
		result.warning = true;
		result.detail = "important archive is retained";
		return result;
	}
	const auto chain = WorldHistory(request.config, request.history, request.entry);
	const auto target = find_if(chain.begin(), chain.end(), [&](const auto& entry) {
		return IsSame(request.config, entry, request.entry);
	});
	if (target == chain.end()) {
		result.warning = true;
		result.detail = "archive is not present in the configured history chain";
		return result;
	}
	const auto targetIndex = static_cast<size_t>(distance(chain.begin(), target));
	const HistoryEntry* next = targetIndex + 1 < chain.size() ? &chain[targetIndex + 1] : nullptr;
	if (!next
		|| FolderRewindFormat::IsFullLikeBackupType(next->backupType)
		|| FolderRewindFormat::IsFullLikeBackupType(next->backupFile)) {
		return DirectRemove(request);
	}
	if (!FolderRewindFormat::IsSmartBackupType(next->backupType)
		&& !FolderRewindFormat::IsSmartBackupType(next->backupFile)) {
		result.warning = true;
		result.detail = "the next chain archive has no recognized type";
		return result;
	}
	if (!FolderRewindFormat::IsSmartBackupType(request.entry.backupType)
		&& !FolderRewindFormat::IsSmartBackupType(request.entry.backupFile)
		&& !FolderRewindFormat::IsFullLikeBackupType(request.entry.backupType)
		&& !FolderRewindFormat::IsFullLikeBackupType(request.entry.backupFile)) {
		result.warning = true;
		result.detail = "the current archive has no recognized chain type";
		return result;
	}
	if (next->isImportant) {
		result.warning = true;
		result.detail = "the next chain archive is important";
		return result;
	}
	if (!request.archiveRunner || !request.archiveRunner->IsAvailable()) {
		result.warning = true;
		result.detail = "archive tool is unavailable for chain merge";
		return result;
	}
	const filesystem::path archiveToDelete = request.backupDirectory / request.entry.backupFile;
	const filesystem::path mergeTarget = request.backupDirectory / next->backupFile;
	error_code inspectError;
	if (!filesystem::is_regular_file(archiveToDelete, inspectError)
		|| !filesystem::is_regular_file(mergeTarget, inspectError) || inspectError) {
		result.warning = true;
		result.detail = "chain archive is missing";
		return result;
	}
	const bool deletingFullCheckpoint =
		FolderRewindFormat::IsFullLikeBackupType(request.entry.backupType)
		|| FolderRewindFormat::IsFullLikeBackupType(request.entry.backupFile);
	FolderRewindFormat::ChangeRecord mergedRecord;
	FolderRewindFormat::ChangeRecord deletedRecord;
	// Load the Smart record first and release any legacy complete snapshot before
	// loading the checkpoint. This bounds compatibility handling to one large
	// FullFileList at a time.
	if (!FolderRewindMetadataStore::LoadRecord(
			request.metadataDirectory, next->backupFile, mergedRecord)) {
		result.warning = true;
		result.detail = "required next-chain metadata is missing";
		return result;
	}
	ReleaseFullFileList(mergedRecord);
	if (!FolderRewindMetadataStore::LoadRecord(
			request.metadataDirectory, request.entry.backupFile, deletedRecord)) {
		result.warning = true;
		result.detail = "required current-chain metadata is missing";
		return result;
	}
	if (!deletingFullCheckpoint) ReleaseFullFileList(deletedRecord);
	const filesystem::path tempRoot = request.paths.runtimeRoot
		/ (L"MineBackup_Merge_" + FolderRewindFormat::GenerateGuidString());
	const filesystem::path workspace = tempRoot / L"merge_workspace";
	const filesystem::path rebuilt = tempRoot / (L"rebuilt." + request.config.zipFormat);
	const filesystem::path originalTarget = tempRoot / (L"target_backup." + request.config.zipFormat);
	const filesystem::path deletedBackup = tempRoot / (L"deleted_backup." + request.config.zipFormat);
	MetadataBackup metadataBackup;
	filesystem::file_time_type originalTime{};
	filesystem::path finalArchive = mergeTarget;
	wstring finalName = next->backupFile;
	wstring finalType = next->backupType;
	bool targetReplaced = false;
	bool deletedArchiveRemoved = false;
	try {
		if (IsCancelled(request)) {
			result.warning = true;
			result.detail = "retention was cancelled before chain merge";
			return result;
		}
		filesystem::create_directories(workspace);
		filesystem::copy_file(mergeTarget, originalTarget,
			filesystem::copy_options::overwrite_existing);
		filesystem::copy_file(archiveToDelete, deletedBackup,
			filesystem::copy_options::overwrite_existing);
		originalTime = filesystem::last_write_time(mergeTarget);
		if (!BackupMetadata(request.metadataDirectory,
						tempRoot / L"metadata", metadataBackup)) {
			throw runtime_error("failed to snapshot retention metadata");
		}
		if (request.archiveRunner->Execute(
			{L"x", archiveToDelete.wstring(), L"-o" + workspace.wstring(), L"-y"},
			{}, request.config.useLowPriority).status != ProcessStatus::Succeeded) {
			throw runtime_error("failed to extract deleted smart archive");
		}
		if (IsCancelled(request)) throw runtime_error("retention was cancelled after first extraction");
		if (request.archiveRunner->Execute(
			{L"x", mergeTarget.wstring(), L"-o" + workspace.wstring(), L"-y"},
			{}, request.config.useLowPriority).status != ProcessStatus::Succeeded) {
			throw runtime_error("failed to extract next smart archive");
		}
		error_code error;
		filesystem::remove_all(
			workspace / FolderRewindFormat::kInternalRestoreMarkerDirectoryName, error);
		for (const auto& deletedPath : mergedRecord.deletedFiles) {
			error.clear();
			filesystem::remove(workspace / filesystem::path(deletedPath), error);
			if (error) throw runtime_error("failed to remove deleted content from merged smart archive");
		}
		auto createArguments = ArchiveRunner::BuildCreateArguments(
			request.config,
			NormalizeCompressionLevel(request.config.zipMethod, request.config.zipLevel),
			rebuilt);
		createArguments.push_back(L"*");
		if (request.archiveRunner->Execute(std::move(createArguments), workspace,
				request.config.useLowPriority).status != ProcessStatus::Succeeded) {
			throw runtime_error("failed to rebuild merged smart archive");
		}
		if (IsCancelled(request)) throw runtime_error("retention was cancelled after archive rebuild");
		filesystem::remove(mergeTarget, error);
		if (error) throw runtime_error("failed to replace next smart archive");
		filesystem::rename(rebuilt, mergeTarget, error);
		if (error) {
			error.clear();
			filesystem::copy_file(rebuilt, mergeTarget,
				filesystem::copy_options::overwrite_existing, error);
			if (error) throw runtime_error("failed to deploy merged smart archive");
			filesystem::remove(rebuilt, error);
		}
		targetReplaced = true;
		if (deletingFullCheckpoint) {
			finalType = L"Full";
			finalName = next->backupFile;
			const size_t smartMarker = finalName.find(L"[Smart]");
			if (smartMarker != wstring::npos) finalName.replace(smartMarker, 7, L"[Full]");
			const filesystem::path renamed = request.backupDirectory / finalName;
			if (renamed != mergeTarget) {
				if (filesystem::exists(renamed)) {
					throw runtime_error("merged archive destination already exists");
				}
				filesystem::rename(mergeTarget, renamed);
				finalArchive = renamed;
			}
		}
		error.clear();
		filesystem::last_write_time(finalArchive, originalTime, error);
		if (!RepairMetadata(request.metadataDirectory, request.entry.backupFile,
				next->backupFile, finalName, finalType,
				deletedRecord, mergedRecord, result.detail)) {
			throw runtime_error(result.detail);
		}
		error.clear();
		if (!filesystem::remove(archiveToDelete, error) || error) {
			throw runtime_error("failed to remove deleted chain archive");
		}
		deletedArchiveRemoved = true;
		auto updated = RemoveHistoryEntry(request.config, request.history, request.entry);
		for (auto& entry : updated) {
			if (IsSame(request.config, entry, *next)) {
				entry.backupFile = finalName;
				entry.backupType = finalType;
				break;
			}
		}
		if (!request.commitHistory(std::move(updated))) {
			throw runtime_error("failed to persist history after chain merge");
		}
		filesystem::remove_all(tempRoot, error);
		result.changed = true;
		return result;
	}
	catch (const exception& exception) {
		error_code restoreError;
		if (targetReplaced) {
			filesystem::remove(finalArchive, restoreError);
			if (finalArchive != mergeTarget) filesystem::remove(mergeTarget, restoreError);
			restoreError.clear();
			filesystem::copy_file(originalTarget, mergeTarget,
				filesystem::copy_options::overwrite_existing, restoreError);
			if (!restoreError) filesystem::last_write_time(mergeTarget, originalTime, restoreError);
		}
		if (deletedArchiveRemoved) {
			restoreError.clear();
			filesystem::copy_file(deletedBackup, archiveToDelete,
				filesystem::copy_options::overwrite_existing, restoreError);
		}
		if (!RestoreMetadata(metadataBackup)) {
			result.detail = "retention rollback also failed after: "
				+ string(exception.what());
		}
		else if (result.detail.empty()) {
			result.detail = exception.what();
		}
		filesystem::remove_all(tempRoot, restoreError);
		result.warning = true;
		return result;
	}
}

} // namespace ChainSafeRetention
