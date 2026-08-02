#pragma once

#include "FolderRewindFormat.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

enum class BackupScanStatus {
	NoChange,
	ChangesDetected,
	MetadataInvalid,
	BaseBackupMissing,
	ScanFailed
};

struct BackupChangeSet {
	std::vector<std::wstring> addedFiles;
	std::vector<std::wstring> modifiedFiles;
	std::vector<std::wstring> deletedFiles;

	bool HasChanges() const {
		return !addedFiles.empty() || !modifiedFiles.empty() || !deletedFiles.empty();
	}

	bool HasContentChanges() const {
		return !addedFiles.empty() || !modifiedFiles.empty();
	}
};

struct BackupScanResult {
	BackupScanStatus status = BackupScanStatus::NoChange;
	std::map<std::wstring, FolderRewindFormat::FileState> currentState;
	BackupChangeSet changes;
	std::vector<std::filesystem::path> changedFiles;
};

class BackupChangeDetector {
public:
	BackupScanResult Scan(
		const std::filesystem::path& sourceRoot,
		const std::filesystem::path& metadataDirectory,
		const std::filesystem::path& backupDirectory) const;
};
