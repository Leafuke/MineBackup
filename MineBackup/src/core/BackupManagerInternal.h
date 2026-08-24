#pragma once

#include "DataModels.h"
#include "BackupChangeDetector.h"
#include "FolderRewindFormat.h"
#include "ProcessRunner.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace BackupManagerInternal {

class ScopedRuntimeArtifact {
public:
	explicit ScopedRuntimeArtifact(std::filesystem::path path);
	~ScopedRuntimeArtifact();
	ScopedRuntimeArtifact(const ScopedRuntimeArtifact&) = delete;
	ScopedRuntimeArtifact& operator=(const ScopedRuntimeArtifact&) = delete;

private:
	std::filesystem::path path_;
};

ProcessSpec MakeInternalProcess(
	const std::filesystem::path& executable,
	std::vector<std::wstring> arguments,
	const std::filesystem::path& workingDirectory = {},
	bool useLowPriority = false);
bool RunInternalProcess(const ProcessSpec& spec);
std::vector<std::wstring> SevenZipCreateArguments(
	const Config& config,
	int level,
	const std::filesystem::path& archive);

enum class FolderState {
	BACKUP,
	RESTORE,
};

const char* FolderStateToI18nKey(FolderState state);
bool IsAsciiOnlyPath(const std::wstring& value);
int NormalizeCompressionLevel(const std::wstring& method, int level);

class WorldOperationGuard {
public:
	WorldOperationGuard(const std::filesystem::path& worldPath, FolderState requested);
	WorldOperationGuard(const WorldOperationGuard&) = delete;
	WorldOperationGuard& operator=(const WorldOperationGuard&) = delete;
	WorldOperationGuard(WorldOperationGuard&& other) noexcept;
	WorldOperationGuard& operator=(WorldOperationGuard&& other) noexcept;
	~WorldOperationGuard();

	bool Acquired() const;
	FolderState Requested() const;
	FolderState Existing() const;

private:
	void Release();

	std::wstring key_;
	FolderState requested_ = FolderState::BACKUP;
	FolderState existing_ = FolderState::BACKUP;
	bool acquired_ = false;
};

std::vector<std::wstring> BuildEffectiveBackupBlacklist(
	const std::vector<std::wstring>& userBlacklist);
std::filesystem::path GetMetadataDirectory(
	const Config& config,
	const std::wstring& worldName);
bool UpdateMetadataFiles(
	const std::filesystem::path& metadataDirectory,
	const std::wstring& currentBackupFile,
	const std::wstring& baseBackupFile,
	const std::wstring& previousLastBackupFile,
	const std::wstring& backupType,
	std::map<std::wstring, FolderRewindFormat::FileState> currentState,
	const BackupChangeSet& changeSet);
void InvalidateBackupMetadata(
	const Config& config,
	const std::wstring& worldName,
	const std::wstring& deletedBackupFile,
	const std::wstring& renamedOldFile = L"",
	const std::wstring& renamedNewFile = L"");
void ClearReadonlyAttributesRecursively(const std::filesystem::path& directory);
bool CreateDeletionOnlyArchive(
	const Config& config,
	const std::filesystem::path& archivePath);
void LimitBackupFiles(
	const Config& config,
	const int& configIndex,
	const std::wstring& folderPath,
	int limit);

} // namespace BackupManagerInternal
