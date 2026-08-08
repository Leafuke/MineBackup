#include "RuntimeCloudPostHook.h"

#include "AtomicFileWriter.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "json.hpp"
#include "text_to_text.h"

#include <chrono>
#include <filesystem>

using namespace std;

namespace {

class TemporaryCloudFiles {
public:
	explicit TemporaryCloudFiles(filesystem::path root)
		: root_(std::move(root)) {
	}
	~TemporaryCloudFiles() {
		error_code ignored;
		filesystem::remove_all(root_, ignored);
	}
	const filesystem::path& Root() const { return root_; }

private:
	filesystem::path root_;
};

CloudPostResult Failed(string eventId, string detail) {
	CloudPostResult result;
	result.status = CloudPostStatus::Failed;
	result.diagnostics.push_back({
		std::move(eventId), DiagnosticSeverity::Error, std::move(detail)});
	return result;
}

bool RunCopy(
	const RcloneClient& client,
	const filesystem::path& source,
	const wstring& destination,
	CloudPostResult& result,
	string eventId) {
	if (!filesystem::is_regular_file(source)) {
		result = Failed(std::move(eventId),
			"Required cloud input does not exist: " + wstring_to_utf8(source.wstring()));
		return false;
	}
	const auto execution = client.CopyTo(source.wstring(), destination);
	if (!execution.command.success) {
		result = Failed(std::move(eventId), wstring_to_utf8(
			execution.command.detail.empty()
				? execution.command.message
				: execution.command.detail));
		return false;
	}
	return true;
}

} // namespace

SynchronousRcloneCloudPostHook::SynchronousRcloneCloudPostHook(
	AppPaths paths,
	HistoryRepository& history,
	ConfigSnapshot configSnapshot,
	RcloneClient::ProcessExecutor processExecutor,
	ToolResolver toolResolver)
	: paths_(std::move(paths)),
	  history_(history),
	  configSnapshot_(std::move(configSnapshot)),
	  processExecutor_(std::move(processExecutor)),
	  toolResolver_(toolResolver ? std::move(toolResolver) : ExternalToolManager::ResolveRclone) {
}

CloudPostResult SynchronousRcloneCloudPostHook::Run(
	const BackupRequest& request,
	const HistoryEntry& historyEntry,
	stop_token stopToken) {
	const Config& config = request.config;
	if (!config.cloudSyncEnabled) return {};
	if (stopToken.stop_requested()) {
		return Failed("cloud.upload.cancelled", "Cancellation was requested before cloud upload.");
	}
	if (config.rcloneRemotePath.empty()) {
		return Failed("cloud.config.invalid", "The rclone remote path is empty.");
	}
	const auto resolution = toolResolver_(config.rclonePath, paths_, stopToken);
	if (!resolution.available) {
		return Failed("cloud.tool.unavailable", wstring_to_utf8(resolution.diagnostic));
	}

	RcloneClientOptions options;
	options.executable = resolution.executable;
	options.workingDirectory = config.cloudWorkingDirectory;
	options.timeout = chrono::seconds(max(1, config.cloudTimeoutSeconds));
	options.retryCount = max(0, config.cloudRetryCount);
	options.useLowPriority = config.useLowPriority;
	options.stopToken = stopToken;
	RcloneClient client(std::move(options), processExecutor_);

	FolderRewindFormat::StoragePaths storage;
	if (!FolderRewindFormat::TryResolveStoragePaths(
			config.backupPath,
			historyEntry.worldName,
			historyEntry.worldPath,
			storage)) {
		return Failed("cloud.storage.invalid", "Cannot resolve FolderRewind cloud paths.");
	}
	CloudPostResult result;
	if (config.cloudSyncMode == static_cast<int>(CloudSyncMode::HistoryAndBackups)) {
		const filesystem::path archive = storage.backupSubDir / historyEntry.backupFile;
		if (!RunCopy(
				client,
				archive,
				FolderRewindFormat::BuildArchiveRemotePath(
					config, storage.folderName, historyEntry.backupFile),
				result,
				"cloud.archive.upload_failed")) return result;
	}

	const filesystem::path metadataState = storage.statePath;
	const filesystem::path metadataRecord =
		storage.recordsDir / (historyEntry.backupFile + L".json");
	if (filesystem::is_regular_file(metadataState)
		&& !RunCopy(client, metadataState,
			FolderRewindFormat::BuildMetadataStateRemotePath(config, storage.folderName),
			result, "cloud.metadata.state_upload_failed")) return result;
	if (filesystem::is_regular_file(metadataRecord)
		&& !RunCopy(client, metadataRecord,
			FolderRewindFormat::BuildMetadataRecordRemotePath(
				config, storage.folderName, historyEntry.backupFile),
			result, "cloud.metadata.record_upload_failed")) return result;

	const map<int, Config> configs = configSnapshot_ ? configSnapshot_() : map<int, Config>{};
	const auto snapshot = history_.Snapshot();
	nlohmann::json historyJson = nlohmann::json::array();
	for (const auto& [index, candidate] : configs) {
		(void)index;
		const auto found = snapshot->byConfigId.find(candidate.configId);
		if (found == snapshot->byConfigId.end()) continue;
		for (const auto& entry : *found->second) {
			historyJson.push_back(
				FolderRewindHistoryStore::SerializeHistoryItem(candidate, entry));
		}
	}
	vector<HistoryEntry> configEntries;
	if (const auto found = snapshot->byConfigId.find(config.configId);
		found != snapshot->byConfigId.end()) {
		configEntries.assign(found->second->begin(), found->second->end());
	}

	const filesystem::path tempRoot = paths_.runtimeRoot /
		(L"MineBackup_CloudPost_" + FolderRewindFormat::GenerateGuidString());
	TemporaryCloudFiles cleanup(tempRoot);
	const filesystem::path historyFile = tempRoot / L"history.json";
	const filesystem::path manifestFile = tempRoot / L"active-history.json";
	const AtomicFileWriter::WriteOptions writeOptions{false, true};
	if (!AtomicFileWriter::WriteText(historyFile, historyJson.dump(2), writeOptions).success
		|| !AtomicFileWriter::WriteText(
			manifestFile,
			FolderRewindHistoryStore::SerializeActiveHistoryManifest(
				config, configEntries).dump(2),
			writeOptions).success) {
		return Failed("cloud.history.serialize_failed", "Cannot create the cloud history snapshot.");
	}
	if (!RunCopy(client, historyFile,
			FolderRewindFormat::BuildGlobalHistoryRemotePath(config),
			result, "cloud.history.upload_failed")) return result;
	if (!RunCopy(client, manifestFile,
			FolderRewindFormat::BuildActiveHistoryManifestRemotePath(config),
			result, "cloud.manifest.upload_failed")) return result;

	const wstring archiveRemote = config.cloudSyncMode
		== static_cast<int>(CloudSyncMode::HistoryAndBackups)
		? FolderRewindFormat::BuildArchiveRemotePath(
			config, storage.folderName, historyEntry.backupFile)
		: L"";
	const wstring stateRemote = FolderRewindFormat::BuildMetadataStateRemotePath(
		config, storage.folderName);
	const wstring recordRemote = FolderRewindFormat::BuildMetadataRecordRemotePath(
		config, storage.folderName, historyEntry.backupFile);
	const auto mutation = history_.Mutate(
		config.configId,
		paths_.HistoryFile(),
		configs,
		true,
		[&](vector<HistoryEntry>& entries) {
			for (auto& entry : entries) {
				if (entry.worldName != historyEntry.worldName
					|| entry.backupFile != historyEntry.backupFile) continue;
				entry.isCloudArchived = !archiveRemote.empty();
				entry.cloudArchivedAtUtc = FolderRewindFormat::MakeUtcTimestampString();
				entry.cloudArchiveRemotePath = archiveRemote;
				entry.cloudMetadataStateRemotePath = stateRemote;
				entry.cloudMetadataRecordRemotePath = recordRemote;
				return true;
			}
			return false;
		});
	if (!mutation.changed || !mutation.persisted) {
		return Failed(
			"cloud.history.commit_failed",
			"Cloud upload completed, but its local history state could not be committed.");
	}

	result.status = CloudPostStatus::Succeeded;
	result.diagnostics.push_back({
		"cloud.upload.completed", DiagnosticSeverity::Info, {}});
	return result;
}
