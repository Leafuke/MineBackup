#include "CloudSyncService.h"

#include "ConfigManager.h"
#include "AppPaths.h"
#include "HistoryManager.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "FolderRewindMetadataStore.h"
#include "MigrationCoordinator.h"
#include "ProcessRunner.h"
#include "TaskCoordinator.h"
#include "ExternalToolManager.h"
#include "AtomicFileWriter.h"
#if MINEBACKUP_ENABLE_V15_MIGRATION
#include "V15MigrationAdapter.h"
#endif
#include "i18n.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace std;

namespace {
	const wchar_t* kCloudHistoryFileName = FolderRewindFormat::kCloudHistoryFileName;
	const wchar_t* kPortableCloudConfigFileName = L"portable-config.json";
	const wchar_t* kCloudStateDirName = FolderRewindFormat::kCloudStateDirName;
	const wchar_t* kCloudActiveHistoryFileName = FolderRewindFormat::kCloudActiveHistoryFileName;
	const wchar_t* kCloudMetadataDirName = FolderRewindFormat::kMetadataRootDirName;
	const wchar_t* kCloudMetadataRecordsDirName = FolderRewindFormat::kMetadataRecordsDirName;

	mutex g_cloudMutex;

	struct HistoryCloudPaths {
		filesystem::path archiveLocalPath;
		filesystem::path metadataDir;
		filesystem::path metadataStateLocalPath;
		filesystem::path metadataRecordLocalPath;
		wstring archiveRemotePath;
		wstring metadataStateRemotePath;
		wstring metadataRecordRemotePath;
	};

	vector<HistoryEntry> LoadRemoteHistoryEntriesNoLock(const Config& config, int configIndex, Console& console, CloudCommandResult& outResult);
	bool BelongsToConfiguration(const Config& config, const HistoryEntry& entry);
	bool TryResolveKnownConfigId(const HistoryEntry& entry, wstring& outConfigId);

	bool IsLegacyHistoryJson(const nlohmann::json& root) {
		if (!root.is_array()) return false;
		return any_of(root.begin(), root.end(), [](const nlohmann::json& item) {
			return item.is_object() && (item.contains("backupFile") || item.contains("configIndex"));
		});
	}

	wstring GetUtcTimestampString() {
		auto now = chrono::system_clock::now();
		time_t nowTime = chrono::system_clock::to_time_t(now);
		tm utcTime{};
#ifdef _WIN32
		gmtime_s(&utcTime, &nowTime);
#else
		gmtime_r(&nowTime, &utcTime);
#endif
		wchar_t buf[32];
		wcsftime(buf, size(buf), L"%Y-%m-%dT%H:%M:%SZ", &utcTime);
		return buf;
	}

	wstring NormalizeRemotePath(const wstring& value) {
		wstring result = value;
		for (wchar_t& ch : result) {
			if (ch == L'\\') ch = L'/';
		}
		while (!result.empty() && result.back() == L'/') {
			result.pop_back();
		}
		return result;
	}

	wstring AppendRemotePath(const wstring& root, initializer_list<wstring> segments) {
		wstring result = NormalizeRemotePath(root);
		for (const auto& rawSegment : segments) {
			wstring segment = NormalizeRemotePath(rawSegment);
			while (!segment.empty() && segment.front() == L'/') {
				segment.erase(segment.begin());
			}
			if (segment.empty()) continue;
			if (!result.empty() && result.back() != L'/') {
				result += L"/";
			}
			result += segment;
		}
		return result;
	}

	wstring GetConfigCloudSegment(const Config& config, int configIndex = -1) {
		wstring name = utf8_to_wstring(config.name);
		if (name.empty() && configIndex >= 0) {
			name = L"Config" + to_wstring(configIndex);
		}
		return name.empty() ? L"DefaultConfig" : name;
	}

	ExternalToolResolution ResolveRcloneExecutable(const Config& config) {
		return ExternalToolManager::ResolveRclone(
			config.rclonePath, GetAppPaths(), TaskCoordinator::CurrentStopToken());
	}

	bool IsIncrementalBackupType(const wstring& typeOrFileName) {
		return FolderRewindFormat::IsSmartBackupType(typeOrFileName);
	}

	bool IsFullLikeBackupType(const wstring& typeOrFileName) {
		return FolderRewindFormat::IsFullLikeBackupType(typeOrFileName);
	}

	void SetCloudRuntimeState(int configIndex, bool busy, int progress, const wstring& statusText, const wstring& lastMessage = L"") {
		lock_guard<mutex> lock(g_appState.cloudTask.mutex);
		g_appState.cloudTask.busy = busy;
		g_appState.cloudTask.progress = progress;
		g_appState.cloudTask.activeConfigIndex = configIndex;
		if (!statusText.empty()) {
			g_appState.cloudTask.statusText = statusText;
		}
		if (!lastMessage.empty()) {
			g_appState.cloudTask.lastMessage = lastMessage;
		}
	}

	void UpdateConfigCloudLastResult(int configIndex, const CloudCommandResult& result) {
		lock_guard<mutex> lock(g_appState.configsMutex);
		auto it = g_appState.configs.find(configIndex);
		if (it == g_appState.configs.end()) return;

		it->second.cloudLastRunUtc = GetUtcTimestampString();
		it->second.cloudLastExitCode = result.exitCode;
		it->second.cloudLastErrorMessage = result.success ? L"" : result.message;
	}

	CloudCommandResult MakeConfigErrorResult(const char* key, const wstring& detail = L"") {
		CloudCommandResult result;
		result.success = false;
		result.exitCode = -1;
		result.message = utf8_to_wstring(L(key));
		result.detail = detail;
		return result;
	}

	bool EnsureCloudConfigured(const Config& config, CloudCommandResult& outResult) {
		if (config.pendingLocalBinding) {
			outResult = MakeConfigErrorResult("CLOUD_CONFIG_INVALID", L"This configuration is waiting for local path binding.");
			return false;
		}
		if (config.rcloneRemotePath.empty()) {
			outResult = MakeConfigErrorResult("CLOUD_CONFIG_INVALID");
			return false;
		}
		const auto rclone = ResolveRcloneExecutable(config);
		if (!rclone.available) {
			outResult = MakeConfigErrorResult("CLOUD_RCLONE_NOT_FOUND", rclone.diagnostic);
			return false;
		}
		if (!config.cloudWorkingDirectory.empty() && !filesystem::exists(config.cloudWorkingDirectory)) {
			outResult = MakeConfigErrorResult("CLOUD_WORKDIR_MISSING", config.cloudWorkingDirectory);
			return false;
		}
		return true;
	}

	FolderRewindFormat::StoragePaths ResolveStoragePaths(const Config& config, const wstring& folderName, const wstring& folderPath) {
		FolderRewindFormat::StoragePaths storagePaths;
		if (!FolderRewindFormat::TryResolveStoragePaths(config.backupPath, folderName, folderPath, storagePaths)) {
			storagePaths.folderName = FolderRewindFormat::SanitizePathSegment(folderName);
			storagePaths.backupSubDir = filesystem::path(config.backupPath) / storagePaths.folderName;
			storagePaths.metadataDir = filesystem::path(config.backupPath) / FolderRewindFormat::kMetadataRootDirName / storagePaths.folderName;
			storagePaths.recordsDir = storagePaths.metadataDir / FolderRewindFormat::kMetadataRecordsDirName;
			storagePaths.statePath = storagePaths.metadataDir / FolderRewindFormat::kMetadataStateFileName;
		}
		return storagePaths;
	}

	HistoryCloudPaths BuildHistoryPaths(const Config& config, const HistoryEntry& entry) {
		HistoryCloudPaths paths;
		FolderRewindFormat::StoragePaths storagePaths = ResolveStoragePaths(config, entry.worldName, entry.worldPath);

		paths.archiveLocalPath = storagePaths.backupSubDir / entry.backupFile;
		paths.metadataDir = storagePaths.metadataDir;
		paths.metadataStateLocalPath = storagePaths.statePath;
		paths.metadataRecordLocalPath = storagePaths.recordsDir / (entry.backupFile + L".json");

		paths.archiveRemotePath = entry.cloudArchiveRemotePath.empty()
			? FolderRewindFormat::BuildArchiveRemotePath(config, storagePaths.folderName, entry.backupFile)
			: entry.cloudArchiveRemotePath;
		paths.metadataStateRemotePath = entry.cloudMetadataStateRemotePath.empty()
			? FolderRewindFormat::BuildMetadataStateRemotePath(config, storagePaths.folderName)
			: entry.cloudMetadataStateRemotePath;
		paths.metadataRecordRemotePath = entry.cloudMetadataRecordRemotePath.empty()
			? FolderRewindFormat::BuildMetadataRecordRemotePath(config, storagePaths.folderName, entry.backupFile)
			: entry.cloudMetadataRecordRemotePath;
		return paths;
	}

	ProcessSpec BuildRcloneCopyToCommand(const Config& config, const wstring& sourcePath, const wstring& destinationPath) {
		ProcessSpec spec;
		spec.executable = ResolveRcloneExecutable(config).executable;
		spec.arguments = {L"copyto", sourcePath, destinationPath, L"--progress"};
		return spec;
	}

	ProcessSpec BuildRcloneCopyCommand(const Config& config, const wstring& sourcePath, const wstring& destinationPath) {
		ProcessSpec spec;
		spec.executable = ResolveRcloneExecutable(config).executable;
		spec.arguments = {L"copy", sourcePath, destinationPath, L"--progress"};
		return spec;
	}

	CloudCommandResult ExecuteCommandWithRetry(
		const Config& config,
		int configIndex,
		Console& console,
		const ProcessSpec& command,
		const char* busyStatusKey,
		int progress) {
		CloudCommandResult result;
		const int retryCount = max(0, config.cloudRetryCount);
		for (int attempt = 0; attempt <= retryCount; ++attempt) {
			SetCloudRuntimeState(configIndex, true, progress, utf8_to_wstring(L(busyStatusKey)));

			ProcessSpec invocation = command;
			invocation.useLowPriority = config.useLowPriority;
			invocation.timeout = chrono::seconds(config.cloudTimeoutSeconds);
			invocation.workingDirectory = config.cloudWorkingDirectory;
			const auto process = ProcessRunner::Run(invocation);
			if (!process.standardOutput.empty()) console.AddLog("%s", process.standardOutput.c_str());
			if (!process.standardError.empty()) console.AddLog("%s", process.standardError.c_str());
			const bool success = process.status == ProcessStatus::Succeeded;
			const int exitCode = process.exitCode;
			const bool timedOut = process.status == ProcessStatus::TimedOut;
			const string errorMessage = !process.error.empty() ? wstring_to_utf8(process.error)
				: (!process.standardError.empty() ? process.standardError
					: "rclone failed with exit code " + to_string(exitCode));

			result.success = success;
			result.exitCode = exitCode;
			result.timedOut = timedOut;
			if (success) {
				result.message = utf8_to_wstring(L("CLOUD_TASK_COMPLETED"));
				return result;
			}

			result.message = timedOut
				? MineFormatMessage("CLOUD_TIMEOUT", config.cloudTimeoutSeconds)
				: MineFormatMessage("CLOUD_COMMAND_FAILED_WITH_CODE", exitCode);
			result.detail = utf8_to_wstring(errorMessage);

			if (attempt < retryCount) {
				console.AddLog(L("CLOUD_RETRYING"), attempt + 2, retryCount + 1);
			}
		}

		return result;
	}

	bool IsRemoteObjectMissing(const CloudCommandResult& result) {
		if (result.success || result.timedOut) return false;
		// rclone reserves 3 and 4 for directory/file not found. Other exit codes include
		// configuration, authentication, network, quota and fatal backend failures.
		if (result.exitCode == 3 || result.exitCode == 4) return true;
		wstring detail = result.detail;
		transform(detail.begin(), detail.end(), detail.begin(), ::towlower);
		if (detail.find(L"config file") != wstring::npos
			|| detail.find(L"didn't find section") != wstring::npos
			|| detail.find(L"failed to create file system") != wstring::npos
			|| detail.find(L"authentication") != wstring::npos
			|| detail.find(L"unauthorized") != wstring::npos) return false;
		return detail.find(L"object not found") != wstring::npos
			|| detail.find(L"directory not found") != wstring::npos
			|| detail.find(L"file not found") != wstring::npos;
	}

	enum class ActiveManifestLoadStatus {
		Loaded,
		NotFound,
		Failed
	};

	nlohmann::json SerializeHistoryEntryForCloud(const Config& config, const HistoryEntry& entry) {
		return FolderRewindHistoryStore::SerializeHistoryItem(config, entry);
	}

	bool TryParseHistoryEntryFromCloudJson(const nlohmann::json& item, HistoryEntry& outEntry, wstring& outConfigId) {
		outConfigId.clear();
		if (FolderRewindHistoryStore::TryParseHistoryItem(item, outEntry, outConfigId)) {
			return true;
		}

		int parsedConfigIndex = -1;
		if (!FolderRewindHistoryStore::TryParseLegacyHistoryItem(item, outEntry, parsedConfigIndex)) {
			return false;
		}

		if (parsedConfigIndex >= 0) {
			lock_guard<mutex> lock(g_appState.configsMutex);
			auto it = g_appState.configs.find(parsedConfigIndex);
			if (it != g_appState.configs.end()) {
				outConfigId = it->second.configId;
			}
		}
		if (outConfigId.empty()) {
			TryResolveKnownConfigId(outEntry, outConfigId);
		}
		outEntry.configId = outConfigId;
		return true;
	}

	bool TryParseCloudHistoryArray(const nlohmann::json& root, vector<HistoryEntry>& outEntries) {
		outEntries.clear();
		if (root.is_discarded() || !root.is_array()) {
			return false;
		}
		for (const auto& item : root) {
			HistoryEntry entry;
			wstring parsedConfigId;
			if (TryParseHistoryEntryFromCloudJson(item, entry, parsedConfigId)) {
				entry.configId = parsedConfigId;
				outEntries.push_back(std::move(entry));
			}
		}
		return root.empty() || !outEntries.empty();
	}

	filesystem::path BuildTempFilePath(const wchar_t* prefix, const wchar_t* extension) {
		wstringstream name;
		name << prefix << L"_" << chrono::steady_clock::now().time_since_epoch().count() << extension;
		return GetAppPaths().runtimeRoot / name.str();
	}

	CloudActiveHistoryManifest BuildActiveManifest(int configIndex) {
		CloudActiveHistoryManifest manifest;
		auto cfgIt = g_appState.configs.find(configIndex);
		if (cfgIt == g_appState.configs.end()) return manifest;

		manifest.configId = cfgIt->second.configId;
		manifest.configName = utf8_to_wstring(cfgIt->second.name);
		manifest.updatedAtUtc = FolderRewindFormat::MakeUtcTimestampString();
		for (const auto& entry : GetHistoryEntriesForConfig(configIndex)) {
			CloudActiveHistoryEntry item;
			item.folderPath = entry.worldPath;
			item.folderName = entry.worldName;
			item.fileName = entry.backupFile;
			item.timestamp = entry.timestamp_str;
			manifest.entries.push_back(std::move(item));
		}
		return manifest;
	}

	wstring BuildActiveManifestRemotePath(const Config& config) {
		return FolderRewindFormat::BuildActiveHistoryManifestRemotePath(config);
	}

	nlohmann::json SerializeManifest(const CloudActiveHistoryManifest& manifest) {
		nlohmann::json root;
		root["ConfigId"] = wstring_to_utf8(manifest.configId);
		root["ConfigName"] = wstring_to_utf8(manifest.configName);
		root["UpdatedAtUtc"] = wstring_to_utf8(manifest.updatedAtUtc);
		root["Entries"] = nlohmann::json::array();
		for (const auto& entry : manifest.entries) {
			nlohmann::json item;
			item["FolderPath"] = wstring_to_utf8(entry.folderPath);
			item["FolderName"] = wstring_to_utf8(entry.folderName);
			item["FileName"] = wstring_to_utf8(entry.fileName);
			item["Timestamp"] = wstring_to_utf8(entry.timestamp);
			root["Entries"].push_back(std::move(item));
		}
		return root;
	}

	bool TryParseManifest(const nlohmann::json& root, CloudActiveHistoryManifest& manifest) {
		return FolderRewindHistoryStore::TryParseActiveHistoryManifest(root, manifest);
	}

	bool ManifestContainsHistoryItem(const CloudActiveHistoryManifest& manifest, const HistoryEntry& entry) {
		return FolderRewindHistoryStore::ManifestContainsHistoryItem(manifest, entry);
	}

	bool HasRemotePrefix(const wstring& value, const wstring& root) {
		if (value.empty() || root.empty()) return false;
		const wstring normalizedValue = NormalizeRemotePath(value);
		const wstring normalizedRoot = NormalizeRemotePath(root);
		return normalizedValue == normalizedRoot
			|| normalizedValue.rfind(normalizedRoot + L"/", 0) == 0;
	}

	bool TryResolveKnownConfigId(const HistoryEntry& entry, wstring& outConfigId) {
		lock_guard<mutex> lock(g_appState.configsMutex);
		const Config* matchedConfig = nullptr;
		for (const auto& pair : g_appState.configs) {
			if (!BelongsToConfiguration(pair.second, entry)) {
				continue;
			}
			if (matchedConfig != nullptr) {
				outConfigId.clear();
				return false;
			}
			matchedConfig = &pair.second;
		}
		if (matchedConfig == nullptr || matchedConfig->configId.empty()) {
			outConfigId.clear();
			return false;
		}
		outConfigId = matchedConfig->configId;
		return true;
	}

	bool BelongsToConfiguration(const Config& config, const HistoryEntry& entry) {
		if (!entry.configId.empty() && !config.configId.empty()) {
			return _wcsicmp(entry.configId.c_str(), config.configId.c_str()) == 0;
		}

		const wstring configRoot = FolderRewindFormat::BuildConfigCloudRoot(config);
		if (HasRemotePrefix(entry.cloudArchiveRemotePath, configRoot)
			|| HasRemotePrefix(entry.cloudMetadataRecordRemotePath, configRoot)
			|| HasRemotePrefix(entry.cloudMetadataStateRemotePath, configRoot)) {
			return true;
		}
		for (const auto& world : config.worlds) {
			if (entry.worldName == world.first) return true;
		}
		return false;
	}

	CloudCommandResult UploadConfigurationHistorySnapshotNoLock(const Config& config, int configIndex, Console& console) {
		CloudCommandResult configError;
		if (!EnsureCloudConfigured(config, configError)) {
			return configError;
		}

		const filesystem::path tempHistoryPath = BuildTempFilePath(L"MineBackup_cloud_history", L".json");
		const filesystem::path tempManifestPath = BuildTempFilePath(L"MineBackup_cloud_manifest", L".json");
		const auto cleanupTempFiles = [&]() {
			error_code ec;
			filesystem::remove(tempHistoryPath, ec);
			filesystem::remove(tempManifestPath, ec);
		};

		CloudCommandResult result;
		CloudCommandResult remoteLoadResult;
		vector<HistoryEntry> mergedEntries = LoadRemoteHistoryEntriesNoLock(config, configIndex, console, remoteLoadResult);
		if (!remoteLoadResult.success) {
			if (IsRemoteObjectMissing(remoteLoadResult)) {
				// A genuinely absent remote history is the expected first-upload case.
				mergedEntries.clear();
			}
			else {
				cleanupTempFiles();
				return remoteLoadResult;
			}
		}
		mergedEntries.erase(
			remove_if(mergedEntries.begin(), mergedEntries.end(), [&](const HistoryEntry& entry) {
				return BelongsToConfiguration(config, entry);
			}),
			mergedEntries.end());
		vector<HistoryEntry> localEntries = GetHistoryEntriesForConfig(configIndex);
		mergedEntries.insert(mergedEntries.end(), localEntries.begin(), localEntries.end());

		ofstream historyOut(tempHistoryPath, ios::binary | ios::trunc);
		if (!historyOut.is_open()) {
			cleanupTempFiles();
			return MakeConfigErrorResult("CLOUD_HISTORY_EXPORT_FAILED");
		}
		nlohmann::json historyRoot = nlohmann::json::array();
		for (const auto& entry : mergedEntries) {
			historyRoot.push_back(SerializeHistoryEntryForCloud(BelongsToConfiguration(config, entry) ? config : Config{}, entry));
		}
		historyOut << historyRoot.dump(2);
		historyOut.close();

		ofstream manifestOut(tempManifestPath, ios::binary | ios::trunc);
		if (!manifestOut.is_open()) {
			cleanupTempFiles();
			return MakeConfigErrorResult("CLOUD_HISTORY_EXPORT_FAILED");
		}
		manifestOut << SerializeManifest(BuildActiveManifest(configIndex)).dump(2);
		manifestOut.close();

		const wstring historyRemotePath = FolderRewindFormat::BuildGlobalHistoryRemotePath(config);
		result = ExecuteCommandWithRetry(config, configIndex, console,
			BuildRcloneCopyToCommand(config, tempHistoryPath.wstring(), historyRemotePath),
			"CLOUD_STATUS_UPLOADING_HISTORY",
			85);
		if (!result.success) {
			cleanupTempFiles();
			return result;
		}

		const wstring manifestRemotePath = BuildActiveManifestRemotePath(config);
		CloudCommandResult manifestResult = ExecuteCommandWithRetry(config, configIndex, console,
			BuildRcloneCopyToCommand(config, tempManifestPath.wstring(), manifestRemotePath),
			"CLOUD_STATUS_UPLOADING_HISTORY",
			92);

		if (!manifestResult.success) {
			manifestResult.success = true;
			manifestResult.message = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
			cleanupTempFiles();
			return manifestResult;
		}

		result.success = true;
		result.exitCode = 0;
		result.message = utf8_to_wstring(L("CLOUD_HISTORY_UPLOAD_SUCCEEDED"));
		cleanupTempFiles();
		return result;
	}

	CloudCommandResult UploadHistoryEntryNoLock(const Config& config, int configIndex, const HistoryEntry& entry, Console& console) {
		CloudCommandResult configError;
		if (!EnsureCloudConfigured(config, configError)) {
			return configError;
		}

		const HistoryCloudPaths paths = BuildHistoryPaths(config, entry);
		if (!filesystem::exists(paths.archiveLocalPath)) {
			return MakeConfigErrorResult("CLOUD_LOCAL_ARCHIVE_MISSING", paths.archiveLocalPath.wstring());
		}

		CloudCommandResult result = ExecuteCommandWithRetry(
			config,
			configIndex,
			console,
			BuildRcloneCopyToCommand(config, paths.archiveLocalPath.wstring(), paths.archiveRemotePath),
			"CLOUD_STATUS_UPLOADING_ARCHIVE",
			40);
		if (!result.success) {
			result.message = MineFormatMessage("CLOUD_UPLOAD_FAILED", wstring_to_utf8(entry.backupFile).c_str());
			return result;
		}

		wstring warningMessage;
		const filesystem::path cloudMigrationMarker = paths.metadataDir / L".cloud-v15-migrated";
		const bool needsLegacyMetadataSnapshot = filesystem::exists(paths.metadataDir / L"metadata.json")
			&& !filesystem::exists(cloudMigrationMarker);
		if (needsLegacyMetadataSnapshot) {
			const wstring stamp = FolderRewindFormat::MakeLocalTimestampString();
			const wstring snapshotRoot = FolderRewindFormat::AppendRemotePath(config.rcloneRemotePath,
				{ L"_minebackup", L"migration-backups", L"1.15", stamp, utf8_to_wstring(config.name), entry.worldName });
			// Missing remote metadata is normal for archives that were never uploaded by 1.15.
			CloudCommandResult stateSnapshot = ExecuteCommandWithRetry(config, configIndex, console,
				BuildRcloneCopyToCommand(config, paths.metadataStateRemotePath,
					FolderRewindFormat::AppendRemotePath(snapshotRoot, { L"state.json" })), "CLOUD_STATUS_UPLOADING_METADATA", 52);
			if (!stateSnapshot.success && !IsRemoteObjectMissing(stateSnapshot)) {
				stateSnapshot.message = L"Remote metadata snapshot failed; converted metadata was not uploaded.";
				return stateSnapshot;
			}
			CloudCommandResult recordSnapshot = ExecuteCommandWithRetry(config, configIndex, console,
				BuildRcloneCopyToCommand(config, paths.metadataRecordRemotePath,
					FolderRewindFormat::AppendRemotePath(snapshotRoot, { L"records", entry.backupFile + L".json" })), "CLOUD_STATUS_UPLOADING_METADATA", 54);
			if (!recordSnapshot.success && !IsRemoteObjectMissing(recordSnapshot)) {
				recordSnapshot.message = L"Remote metadata record snapshot failed; converted metadata was not uploaded.";
				return recordSnapshot;
			}
		}
		bool metadataUploadComplete = true;
		if (filesystem::exists(paths.metadataStateLocalPath)) {
			CloudCommandResult metadataStateResult = ExecuteCommandWithRetry(
				config,
				configIndex,
				console,
				BuildRcloneCopyToCommand(config, paths.metadataStateLocalPath.wstring(), paths.metadataStateRemotePath),
				"CLOUD_STATUS_UPLOADING_METADATA",
				60);
			if (!metadataStateResult.success) {
				warningMessage = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
				metadataUploadComplete = false;
			}
		}

		if (filesystem::exists(paths.metadataRecordLocalPath)) {
			CloudCommandResult metadataRecordResult = ExecuteCommandWithRetry(
				config,
				configIndex,
				console,
				BuildRcloneCopyToCommand(config, paths.metadataRecordLocalPath.wstring(), paths.metadataRecordRemotePath),
				"CLOUD_STATUS_UPLOADING_METADATA",
				75);
			if (!metadataRecordResult.success) {
				warningMessage = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
				metadataUploadComplete = false;
			}
		}
		if (needsLegacyMetadataSnapshot && metadataUploadComplete) {
			ofstream marker(cloudMigrationMarker, ios::binary | ios::trunc);
			marker << "FolderRewind v3 metadata uploaded after 1.15 recovery snapshot.";
		}

		UpdateHistoryCloudState(
			configIndex,
			entry.worldName,
			entry.backupFile,
			true,
			GetUtcTimestampString(),
			paths.archiveRemotePath,
			paths.metadataRecordRemotePath,
			paths.metadataStateRemotePath);

		if (config.cloudSyncHistoryAfterUpload) {
			CloudCommandResult historyResult = UploadConfigurationHistorySnapshotNoLock(config, configIndex, console);
			if (!historyResult.success) {
				warningMessage = historyResult.message;
			}
		}

		result.success = true;
		result.exitCode = 0;
		result.message = warningMessage.empty()
			? MineFormatMessage("CLOUD_UPLOAD_SUCCEEDED", wstring_to_utf8(entry.backupFile).c_str())
			: warningMessage;
		return result;
	}

	CloudCommandResult DownloadHistoryEntryNoLock(const Config& config, int configIndex, const HistoryEntry& entry, Console& console) {
		CloudCommandResult configError;
		if (!EnsureCloudConfigured(config, configError)) {
			return configError;
		}

		HistoryCloudPaths paths = BuildHistoryPaths(config, entry);
		if (paths.archiveRemotePath.empty()) {
			return MakeConfigErrorResult("CLOUD_NO_REMOTE_COPY");
		}

		error_code ec;
		filesystem::create_directories(paths.archiveLocalPath.parent_path(), ec);
		filesystem::create_directories(paths.metadataDir, ec);

		CloudCommandResult result = ExecuteCommandWithRetry(
			config,
			configIndex,
			console,
			BuildRcloneCopyToCommand(config, paths.archiveRemotePath, paths.archiveLocalPath.wstring()),
			"CLOUD_STATUS_DOWNLOADING_ARCHIVE",
			45);
		if (!result.success) {
			result.message = MineFormatMessage("CLOUD_DOWNLOAD_FAILED", wstring_to_utf8(entry.backupFile).c_str());
			return result;
		}

		wstring warningMessage;
		CloudCommandResult metadataStateResult = ExecuteCommandWithRetry(
			config,
			configIndex,
			console,
			BuildRcloneCopyToCommand(config, paths.metadataStateRemotePath, paths.metadataStateLocalPath.wstring()),
			"CLOUD_STATUS_DOWNLOADING_METADATA",
			65);
		if (!metadataStateResult.success) {
			warningMessage = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
		}

		CloudCommandResult metadataRecordResult = ExecuteCommandWithRetry(
			config,
			configIndex,
			console,
			BuildRcloneCopyToCommand(config, paths.metadataRecordRemotePath, paths.metadataRecordLocalPath.wstring()),
			"CLOUD_STATUS_DOWNLOADING_METADATA",
			80);
		if (!metadataRecordResult.success) {
			warningMessage = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
		}

		// A 1.15 cloud state used the state.json destination but contained metadata v2 camelCase.
		// Convert the downloaded small JSON files locally; the archive remains untouched.
		bool downloadedLegacyState = false;
		try {
			ifstream stateIn(paths.metadataStateLocalPath, ios::binary);
			nlohmann::json stateRoot = nlohmann::json::parse(stateIn, nullptr, false);
			downloadedLegacyState = stateRoot.is_object() && stateRoot.contains("fileStates") && !stateRoot.contains("FileStates");
		}
		catch (...) {}
		if (downloadedLegacyState) {
			const filesystem::path legacySummary = paths.metadataDir / L"metadata.json";
			const filesystem::path legacyRecord = paths.metadataDir / (entry.backupFile + L".json");
			error_code migrateEc;
			filesystem::copy_file(paths.metadataStateLocalPath, legacySummary, filesystem::copy_options::overwrite_existing, migrateEc);
			migrateEc.clear();
			if (filesystem::exists(paths.metadataRecordLocalPath))
				filesystem::copy_file(paths.metadataRecordLocalPath, legacyRecord, filesystem::copy_options::overwrite_existing, migrateEc);
			filesystem::remove(paths.metadataStateLocalPath, migrateEc);
			filesystem::remove(paths.metadataRecordLocalPath, migrateEc);
			const MigrationUnitResult migrated = MigrationCoordinator::EnsureWorldMigrated(config, configIndex, entry.worldName, entry.worldPath);
			if (migrated.status == MigrationStatus::Failed || migrated.status == MigrationStatus::Degraded)
				warningMessage = L"Downloaded legacy metadata could not be migrated completely: " + migrated.message;
		}

		UpdateHistoryCloudState(
			configIndex,
			entry.worldName,
			entry.backupFile,
			true,
			entry.cloudArchivedAtUtc.empty() ? GetUtcTimestampString() : entry.cloudArchivedAtUtc,
			paths.archiveRemotePath,
			paths.metadataRecordRemotePath,
			paths.metadataStateRemotePath);

		result.success = true;
		result.exitCode = 0;
		result.message = warningMessage.empty()
			? MineFormatMessage("CLOUD_DOWNLOAD_SUCCEEDED", wstring_to_utf8(entry.backupFile).c_str())
			: warningMessage;
		return result;
	}

	vector<HistoryEntry> LoadRemoteHistoryEntriesNoLock(const Config& config, int configIndex, Console& console, CloudCommandResult& outResult) {
		vector<HistoryEntry> entries;
		const MigrationUnitResult cloudGate = MigrationCoordinator::EnsureCloudMigrated(configIndex);
		if (cloudGate.status == MigrationStatus::Failed) {
			outResult.success = false;
			outResult.message = cloudGate.message;
			return entries;
		}
		const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_cloud_analysis", L".json");
		const wstring remoteHistoryPath = FolderRewindFormat::BuildGlobalHistoryRemotePath(config);
		outResult = ExecuteCommandWithRetry(
			config,
			configIndex,
			console,
			BuildRcloneCopyToCommand(config, remoteHistoryPath, tempPath.wstring()),
			"CLOUD_STATUS_ANALYZING",
			20);
		if (!outResult.success) {
			error_code ec;
			filesystem::remove(tempPath, ec);
			return entries;
		}

		try {
			ifstream in(tempPath, ios::binary);
			nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
			if (!TryParseCloudHistoryArray(root, entries)) {
				outResult.success = false;
				outResult.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_FAILED"));
			}
			else if (IsLegacyHistoryJson(root)) {
				const bool hasUnmapped = any_of(entries.begin(), entries.end(), [](const HistoryEntry& entry) { return entry.configId.empty(); });
				if (hasUnmapped || entries.size() != root.size()) {
					outResult.success = false;
					outResult.message = L"Legacy cloud history contains entries that cannot be mapped safely; remote data was not changed.";
					MigrationCoordinator::RecordCloudMigrationResult(configIndex, MigrationStatus::Failed, outResult.message);
				}
				else {
					const wstring stamp = FolderRewindFormat::MakeLocalTimestampString();
					const wstring backupRemote = FolderRewindFormat::AppendRemotePath(config.rcloneRemotePath,
						{ L"_minebackup", L"migration-backups", L"1.15", stamp, L"history.json" });
					CloudCommandResult snapshotResult = ExecuteCommandWithRetry(config, configIndex, console,
						BuildRcloneCopyToCommand(config, remoteHistoryPath, backupRemote), "CLOUD_STATUS_ANALYZING", 24);
					if (!snapshotResult.success) {
						outResult = snapshotResult;
						outResult.message = L"Could not create the remote 1.15 history snapshot; migration was aborted.";
						MigrationCoordinator::RecordCloudMigrationResult(configIndex, MigrationStatus::Failed, outResult.message);
					}
					else {
						nlohmann::json converted = nlohmann::json::array();
						for (const auto& entry : entries) {
							const Config* owner = nullptr;
							for (const auto& [candidateIndex, candidate] : g_appState.configs)
								if (_wcsicmp(candidate.configId.c_str(), entry.configId.c_str()) == 0) { owner = &candidate; break; }
							if (!owner) { outResult.success = false; break; }
							converted.push_back(FolderRewindHistoryStore::SerializeHistoryItem(*owner, entry));
						}
						if (outResult.success) {
							ofstream convertedOut(tempPath, ios::binary | ios::trunc);
							convertedOut << converted.dump(2);
							convertedOut.close();
							outResult = ExecuteCommandWithRetry(config, configIndex, console,
								BuildRcloneCopyToCommand(config, tempPath.wstring(), remoteHistoryPath), "CLOUD_STATUS_ANALYZING", 28);
							if (outResult.success) MigrationCoordinator::RecordCloudMigrationResult(configIndex, MigrationStatus::Succeeded,
								L"Legacy cloud history migrated; archive objects were left in place.", backupRemote);
							else MigrationCoordinator::RecordCloudMigrationResult(configIndex, MigrationStatus::Failed,
								L"Uploading converted cloud history failed; the recovery snapshot is intact.", backupRemote);
						}
					}
				}
			}
		}
		catch (...) {
			outResult.success = false;
			outResult.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_FAILED"));
		}

		error_code ec;
		filesystem::remove(tempPath, ec);
		return entries;
	}

	ActiveManifestLoadStatus TryLoadActiveManifestNoLock(const Config& config, int configIndex, Console& console,
		CloudActiveHistoryManifest& outManifest, CloudCommandResult& outResult) {
		const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_cloud_active_history", L".json");
		CloudCommandResult result = ExecuteCommandWithRetry(
			config,
			configIndex,
			console,
			BuildRcloneCopyToCommand(config, BuildActiveManifestRemotePath(config), tempPath.wstring()),
			"CLOUD_STATUS_ANALYZING",
			30);
		if (!result.success) {
			if (!IsRemoteObjectMissing(result)) {
				outResult = result;
				error_code ec; filesystem::remove(tempPath, ec);
				return ActiveManifestLoadStatus::Failed;
			}
			// 1.15 stored the manifest under _minebackup. Read and convert it lazily.
			const wstring legacyRemote = FolderRewindFormat::AppendRemotePath(config.rcloneRemotePath,
				{ utf8_to_wstring(config.name), L"_minebackup", L"active-history.json" });
			result = ExecuteCommandWithRetry(config, configIndex, console,
				BuildRcloneCopyToCommand(config, legacyRemote, tempPath.wstring()), "CLOUD_STATUS_ANALYZING", 30);
			if (!result.success) {
				outResult = result;
				error_code ec; filesystem::remove(tempPath, ec);
				return IsRemoteObjectMissing(result) ? ActiveManifestLoadStatus::NotFound : ActiveManifestLoadStatus::Failed;
			}
			ifstream legacyIn(tempPath, ios::binary);
			nlohmann::json legacy = nlohmann::json::parse(legacyIn, nullptr, false);
			legacyIn.close();
			if (!legacy.is_object() || !legacy.contains("entries") || !legacy["entries"].is_array()) {
				outResult.success = false; outResult.message = L"Legacy active-history manifest is malformed.";
				error_code ec; filesystem::remove(tempPath, ec); return ActiveManifestLoadStatus::Failed;
			}
			CloudActiveHistoryManifest converted;
			converted.configId = config.configId;
			converted.configName = utf8_to_wstring(config.name);
			converted.updatedAtUtc = legacy.value("updatedAtUtc", string{}).empty()
				? FolderRewindFormat::MakeUtcTimestampString() : utf8_to_wstring(legacy.value("updatedAtUtc", string{}));
			for (const auto& item : legacy["entries"]) {
				if (!item.is_object()) continue;
				CloudActiveHistoryEntry entry;
				entry.folderPath = utf8_to_wstring(item.value("worldPath", string{}));
				entry.folderName = utf8_to_wstring(item.value("worldName", string{}));
				entry.fileName = utf8_to_wstring(item.value("backupFile", string{}));
				entry.timestamp = utf8_to_wstring(item.value("timestamp", string{}));
				if (!entry.folderName.empty() && !entry.fileName.empty()) converted.entries.push_back(std::move(entry));
			}
			const wstring stamp = FolderRewindFormat::MakeLocalTimestampString();
			const wstring backupRemote = FolderRewindFormat::AppendRemotePath(config.rcloneRemotePath,
				{ L"_minebackup", L"migration-backups", L"1.15", stamp, utf8_to_wstring(config.name), L"active-history.json" });
			CloudCommandResult snapshot = ExecuteCommandWithRetry(config, configIndex, console,
				BuildRcloneCopyToCommand(config, legacyRemote, backupRemote), "CLOUD_STATUS_ANALYZING", 31);
			if (!snapshot.success) { outResult = snapshot; error_code ec; filesystem::remove(tempPath, ec); return ActiveManifestLoadStatus::Failed; }
			ofstream convertedOut(tempPath, ios::binary | ios::trunc);
			convertedOut << SerializeManifest(converted).dump(2);
			convertedOut.close();
			CloudCommandResult upload = ExecuteCommandWithRetry(config, configIndex, console,
				BuildRcloneCopyToCommand(config, tempPath.wstring(), BuildActiveManifestRemotePath(config)), "CLOUD_STATUS_ANALYZING", 32);
			if (!upload.success) { outResult = upload; error_code ec; filesystem::remove(tempPath, ec); return ActiveManifestLoadStatus::Failed; }
			outManifest = std::move(converted);
			outResult.success = true; outResult.exitCode = 0;
			error_code ec; filesystem::remove(tempPath, ec); return ActiveManifestLoadStatus::Loaded;
		}

		bool ok = false;
		try {
			ifstream in(tempPath, ios::binary);
			nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
			if (!root.is_discarded()) {
				ok = TryParseManifest(root, outManifest);
				const bool looksLegacyManifest = root.is_object()
					&& root.find("entries") != root.end()
					&& root.find("Entries") == root.end();
				if (looksLegacyManifest) {
					ok = false;
				}
			}
		}
		catch (...) {
			ok = false;
		}

		error_code ec;
		filesystem::remove(tempPath, ec);
		if (!ok) {
			outResult.success = false;
			outResult.message = L"FolderRewind active-history manifest is malformed.";
			return ActiveManifestLoadStatus::Failed;
		}
		outResult.success = true; outResult.exitCode = 0;
		return ActiveManifestLoadStatus::Loaded;
	}

	wstring NormalizeWorldPathKey(const wstring& input) {
		filesystem::path path = filesystem::path(input).lexically_normal();
		wstring key = path.wstring();
#ifdef _WIN32
		for (wchar_t& ch : key) {
			if (ch == L'/') ch = L'\\';
			ch = static_cast<wchar_t>(towlower(ch));
		}
#endif
		return key;
	}

	bool HasLocalBackupOrMetadataInternal(const Config& config, const HistoryEntry& entry) {
		const HistoryCloudPaths paths = BuildHistoryPaths(config, entry);
		if (!filesystem::exists(paths.archiveLocalPath)) {
			return false;
		}
		if (IsIncrementalBackupType(entry.backupType) || IsIncrementalBackupType(entry.backupFile)) {
			return filesystem::exists(paths.metadataStateLocalPath) && filesystem::exists(paths.metadataRecordLocalPath);
		}
		return true;
	}
}

bool CanUseCloudActions(const Config& config) {
	CloudCommandResult result;
	return EnsureCloudConfigured(config, result);
}

bool HasHistoryCloudCopy(const HistoryEntry& entry) {
	return entry.isCloudArchived && !entry.cloudArchiveRemotePath.empty();
}

bool HasLocalBackupOrMetadata(const Config& config, const HistoryEntry& entry) {
	return HasLocalBackupOrMetadataInternal(config, entry);
}

int ResolveConfigIndexForCloud(const Config& config) {
	{
		lock_guard<mutex> lock(g_appState.configsMutex);
		for (const auto& pair : g_appState.configs) {
			const Config& candidate = pair.second;
			if (candidate.backupPath == config.backupPath
				&& candidate.saveRoot == config.saveRoot
				&& candidate.name == config.name) {
				return pair.first;
			}
		}
		for (const auto& pair : g_appState.configs) {
			const Config& candidate = pair.second;
			if (candidate.backupPath == config.backupPath
				&& candidate.saveRoot == config.saveRoot) {
				return pair.first;
			}
		}
	}
	return -1;
}

bool QueueUploadAfterBackup(const Config& config, int configIndex, const MyFolder& folder, const wstring& archiveFile, const wstring& comment) {
	(void)comment;
	if (!config.cloudSyncEnabled) {
		return false;
	}

	HistoryEntry historyEntry;
	if (!TryGetHistoryEntry(configIndex, folder.name, archiveFile, historyEntry)) {
		historyEntry.worldName = folder.name;
		historyEntry.worldPath = folder.path;
		historyEntry.backupFile = archiveFile;
		historyEntry.backupType = L"";
	}

	const Config configCopy = config;
	const int configIndexCopy = configIndex;
	const HistoryEntry entryCopy = historyEntry;

	return TaskCoordinator::Instance().Submit(L"cloud-upload-after-backup",
		{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
		[configCopy, configIndexCopy, entryCopy](stop_token) {
		unique_lock<mutex> lock(g_cloudMutex);
		SetCloudRuntimeState(configIndexCopy, true, 5, utf8_to_wstring(L("CLOUD_STATUS_PREPARING")));
		CloudCommandResult result = UploadHistoryEntryNoLock(configCopy, configIndexCopy, entryCopy, console);
		UpdateConfigCloudLastResult(configIndexCopy, result);
		SetCloudRuntimeState(configIndexCopy, false, 100, result.message, result.message);
	});
}

bool QueueConfigurationHistorySyncAfterLocalChange(const Config& config, int configIndex, const char* reason) {
	if (!config.cloudSyncEnabled || !CanUseCloudActions(config)) {
		return false;
	}

	const Config configCopy = config;
	const string reasonCopy = reason ? reason : "";
	return TaskCoordinator::Instance().Submit(L"cloud-history-sync",
		{TaskCoordinator::CloudResourceKey(GetAppPaths().profileIdentity)},
		[configCopy, configIndex, reasonCopy](stop_token) {
		unique_lock<mutex> lock(g_cloudMutex);
		SetCloudRuntimeState(configIndex, true, 5, utf8_to_wstring(L("CLOUD_STATUS_UPLOADING_HISTORY")));
		CloudCommandResult result = UploadConfigurationHistorySnapshotNoLock(configCopy, configIndex, console);
		UpdateConfigCloudLastResult(configIndex, result);
		if (result.success && !reasonCopy.empty()) {
			console.AddLog(L("CLOUD_BACKGROUND_HISTORY_SYNC_DONE"), reasonCopy.c_str());
		}
		SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
	});
}

CloudHistoryAnalysisResult AnalyzeCloudHistory(const Config& config, int configIndex, Console& console) {
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_ANALYZING")));

	CloudHistoryAnalysisResult analysis;
	CloudCommandResult downloadResult;
	vector<HistoryEntry> remoteEntries = LoadRemoteHistoryEntriesNoLock(config, configIndex, console, downloadResult);
	if (!downloadResult.success) {
		analysis.success = false;
		analysis.message = downloadResult.message;
		UpdateConfigCloudLastResult(configIndex, downloadResult);
		SetCloudRuntimeState(configIndex, false, 100, analysis.message, analysis.message);
		return analysis;
	}

	vector<pair<wstring, wstring>> localWorlds = config.worlds;
	map<wstring, vector<int>> worldNameMap;
	map<wstring, vector<int>> worldPathMap;
	for (int i = 0; i < static_cast<int>(localWorlds.size()); ++i) {
		worldNameMap[localWorlds[i].first].push_back(i);
		const wstring fullWorldPath = (filesystem::path(config.saveRoot) / localWorlds[i].first).wstring();
		worldPathMap[NormalizeWorldPathKey(fullWorldPath)].push_back(i);
	}

	analysis.totalRemoteEntries = static_cast<int>(remoteEntries.size());
	CloudActiveHistoryManifest activeManifest;
	CloudCommandResult manifestResult;
	const ActiveManifestLoadStatus manifestStatus = TryLoadActiveManifestNoLock(config, configIndex, console, activeManifest, manifestResult);
	if (manifestStatus == ActiveManifestLoadStatus::Failed) {
		analysis.success = false;
		analysis.message = manifestResult.message;
		UpdateConfigCloudLastResult(configIndex, manifestResult);
		SetCloudRuntimeState(configIndex, false, 100, analysis.message, analysis.message);
		return analysis;
	}
	const bool hasActiveManifest = manifestStatus == ActiveManifestLoadStatus::Loaded;
	for (auto remoteEntry : remoteEntries) {
		if (!BelongsToConfiguration(config, remoteEntry)) {
			continue;
		}
		if (hasActiveManifest && !ManifestContainsHistoryItem(activeManifest, remoteEntry)) {
			continue;
		}
		vector<int> matches;
		if (!remoteEntry.worldPath.empty()) {
			const wstring pathKey = NormalizeWorldPathKey(remoteEntry.worldPath);
			auto pathIt = worldPathMap.find(pathKey);
			if (pathIt != worldPathMap.end()) {
				matches = pathIt->second;
			}
		}

		if (matches.empty()) {
			auto nameIt = worldNameMap.find(remoteEntry.worldName);
			if (nameIt != worldNameMap.end()) {
				matches = nameIt->second;
			}
		}

		if (matches.empty()) {
			analysis.unmappedEntries++;
			continue;
		}

		if (matches.size() > 1) {
			analysis.ambiguousEntries++;
			continue;
		}

		const int worldIndex = matches.front();
		remoteEntry.worldName = localWorlds[worldIndex].first;
		remoteEntry.worldPath = (filesystem::path(config.saveRoot) / localWorlds[worldIndex].first).wstring();
		analysis.matchedEntries++;
		if (!FindHistoryEntry(configIndex, remoteEntry.worldName, remoteEntry.backupFile)) {
			analysis.importableEntries++;
		}
		analysis.mappedItems.push_back(std::move(remoteEntry));
	}

	analysis.success = true;
	analysis.message = MineFormatMessage(
		"CLOUD_ANALYSIS_SUMMARY",
		analysis.totalRemoteEntries,
		analysis.matchedEntries,
		analysis.importableEntries,
		analysis.unmappedEntries,
		analysis.ambiguousEntries);

	CloudCommandResult result;
	result.success = true;
	result.exitCode = 0;
	result.message = analysis.message;
	UpdateConfigCloudLastResult(configIndex, result);
	SetCloudRuntimeState(configIndex, false, 100, analysis.message, analysis.message);
	return analysis;
}

CloudSyncResult SyncConfigFromCloud(const Config& config, int configIndex, CloudSyncMode mode, Console& console) {
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_SYNCING")));

	CloudSyncResult syncResult;
	CloudCommandResult downloadResult;
	vector<HistoryEntry> remoteEntries = LoadRemoteHistoryEntriesNoLock(config, configIndex, console, downloadResult);
	if (!downloadResult.success) {
		syncResult.success = false;
		syncResult.message = downloadResult.message;
		UpdateConfigCloudLastResult(configIndex, downloadResult);
		SetCloudRuntimeState(configIndex, false, 100, syncResult.message, syncResult.message);
		return syncResult;
	}

	lock.unlock();
	syncResult.analysis = AnalyzeCloudHistory(config, configIndex, console);
	lock.lock();
	if (!syncResult.analysis.success) {
		syncResult.success = false;
		syncResult.message = syncResult.analysis.message;
		SetCloudRuntimeState(configIndex, false, 100, syncResult.message, syncResult.message);
		return syncResult;
	}

	int duplicates = 0;
	int imported = 0;
	for (const auto& entry : syncResult.analysis.mappedItems) {
		if (UpsertHistoryEntry(configIndex, entry, false)) {
			imported++;
		}
		else {
			duplicates++;
		}
	}
	if (imported > 0) {
		SaveHistory();
	}

	int recoveredCount = 0;
	if (mode == CloudSyncMode::HistoryAndBackups) {
		for (const auto& entry : syncResult.analysis.mappedItems) {
			if (HasLocalBackupOrMetadata(config, entry)) {
				continue;
			}
			CloudCommandResult itemResult = DownloadHistoryEntryNoLock(config, configIndex, entry, console);
			if (itemResult.success) {
				recoveredCount++;
			}
		}
	}

	syncResult.success = true;
	syncResult.importedHistoryCount = imported;
	syncResult.duplicateHistoryCount = duplicates;
	syncResult.recoveredBackupCount = recoveredCount;
	syncResult.message = (mode == CloudSyncMode::HistoryAndBackups)
		? MineFormatMessage("CLOUD_SYNC_DOWNLOADED_SUMMARY", imported, duplicates, recoveredCount)
		: MineFormatMessage("CLOUD_SYNC_HISTORY_SUMMARY", imported, duplicates);

	CloudCommandResult result;
	result.success = true;
	result.exitCode = 0;
	result.message = syncResult.message;
	UpdateConfigCloudLastResult(configIndex, result);
	SetCloudRuntimeState(configIndex, false, 100, syncResult.message, syncResult.message);
	return syncResult;
}

CloudCommandResult UploadHistoryEntry(const Config& config, int configIndex, const HistoryEntry& entry, Console& console) {
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_PREPARING")));
	const MigrationUnitResult localMigration = MigrationCoordinator::EnsureWorldMigrated(config, configIndex, entry.worldName, entry.worldPath);
	if (localMigration.status == MigrationStatus::Failed || localMigration.status == MigrationStatus::Degraded) {
		CloudCommandResult blocked;
		blocked.success = false;
		blocked.message = L"Cloud upload requires complete local metadata migration: " + localMigration.message;
		SetCloudRuntimeState(configIndex, false, 100, blocked.message, blocked.message);
		return blocked;
	}
	CloudCommandResult result = UploadHistoryEntryNoLock(config, configIndex, entry, console);
	UpdateConfigCloudLastResult(configIndex, result);
	SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
	return result;
}

CloudCommandResult DownloadHistoryEntry(const Config& config, int configIndex, const HistoryEntry& entry, Console& console) {
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_PREPARING")));
	CloudCommandResult result = DownloadHistoryEntryNoLock(config, configIndex, entry, console);
	UpdateConfigCloudLastResult(configIndex, result);
	SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
	return result;
}

CloudCommandResult UploadWorldBackupFolderToCloud(const Config& config, int configIndex, const wstring& worldName, Console& console) {
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_UPLOADING_ARCHIVE")));

	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		UpdateConfigCloudLastResult(configIndex, configError);
		SetCloudRuntimeState(configIndex, false, 100, configError.message, configError.message);
		return configError;
	}

	const FolderRewindFormat::StoragePaths storagePaths = ResolveStoragePaths(config, worldName, (filesystem::path(config.saveRoot) / worldName).wstring());
	const filesystem::path backupDir = storagePaths.backupSubDir;
	if (!filesystem::exists(backupDir)) {
		CloudCommandResult result = MakeConfigErrorResult("CLOUD_LOCAL_ARCHIVE_MISSING", backupDir.wstring());
		UpdateConfigCloudLastResult(configIndex, result);
		SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
		return result;
	}

	const wstring remoteWorldRoot = FolderRewindFormat::AppendRemotePath(
		FolderRewindFormat::BuildConfigCloudRoot(config),
		{ storagePaths.folderName });
	CloudCommandResult result = ExecuteCommandWithRetry(
		config,
		configIndex,
		console,
		BuildRcloneCopyCommand(config, backupDir.wstring(), remoteWorldRoot),
		"CLOUD_STATUS_UPLOADING_ARCHIVE",
		40);
	if (!result.success) {
		result.message = MineFormatMessage("CLOUD_UPLOAD_FOLDER_FAILED", wstring_to_utf8(worldName).c_str());
		UpdateConfigCloudLastResult(configIndex, result);
		SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
		return result;
	}

	wstring warningMessage;
	const filesystem::path metadataDir = storagePaths.metadataDir;
	if (filesystem::exists(metadataDir)) {
		CloudCommandResult metadataResult = ExecuteCommandWithRetry(
			config,
			configIndex,
			console,
			BuildRcloneCopyCommand(config, metadataDir.wstring(), FolderRewindFormat::AppendRemotePath(remoteWorldRoot, { kCloudMetadataDirName })),
			"CLOUD_STATUS_UPLOADING_METADATA",
			70);
		if (!metadataResult.success) {
			warningMessage = utf8_to_wstring(L("CLOUD_METADATA_PARTIAL"));
		}
	}

	// 目录上传后，逐条标记已有本地文件的云端路径，后续即使只删本地文件也能从云端找回。
	for (const auto& entry : GetHistoryEntriesForWorld(configIndex, worldName)) {
		HistoryCloudPaths paths = BuildHistoryPaths(config, entry);
		if (!filesystem::exists(paths.archiveLocalPath)) continue;
		UpdateHistoryCloudState(
			configIndex,
			entry.worldName,
			entry.backupFile,
			true,
			GetUtcTimestampString(),
			paths.archiveRemotePath,
			paths.metadataRecordRemotePath,
			paths.metadataStateRemotePath);
	}

	CloudCommandResult historyResult = UploadConfigurationHistorySnapshotNoLock(config, configIndex, console);
	if (!historyResult.success) {
		warningMessage = historyResult.message;
	}

	result.success = true;
	result.exitCode = 0;
	result.message = warningMessage.empty()
		? MineFormatMessage("CLOUD_UPLOAD_FOLDER_SUCCEEDED", wstring_to_utf8(worldName).c_str())
		: warningMessage;
	UpdateConfigCloudLastResult(configIndex, result);
	SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
	return result;
}

bool EnsureRestoreChainAvailable(const Config& config, int configIndex, const HistoryEntry& targetEntry) {
	if (!config.cloudAutoDownloadBeforeRestore || !CanUseCloudActions(config)) {
		return false;
	}

	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_ARCHIVE")));

	CloudCommandResult remoteLoadResult;
	vector<HistoryEntry> remoteEntries = LoadRemoteHistoryEntriesNoLock(config, configIndex, console, remoteLoadResult);
	if (remoteLoadResult.success) {
		CloudActiveHistoryManifest activeManifest;
		CloudCommandResult manifestResult;
		const ActiveManifestLoadStatus manifestStatus = TryLoadActiveManifestNoLock(config, configIndex, console, activeManifest, manifestResult);
		if (manifestStatus == ActiveManifestLoadStatus::Failed) {
			UpdateConfigCloudLastResult(configIndex, manifestResult);
			SetCloudRuntimeState(configIndex, false, 100, manifestResult.message, manifestResult.message);
			return false;
		}
		const bool hasActiveManifest = manifestStatus == ActiveManifestLoadStatus::Loaded;
		for (const auto& remoteEntry : remoteEntries) {
			if (!BelongsToConfiguration(config, remoteEntry)) continue;
			if (hasActiveManifest && !ManifestContainsHistoryItem(activeManifest, remoteEntry)) continue;
			if (remoteEntry.worldName == targetEntry.worldName) {
				UpsertHistoryEntry(configIndex, remoteEntry, false);
			}
		}
		SaveHistory();
	}

	vector<HistoryEntry> worldEntries = GetHistoryEntriesForWorld(configIndex, targetEntry.worldName);
	if (worldEntries.empty()) {
		SetCloudRuntimeState(configIndex, false, 100, utf8_to_wstring(L("CLOUD_RESTORE_CHAIN_SKIPPED")));
		return false;
	}

	auto it = find_if(worldEntries.begin(), worldEntries.end(), [&](const HistoryEntry& entry) {
		return entry.backupFile == targetEntry.backupFile;
	});
	if (it == worldEntries.end()) {
		SetCloudRuntimeState(configIndex, false, 100, utf8_to_wstring(L("CLOUD_RESTORE_CHAIN_SKIPPED")));
		return false;
	}

	vector<HistoryEntry> requiredEntries;
	for (auto rit = make_reverse_iterator(it + 1); rit != worldEntries.rend(); ++rit) {
		requiredEntries.push_back(*rit);
		if (IsFullLikeBackupType(rit->backupType) || IsFullLikeBackupType(rit->backupFile)) {
			break;
		}
	}
	reverse(requiredEntries.begin(), requiredEntries.end());

	bool downloadedAny = false;
	for (const auto& entry : requiredEntries) {
		if (HasLocalBackupOrMetadata(config, entry)) {
			continue;
		}
		CloudCommandResult result = DownloadHistoryEntryNoLock(config, configIndex, entry, console);
		if (!result.success) {
			UpdateConfigCloudLastResult(configIndex, result);
			SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
			return false;
		}
		downloadedAny = true;
	}

	CloudCommandResult result;
	result.success = true;
	result.exitCode = 0;
	result.message = downloadedAny
		? utf8_to_wstring(L("CLOUD_RESTORE_CHAIN_READY"))
		: utf8_to_wstring(L("CLOUD_RESTORE_CHAIN_ALREADY_READY"));
	UpdateConfigCloudLastResult(configIndex, result);
	SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
	return true;
}

CloudCommandResult UploadConfigurationHistorySnapshot(const Config& config, int configIndex, Console& console) {
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_UPLOADING_HISTORY")));
	CloudCommandResult result = UploadConfigurationHistorySnapshotNoLock(config, configIndex, console);
	UpdateConfigCloudLastResult(configIndex, result);
	SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
	return result;
}

PortableConfigTransferPreparation PreparePortableConfigUpload(
	const Config& config,
	const map<int, Config>& localConfigs,
	Console& console) {
	PortableConfigTransferPreparation preparation;
	const int configIndex = ResolveConfigIndexForCloud(config);
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_CONFIG")));
	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		preparation.result = configError;
		UpdateConfigCloudLastResult(configIndex, preparation.result);
		SetCloudRuntimeState(configIndex, false, 100, configError.message, configError.message);
		return preparation;
	}

	PortableConfigDocument remote;
	const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_portable_config_prepare", L".json");
	CloudCommandResult download = ExecuteCommandWithRetry(
		config, configIndex, console,
		BuildRcloneCopyToCommand(config,
			AppendRemotePath(config.rcloneRemotePath, {kPortableCloudConfigFileName}), tempPath.wstring()),
		"CLOUD_STATUS_DOWNLOADING_CONFIG", 35);
	if (download.success) {
		wstring parseError;
		error_code sizeError;
		const auto fileSize = filesystem::file_size(tempPath, sizeError);
		if (sizeError || fileSize > PortableConfigDocument::MaximumBytes) {
			parseError = L"portable-config.json is unavailable or exceeds the 1 MiB safety limit.";
		}
		else {
			ifstream input(tempPath, ios::binary);
			const string content((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
			PortableConfigDocument::Parse(content, remote, parseError);
		}
		if (!parseError.empty()) {
			download.success = false;
			download.message = utf8_to_wstring(L("CLOUD_CONFIG_IMPORT_FAILED"));
			download.detail = parseError;
		}
	}
	else if (IsRemoteObjectMissing(download)) {
		download.success = true;
		download.exitCode = 0;
		download.detail.clear();
	}
	error_code ignored;
	filesystem::remove(tempPath, ignored);
	if (!download.success) {
		preparation.result = download;
		UpdateConfigCloudLastResult(configIndex, preparation.result);
		SetCloudRuntimeState(configIndex, false, 100, download.message, download.message);
		return preparation;
	}

	const auto merged = PortableConfigDocument::MergeForUpload(localConfigs, remote, preparation.preview);
	preparation.payload = merged.Serialize();
	if (preparation.payload.size() > PortableConfigDocument::MaximumBytes) {
		preparation.payload.clear();
		preparation.result.success = false;
		preparation.result.message = utf8_to_wstring(L("CLOUD_CONFIG_EXPORT_FAILED"));
		preparation.result.detail = L"The merged portable configuration exceeds the 1 MiB safety limit.";
		UpdateConfigCloudLastResult(configIndex, preparation.result);
		SetCloudRuntimeState(configIndex, false, 100, preparation.result.message, preparation.result.message);
		return preparation;
	}
	preparation.result.success = true;
	preparation.result.exitCode = 0;
	preparation.result.message = L"Portable configuration upload preview is ready.";
	SetCloudRuntimeState(configIndex, false, 100, preparation.result.message, preparation.result.message);
	return preparation;
}

PortableConfigTransferPreparation PreparePortableConfigImport(
	const Config& config,
	const map<int, Config>& localConfigs,
	Console& console) {
	PortableConfigTransferPreparation preparation;
	const int configIndex = ResolveConfigIndexForCloud(config);
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_CONFIG")));
	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		preparation.result = configError;
		UpdateConfigCloudLastResult(configIndex, preparation.result);
		SetCloudRuntimeState(configIndex, false, 100, configError.message, configError.message);
		return preparation;
	}

	const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_portable_config_import", L".json");
	preparation.result = ExecuteCommandWithRetry(
		config, configIndex, console,
		BuildRcloneCopyToCommand(config,
			AppendRemotePath(config.rcloneRemotePath, {kPortableCloudConfigFileName}), tempPath.wstring()),
		"CLOUD_STATUS_DOWNLOADING_CONFIG", 65);
	if (preparation.result.success) {
		PortableConfigDocument remote;
		wstring parseError;
		error_code sizeError;
		const auto fileSize = filesystem::file_size(tempPath, sizeError);
		if (sizeError || fileSize > PortableConfigDocument::MaximumBytes) {
			parseError = L"portable-config.json is unavailable or exceeds the 1 MiB safety limit.";
		}
		else {
			ifstream input(tempPath, ios::binary);
			const string content((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
			PortableConfigDocument::Parse(content, remote, parseError);
		}
		if (!parseError.empty()) {
			preparation.result.success = false;
			preparation.result.detail = parseError;
		}
		else {
			preparation.payload = remote.Serialize();
			preparation.preview = PortableConfigDocument::PreviewImport(localConfigs, remote);
		}
	}
	error_code ignored;
	filesystem::remove(tempPath, ignored);
	preparation.result.message = preparation.result.success
		? L"Portable configuration import preview is ready."
		: utf8_to_wstring(L("CLOUD_CONFIG_IMPORT_FAILED"));
	UpdateConfigCloudLastResult(configIndex, preparation.result);
	SetCloudRuntimeState(configIndex, false, 100, preparation.result.message, preparation.result.message);
	return preparation;
}

#if MINEBACKUP_ENABLE_V15_MIGRATION
PortableConfigTransferPreparation PrepareLegacyPortableConfigImport(
	const Config& config,
	const map<int, Config>& localConfigs,
	Console& console) {
	PortableConfigTransferPreparation preparation;
	const int configIndex = ResolveConfigIndexForCloud(config);
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_CONFIG")));
	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		preparation.result = configError;
		SetCloudRuntimeState(configIndex, false, 100, configError.message, configError.message);
		return preparation;
	}
	const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_legacy_remote_config", L".ini");
	preparation.result = ExecuteCommandWithRetry(
		config, configIndex, console,
		BuildRcloneCopyToCommand(config,
			AppendRemotePath(config.rcloneRemotePath, {L"config.ini"}), tempPath.wstring()),
		"CLOUD_STATUS_DOWNLOADING_CONFIG", 50);
	if (preparation.result.success) {
		PortableConfigDocument filtered;
		wstring filterError;
		if (!V15MigrationAdapter::ImportLegacyRemoteIni(tempPath, filtered, filterError)) {
			preparation.result.success = false;
			preparation.result.detail = filterError;
		}
		else {
			preparation.payload = filtered.Serialize();
			preparation.preview = PortableConfigDocument::PreviewImport(localConfigs, filtered);
		}
	}
	error_code ignored;
	filesystem::remove(tempPath, ignored);
	preparation.result.message = preparation.result.success
		? L"Legacy remote config.ini import preview is ready."
		: utf8_to_wstring(L("CLOUD_CONFIG_IMPORT_FAILED"));
	UpdateConfigCloudLastResult(configIndex, preparation.result);
	SetCloudRuntimeState(configIndex, false, 100, preparation.result.message, preparation.result.message);
	return preparation;
}
#endif

CloudCommandResult CommitPortableConfigUpload(
	const Config& config,
	const string& payload,
	Console& console) {
	const int configIndex = ResolveConfigIndexForCloud(config);
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_UPLOADING_CONFIG")));
	CloudCommandResult result;
	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		result = configError;
	}
	else {
		PortableConfigDocument verified;
		wstring parseError;
		if (!PortableConfigDocument::Parse(payload, verified, parseError)) {
			result = MakeConfigErrorResult("CLOUD_CONFIG_EXPORT_FAILED", parseError);
		}
		else {
			const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_portable_config_upload", L".json");
			AtomicFileWriter::WriteOptions options;
			options.keepBackup = false;
			const auto write = AtomicFileWriter::WriteText(tempPath, verified.Serialize(), options);
			if (!write.success) {
				result = MakeConfigErrorResult("CLOUD_CONFIG_EXPORT_FAILED", write.error);
			}
			else {
				result = ExecuteCommandWithRetry(
					config, configIndex, console,
					BuildRcloneCopyToCommand(config, tempPath.wstring(),
						AppendRemotePath(config.rcloneRemotePath, {kPortableCloudConfigFileName})),
					"CLOUD_STATUS_UPLOADING_CONFIG", 70);
			}
			error_code ignored;
			filesystem::remove(tempPath, ignored);
		}
	}
	result.message = result.success
		? utf8_to_wstring(L("CLOUD_CONFIG_EXPORT_SUCCEEDED"))
		: utf8_to_wstring(L("CLOUD_CONFIG_EXPORT_FAILED"));
	UpdateConfigCloudLastResult(configIndex, result);
	SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
	return result;
}

CloudCommandResult ExportHistoryToCloud(const Config& config, int configIndex, Console& console) {
	return UploadConfigurationHistorySnapshot(config, configIndex, console);
}

CloudCommandResult ImportHistoryFromCloud(const Config& config, int configIndex, bool mergeExisting, Console& console) {
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_HISTORY")));

	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		UpdateConfigCloudLastResult(configIndex, configError);
		SetCloudRuntimeState(configIndex, false, 100, configError.message, configError.message);
		return configError;
	}

	const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_cloud_history_import", L".json");
	CloudCommandResult result = ExecuteCommandWithRetry(
		config,
		configIndex,
		console,
		BuildRcloneCopyToCommand(config, FolderRewindFormat::BuildGlobalHistoryRemotePath(config), tempPath.wstring()),
		"CLOUD_STATUS_DOWNLOADING_HISTORY",
		70);

	if (result.success) {
		vector<HistoryEntry> remoteEntries;
		try {
			ifstream in(tempPath, ios::binary);
			nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
			if (!TryParseCloudHistoryArray(root, remoteEntries)) {
				result.success = false;
			}
		}
		catch (...) {
			result.success = false;
		}

		if (result.success) {
			if (!mergeExisting) {
				g_appState.g_history[configIndex].clear();
			}
			bool changed = false;
			for (const auto& entry : remoteEntries) {
				if (!BelongsToConfiguration(config, entry)) {
					continue;
				}
				changed = UpsertHistoryEntry(configIndex, entry, false) || changed;
			}
			if (changed || !mergeExisting) {
				SaveHistory();
			}
			result.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_SUCCEEDED"));
		}
		else {
			result.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_FAILED"));
		}
	}
	else {
		result.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_FAILED"));
	}

	error_code ec;
	filesystem::remove(tempPath, ec);
	UpdateConfigCloudLastResult(configIndex, result);
	SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
	return result;
}
