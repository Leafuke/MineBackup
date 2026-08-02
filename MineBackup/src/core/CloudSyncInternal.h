#pragma once

#include "CloudSyncService.h"
#include "FolderRewindFormat.h"
#include "ProcessRunner.h"
#include "json.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace CloudSyncInternal {
	struct HistoryCloudPaths {
		std::filesystem::path archiveLocalPath;
		std::filesystem::path metadataDir;
		std::filesystem::path metadataStateLocalPath;
		std::filesystem::path metadataRecordLocalPath;
		std::wstring archiveRemotePath;
		std::wstring metadataStateRemotePath;
		std::wstring metadataRecordRemotePath;
	};

	enum class ActiveManifestLoadStatus {
		Loaded,
		NotFound,
		Failed
	};

	extern std::mutex g_cloudMutex;

	class CloudOperationScope {
	public:
		CloudOperationScope(int configIndex, const std::wstring& initialStatus);
		~CloudOperationScope();

		CloudOperationScope(const CloudOperationScope&) = delete;
		CloudOperationScope& operator=(const CloudOperationScope&) = delete;

		void Finish(const CloudCommandResult& result, bool updateConfig = true);
		void Finish(const std::wstring& message, bool success, bool updateConfig = true);

	private:
		int configIndex_;
		std::wstring initialStatus_;
		bool finished_ = false;
	};

	void SetCloudRuntimeState(
		int configIndex,
		bool busy,
		int progress,
		const std::wstring& statusText,
		const std::wstring& lastMessage = L"");
	void UpdateConfigCloudLastResult(int configIndex, const CloudCommandResult& result);
	CloudCommandResult MakeConfigErrorResult(
		const char* key,
		const std::wstring& detail = L"");
	bool EnsureCloudConfigured(const Config& config, CloudCommandResult& outResult);

	FolderRewindFormat::StoragePaths ResolveStoragePaths(
		const Config& config,
		const std::wstring& folderName,
		const std::wstring& folderPath);
	HistoryCloudPaths BuildHistoryPaths(const Config& config, const HistoryEntry& entry);
	ProcessSpec BuildRcloneCopyToCommand(
		const Config& config,
		const std::wstring& sourcePath,
		const std::wstring& destinationPath);
	ProcessSpec BuildRcloneCopyCommand(
		const Config& config,
		const std::wstring& sourcePath,
		const std::wstring& destinationPath);
	CloudCommandResult ExecuteCommandWithRetry(
		const Config& config,
		int configIndex,
		const ProcessSpec& command,
		const char* busyStatusKey,
		int progress);
	bool IsRemoteObjectMissing(const CloudCommandResult& result);
	bool IsFullLikeBackupType(const std::wstring& typeOrFileName);

	std::filesystem::path BuildTempFilePath(
		const wchar_t* prefix,
		const wchar_t* extension);
	bool TryParseCloudHistoryArray(
		const nlohmann::json& root,
		std::vector<HistoryEntry>& outEntries);
	bool ManifestContainsHistoryItem(
		const CloudActiveHistoryManifest& manifest,
		const HistoryEntry& entry);
	bool BelongsToConfiguration(const Config& config, const HistoryEntry& entry);
	bool HasLocalBackupOrMetadataInternal(const Config& config, const HistoryEntry& entry);

	CloudCommandResult UploadConfigurationHistorySnapshotNoLock(
		const Config& config,
		int configIndex);
	CloudCommandResult UploadHistoryEntryNoLock(
		const Config& config,
		int configIndex,
		const HistoryEntry& entry);
	CloudCommandResult DownloadHistoryEntryNoLock(
		const Config& config,
		int configIndex,
		const HistoryEntry& entry);
	std::vector<HistoryEntry> LoadRemoteHistoryEntriesNoLock(
		const Config& config,
		int configIndex,
		CloudCommandResult& outResult);
	ActiveManifestLoadStatus TryLoadActiveManifestNoLock(
		const Config& config,
		int configIndex,
		CloudActiveHistoryManifest& outManifest,
		CloudCommandResult& outResult);
}
