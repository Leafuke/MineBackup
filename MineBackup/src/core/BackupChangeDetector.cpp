#include "BackupChangeDetector.h"

#include "FolderRewindMetadataStore.h"

#include <algorithm>
#include <system_error>

using namespace std;

namespace {

bool IsSafeNormalizedRelativePath(const wstring& value) {
	if (value.empty() || value == L"." || value == L"..") return false;
	const filesystem::path path(value);
	if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
	for (const auto& part : path) {
		const wstring segment = part.wstring();
		if (segment.empty() || segment == L"." || segment == L".."
			|| !FolderRewindFormat::IsSafeSinglePathSegment(segment)) {
			return false;
		}
	}
	return true;
}

bool TryGetNormalizedRelativePath(
	const filesystem::path& file,
	const filesystem::path& root,
	wstring& result) {
	error_code ec;
	const filesystem::path relative = filesystem::relative(file, root, ec);
	if (ec || relative.empty() || relative.is_absolute()
		|| relative.has_root_name() || relative.has_root_directory()) {
		return false;
	}
	try {
		result = FolderRewindFormat::NormalizeRelativePath(relative);
	}
	catch (...) {
		result.clear();
		return false;
	}
	return IsSafeNormalizedRelativePath(result);
}

bool TryCaptureFileState(const filesystem::path& file, FolderRewindFormat::FileState& result) {
	error_code ec;
	if (!filesystem::is_regular_file(file, ec) || ec) return false;

	FolderRewindFormat::FileState state;
	state.size = filesystem::file_size(file, ec);
	if (ec) return false;
	const auto modified = filesystem::last_write_time(file, ec);
	if (ec) return false;
	state.lastWriteTimeUtc = FolderRewindFormat::FormatFileTimeUtc(modified);
	if (state.lastWriteTimeUtc.empty()) return false;
	result = std::move(state);
	return true;
}

bool CollectCurrentState(
	const filesystem::path& root,
	map<wstring, FolderRewindFormat::FileState>& state) {
	map<wstring, FolderRewindFormat::FileState> nextState;
	error_code ec;
	if (!filesystem::exists(root, ec) || ec) return false;

	filesystem::recursive_directory_iterator iterator(root, ec), end;
	if (ec) return false;
	while (iterator != end) {
		const auto& entry = *iterator;
		const bool regularFile = entry.is_regular_file(ec);
		if (ec) return false;
		if (regularFile) {
			wstring relative;
			FolderRewindFormat::FileState fileState;
			if (!TryGetNormalizedRelativePath(entry.path(), root, relative)
				|| !TryCaptureFileState(entry.path(), fileState)) {
				return false;
			}
			nextState[relative] = std::move(fileState);
		}
		iterator.increment(ec);
		if (ec) return false;
	}
	state = std::move(nextState);
	return true;
}

} // namespace

BackupScanResult BackupChangeDetector::Scan(
	const filesystem::path& sourceRoot,
	const filesystem::path& metadataDirectory,
	const filesystem::path& backupDirectory) const {
	BackupScanResult result;
	auto collectOrFail = [&](BackupScanStatus successStatus) {
		if (CollectCurrentState(sourceRoot, result.currentState)) {
			result.status = successStatus;
		}
		else {
			result = BackupScanResult{};
			result.status = BackupScanStatus::ScanFailed;
		}
	};

	FolderRewindFormat::MetadataState metadata;
	if (!FolderRewindMetadataStore::LoadState(metadataDirectory, metadata)) {
		collectOrFail(BackupScanStatus::MetadataInvalid);
		return result;
	}

	for (const auto& [rawPath, state] : metadata.fileStates) {
		wstring normalized;
		try {
			normalized = FolderRewindFormat::NormalizeRelativePath(rawPath);
		}
		catch (...) {
			normalized.clear();
		}
		if (!IsSafeNormalizedRelativePath(normalized) || state.lastWriteTimeUtc.empty()) {
			collectOrFail(BackupScanStatus::MetadataInvalid);
			return result;
		}
	}
	result.previousLastBackupFileName = metadata.lastBackupFileName;
	result.previousBasedOnFullBackup = metadata.basedOnFullBackup;

	error_code ec;
	if (metadata.lastBackupFileName.empty()
		|| !filesystem::exists(backupDirectory / metadata.lastBackupFileName, ec) || ec) {
		collectOrFail(BackupScanStatus::BaseBackupMissing);
		return result;
	}
	ec.clear();
	if (!metadata.basedOnFullBackup.empty()
		&& (!filesystem::exists(backupDirectory / metadata.basedOnFullBackup, ec) || ec)) {
		collectOrFail(BackupScanStatus::BaseBackupMissing);
		return result;
	}

	if (!CollectCurrentState(sourceRoot, result.currentState)) {
		result.status = BackupScanStatus::ScanFailed;
		return result;
	}

	for (const auto& [path, state] : result.currentState) {
		const auto previous = metadata.fileStates.find(path);
		if (previous == metadata.fileStates.end()) {
			result.changes.addedFiles.push_back(path);
			result.changedFiles.push_back(sourceRoot / filesystem::path(path));
		}
		else if (previous->second.size != state.size
			|| previous->second.lastWriteTimeUtc != state.lastWriteTimeUtc) {
			result.changes.modifiedFiles.push_back(path);
			result.changedFiles.push_back(sourceRoot / filesystem::path(path));
		}
	}
	for (const auto& [path, ignored] : metadata.fileStates) {
		if (!result.currentState.contains(path)) result.changes.deletedFiles.push_back(path);
	}

	sort(result.changes.addedFiles.begin(), result.changes.addedFiles.end());
	sort(result.changes.modifiedFiles.begin(), result.changes.modifiedFiles.end());
	sort(result.changes.deletedFiles.begin(), result.changes.deletedFiles.end());
	result.status = result.changes.HasChanges()
		? BackupScanStatus::ChangesDetected
		: BackupScanStatus::NoChange;
	return result;
}
