#include "CloudSyncService.h"

#include "ConfigManager.h"
#include "HistoryManager.h"
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
	const wchar_t* kCloudHistoryFileName = L"history.json";
	const wchar_t* kCloudConfigFileName = L"config.ini";
	const wchar_t* kCloudStateDirName = L"_minebackup";
	const wchar_t* kCloudActiveHistoryFileName = L"active-history.json";
	const wchar_t* kCloudArchivesDirName = L"archives";
	const wchar_t* kCloudMetadataDirName = L"_metadata";
	const wchar_t* kCloudMetadataRecordsDirName = L"records";

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

	bool IsCommandPathConfigured(const wstring& executablePath) {
		if (executablePath.empty()) return false;
		if (executablePath.find(L'\\') == wstring::npos
			&& executablePath.find(L'/') == wstring::npos
			&& executablePath.find(L':') == wstring::npos) {
			return true;
		}
		return filesystem::exists(executablePath);
	}

	bool IsIncrementalBackupType(const wstring& typeOrFileName) {
		return _wcsicmp(typeOrFileName.c_str(), L"Smart") == 0
			|| typeOrFileName.find(L"[Smart]") != wstring::npos;
	}

	bool IsFullLikeBackupType(const wstring& typeOrFileName) {
		return _wcsicmp(typeOrFileName.c_str(), L"Full") == 0
			|| _wcsicmp(typeOrFileName.c_str(), L"Overwrite") == 0
			|| typeOrFileName.find(L"[Full]") != wstring::npos;
	}

	wstring QuoteCommandArg(const wstring& value) {
		return L"\"" + value + L"\"";
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
		if (config.rclonePath.empty() || config.rcloneRemotePath.empty()) {
			outResult = MakeConfigErrorResult("CLOUD_CONFIG_INVALID");
			return false;
		}
		if (!IsCommandPathConfigured(config.rclonePath)) {
			outResult = MakeConfigErrorResult("CLOUD_RCLONE_NOT_FOUND");
			return false;
		}
		if (!config.cloudWorkingDirectory.empty() && !filesystem::exists(config.cloudWorkingDirectory)) {
			outResult = MakeConfigErrorResult("CLOUD_WORKDIR_MISSING", config.cloudWorkingDirectory);
			return false;
		}
		return true;
	}

	HistoryCloudPaths BuildHistoryPaths(const Config& config, const HistoryEntry& entry) {
		HistoryCloudPaths paths;
		paths.archiveLocalPath = filesystem::path(config.backupPath) / entry.worldName / entry.backupFile;
		paths.metadataDir = filesystem::path(config.backupPath) / L"_metadata" / entry.worldName;
		paths.metadataStateLocalPath = paths.metadataDir / L"metadata.json";
		paths.metadataRecordLocalPath = paths.metadataDir / (entry.backupFile + L".json");

		const wstring archiveRoot = AppendRemotePath(config.rcloneRemotePath, {
			GetConfigCloudSegment(config),
			entry.worldName
		});
		paths.archiveRemotePath = entry.cloudArchiveRemotePath.empty()
			? AppendRemotePath(archiveRoot, { entry.backupFile })
			: entry.cloudArchiveRemotePath;
		paths.metadataStateRemotePath = entry.cloudMetadataStateRemotePath.empty()
			? AppendRemotePath(archiveRoot, { kCloudMetadataDirName, L"state.json" })
			: entry.cloudMetadataStateRemotePath;
		paths.metadataRecordRemotePath = entry.cloudMetadataRecordRemotePath.empty()
			? AppendRemotePath(archiveRoot, { kCloudMetadataDirName, kCloudMetadataRecordsDirName, entry.backupFile + L".json" })
			: entry.cloudMetadataRecordRemotePath;
		return paths;
	}

	wstring BuildRcloneCopyToCommand(const Config& config, const wstring& sourcePath, const wstring& destinationPath) {
		return QuoteCommandArg(config.rclonePath) + L" copyto "
			+ QuoteCommandArg(sourcePath) + L" "
			+ QuoteCommandArg(destinationPath)
			+ L" --progress";
	}

	wstring BuildRcloneCopyCommand(const Config& config, const wstring& sourcePath, const wstring& destinationPath) {
		return QuoteCommandArg(config.rclonePath) + L" copy "
			+ QuoteCommandArg(sourcePath) + L" "
			+ QuoteCommandArg(destinationPath)
			+ L" --progress";
	}

	CloudCommandResult ExecuteCommandWithRetry(
		const Config& config,
		int configIndex,
		Console& console,
		const wstring& command,
		const char* busyStatusKey,
		int progress) {
		CloudCommandResult result;
		const int retryCount = max(0, config.cloudRetryCount);
		for (int attempt = 0; attempt <= retryCount; ++attempt) {
			SetCloudRuntimeState(configIndex, true, progress, utf8_to_wstring(L(busyStatusKey)));

			int exitCode = -1;
			bool timedOut = false;
			string errorMessage;
			const bool success = RunCommandWithResult(
				command,
				console,
				config.useLowPriority,
				config.cloudTimeoutSeconds,
				exitCode,
				timedOut,
				errorMessage,
				config.cloudWorkingDirectory);

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

	nlohmann::json SerializeHistoryEntryForCloud(const HistoryEntry& entry) {
		nlohmann::json item;
		item["timestamp"] = wstring_to_utf8(entry.timestamp_str);
		item["worldPath"] = wstring_to_utf8(entry.worldPath);
		item["worldName"] = wstring_to_utf8(entry.worldName);
		item["backupFile"] = wstring_to_utf8(entry.backupFile);
		item["backupType"] = wstring_to_utf8(entry.backupType);
		item["comment"] = wstring_to_utf8(entry.comment);
		item["isImportant"] = entry.isImportant;
		item["isCloudArchived"] = entry.isCloudArchived;
		item["cloudArchivedAtUtc"] = wstring_to_utf8(entry.cloudArchivedAtUtc);
		item["cloudArchiveRemotePath"] = wstring_to_utf8(entry.cloudArchiveRemotePath);
		item["cloudMetadataRecordRemotePath"] = wstring_to_utf8(entry.cloudMetadataRecordRemotePath);
		item["cloudMetadataStateRemotePath"] = wstring_to_utf8(entry.cloudMetadataStateRemotePath);
		return item;
	}

	bool TryParseHistoryEntryFromCloudJson(const nlohmann::json& item, HistoryEntry& outEntry) {
		if (!item.is_object()) return false;
		outEntry = HistoryEntry{};
		outEntry.timestamp_str = utf8_to_wstring(item.value("timestamp", string{}));
		outEntry.worldPath = utf8_to_wstring(item.value("worldPath", string{}));
		outEntry.worldName = utf8_to_wstring(item.value("worldName", string{}));
		outEntry.backupFile = utf8_to_wstring(item.value("backupFile", string{}));
		outEntry.backupType = utf8_to_wstring(item.value("backupType", string{}));
		outEntry.comment = utf8_to_wstring(item.value("comment", string{}));
		outEntry.isImportant = item.value("isImportant", false);
		outEntry.isCloudArchived = item.value("isCloudArchived", false);
		outEntry.cloudArchivedAtUtc = utf8_to_wstring(item.value("cloudArchivedAtUtc", string{}));
		outEntry.cloudArchiveRemotePath = utf8_to_wstring(item.value("cloudArchiveRemotePath", string{}));
		outEntry.cloudMetadataRecordRemotePath = utf8_to_wstring(item.value("cloudMetadataRecordRemotePath", string{}));
		outEntry.cloudMetadataStateRemotePath = utf8_to_wstring(item.value("cloudMetadataStateRemotePath", string{}));
		return !outEntry.worldName.empty() && !outEntry.backupFile.empty();
	}

	filesystem::path BuildTempFilePath(const wchar_t* prefix, const wchar_t* extension) {
		wstringstream name;
		name << prefix << L"_" << chrono::steady_clock::now().time_since_epoch().count() << extension;
		return filesystem::temp_directory_path() / name.str();
	}

	CloudActiveHistoryManifest BuildActiveManifest(int configIndex) {
		CloudActiveHistoryManifest manifest;
		auto cfgIt = g_appState.configs.find(configIndex);
		if (cfgIt != g_appState.configs.end()) {
			manifest.configName = utf8_to_wstring(cfgIt->second.name);
		}
		manifest.updatedAtUtc = GetUtcTimestampString();
		for (const auto& entry : GetHistoryEntriesForConfig(configIndex)) {
			CloudActiveHistoryEntry item;
			item.worldPath = entry.worldPath;
			item.worldName = entry.worldName;
			item.backupFile = entry.backupFile;
			item.timestamp = entry.timestamp_str;
			manifest.entries.push_back(std::move(item));
		}
		return manifest;
	}

	wstring BuildActiveManifestRemotePath(const Config& config) {
		return AppendRemotePath(config.rcloneRemotePath, {
			GetConfigCloudSegment(config),
			kCloudStateDirName,
			kCloudActiveHistoryFileName
		});
	}

	nlohmann::json SerializeManifest(const CloudActiveHistoryManifest& manifest) {
		nlohmann::json root;
		root["configName"] = wstring_to_utf8(manifest.configName);
		root["updatedAtUtc"] = wstring_to_utf8(manifest.updatedAtUtc);
		root["entries"] = nlohmann::json::array();
		for (const auto& entry : manifest.entries) {
			nlohmann::json item;
			item["worldPath"] = wstring_to_utf8(entry.worldPath);
			item["worldName"] = wstring_to_utf8(entry.worldName);
			item["backupFile"] = wstring_to_utf8(entry.backupFile);
			item["timestamp"] = wstring_to_utf8(entry.timestamp);
			root["entries"].push_back(std::move(item));
		}
		return root;
	}

	bool TryParseManifest(const nlohmann::json& root, CloudActiveHistoryManifest& manifest) {
		if (!root.is_object()) return false;
		manifest = CloudActiveHistoryManifest{};
		manifest.configName = utf8_to_wstring(root.value("configName", string{}));
		manifest.updatedAtUtc = utf8_to_wstring(root.value("updatedAtUtc", string{}));
		const auto entriesIt = root.find("entries");
		if (entriesIt == root.end() || !entriesIt->is_array()) return true;
		for (const auto& item : *entriesIt) {
			if (!item.is_object()) continue;
			CloudActiveHistoryEntry entry;
			entry.worldPath = utf8_to_wstring(item.value("worldPath", string{}));
			entry.worldName = utf8_to_wstring(item.value("worldName", string{}));
			entry.backupFile = utf8_to_wstring(item.value("backupFile", string{}));
			entry.timestamp = utf8_to_wstring(item.value("timestamp", string{}));
			if (!entry.worldName.empty() && !entry.backupFile.empty()) {
				manifest.entries.push_back(std::move(entry));
			}
		}
		return true;
	}

	bool ManifestContainsHistoryItem(const CloudActiveHistoryManifest& manifest, const HistoryEntry& entry) {
		for (const auto& item : manifest.entries) {
			if (item.backupFile != entry.backupFile) continue;
			if (!item.timestamp.empty() && !entry.timestamp_str.empty() && item.timestamp != entry.timestamp_str) continue;
			if (item.worldName == entry.worldName || (!item.worldPath.empty() && item.worldPath == entry.worldPath)) {
				return true;
			}
		}
		return false;
	}

	bool HasRemotePrefix(const wstring& value, const wstring& root) {
		if (value.empty() || root.empty()) return false;
		const wstring normalizedValue = NormalizeRemotePath(value);
		const wstring normalizedRoot = NormalizeRemotePath(root);
		return normalizedValue == normalizedRoot
			|| normalizedValue.rfind(normalizedRoot + L"/", 0) == 0;
	}

	bool BelongsToConfiguration(const Config& config, const HistoryEntry& entry) {
		const wstring configRoot = AppendRemotePath(config.rcloneRemotePath, { GetConfigCloudSegment(config) });
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
			// 远端首次没有 history.json 时，下载会失败；这里按空历史继续上传本配置快照。
			mergedEntries.clear();
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
			historyRoot.push_back(SerializeHistoryEntryForCloud(entry));
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

		const wstring historyRemotePath = AppendRemotePath(config.rcloneRemotePath, { kCloudHistoryFileName });
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
			}
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
		const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_cloud_analysis", L".json");
		const wstring remoteHistoryPath = AppendRemotePath(config.rcloneRemotePath, { kCloudHistoryFileName });
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
			if (root.is_array()) {
				for (const auto& item : root) {
					HistoryEntry entry;
					if (TryParseHistoryEntryFromCloudJson(item, entry)) {
						entries.push_back(std::move(entry));
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

	bool TryLoadActiveManifestNoLock(const Config& config, int configIndex, Console& console, CloudActiveHistoryManifest& outManifest) {
		const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_cloud_active_history", L".json");
		CloudCommandResult result = ExecuteCommandWithRetry(
			config,
			configIndex,
			console,
			BuildRcloneCopyToCommand(config, BuildActiveManifestRemotePath(config), tempPath.wstring()),
			"CLOUD_STATUS_ANALYZING",
			30);
		if (!result.success) {
			error_code ec;
			filesystem::remove(tempPath, ec);
			return false;
		}

		bool ok = false;
		try {
			ifstream in(tempPath, ios::binary);
			nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
			ok = !root.is_discarded() && TryParseManifest(root, outManifest);
		}
		catch (...) {
			ok = false;
		}

		error_code ec;
		filesystem::remove(tempPath, ec);
		return ok;
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

bool QueueUploadAfterBackup(const Config& config, int configIndex, const MyFolder& folder, const wstring& archiveFile, const wstring& comment, Console& console) {
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

	thread([configCopy, configIndexCopy, entryCopy, &console]() {
		unique_lock<mutex> lock(g_cloudMutex);
		SetCloudRuntimeState(configIndexCopy, true, 5, utf8_to_wstring(L("CLOUD_STATUS_PREPARING")));
		CloudCommandResult result = UploadHistoryEntryNoLock(configCopy, configIndexCopy, entryCopy, console);
		UpdateConfigCloudLastResult(configIndexCopy, result);
		SetCloudRuntimeState(configIndexCopy, false, 100, result.message, result.message);
	}).detach();
	return true;
}

bool QueueConfigurationHistorySyncAfterLocalChange(const Config& config, int configIndex, const char* reason, Console& console) {
	if (!config.cloudSyncEnabled || !CanUseCloudActions(config)) {
		return false;
	}

	const Config configCopy = config;
	thread([configCopy, configIndex, reason, &console]() {
		unique_lock<mutex> lock(g_cloudMutex);
		SetCloudRuntimeState(configIndex, true, 5, utf8_to_wstring(L("CLOUD_STATUS_UPLOADING_HISTORY")));
		CloudCommandResult result = UploadConfigurationHistorySnapshotNoLock(configCopy, configIndex, console);
		UpdateConfigCloudLastResult(configIndex, result);
		if (result.success && reason && *reason) {
			console.AddLog(L("CLOUD_BACKGROUND_HISTORY_SYNC_DONE"), reason);
		}
		SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
	}).detach();
	return true;
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
	const bool hasActiveManifest = TryLoadActiveManifestNoLock(config, configIndex, console, activeManifest);
	for (auto remoteEntry : remoteEntries) {
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

	const filesystem::path backupDir = filesystem::path(config.backupPath) / worldName;
	if (!filesystem::exists(backupDir)) {
		CloudCommandResult result = MakeConfigErrorResult("CLOUD_LOCAL_ARCHIVE_MISSING", backupDir.wstring());
		UpdateConfigCloudLastResult(configIndex, result);
		SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
		return result;
	}

	const wstring remoteWorldRoot = AppendRemotePath(config.rcloneRemotePath, {
		GetConfigCloudSegment(config),
		worldName
	});
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
	const filesystem::path metadataDir = filesystem::path(config.backupPath) / L"_metadata" / worldName;
	if (filesystem::exists(metadataDir)) {
		CloudCommandResult metadataResult = ExecuteCommandWithRetry(
			config,
			configIndex,
			console,
			BuildRcloneCopyCommand(config, metadataDir.wstring(), AppendRemotePath(remoteWorldRoot, { kCloudMetadataDirName })),
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

bool EnsureRestoreChainAvailable(const Config& config, int configIndex, const HistoryEntry& targetEntry, Console& console) {
	if (!config.cloudAutoDownloadBeforeRestore || !CanUseCloudActions(config)) {
		return false;
	}

	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_ARCHIVE")));

	CloudCommandResult remoteLoadResult;
	vector<HistoryEntry> remoteEntries = LoadRemoteHistoryEntriesNoLock(config, configIndex, console, remoteLoadResult);
	if (remoteLoadResult.success) {
		CloudActiveHistoryManifest activeManifest;
		const bool hasActiveManifest = TryLoadActiveManifestNoLock(config, configIndex, console, activeManifest);
		for (const auto& remoteEntry : remoteEntries) {
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

CloudCommandResult ExportConfigToCloud(const Config& config, Console& console) {
	const int configIndex = ResolveConfigIndexForCloud(config);
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_UPLOADING_CONFIG")));

	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		UpdateConfigCloudLastResult(configIndex, configError);
		SetCloudRuntimeState(configIndex, false, 100, configError.message, configError.message);
		return configError;
	}

	const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_cloud_config", L".ini");
	SaveConfigs(tempPath.wstring());
	CloudCommandResult result = ExecuteCommandWithRetry(
		config,
		configIndex,
		console,
		BuildRcloneCopyToCommand(config, tempPath.wstring(), AppendRemotePath(config.rcloneRemotePath, { kCloudConfigFileName })),
		"CLOUD_STATUS_UPLOADING_CONFIG",
		70);
	error_code ec;
	filesystem::remove(tempPath, ec);

	if (result.success) {
		result.message = utf8_to_wstring(L("CLOUD_CONFIG_EXPORT_SUCCEEDED"));
	}
	else {
		result.message = utf8_to_wstring(L("CLOUD_CONFIG_EXPORT_FAILED"));
	}

	UpdateConfigCloudLastResult(configIndex, result);
	SetCloudRuntimeState(configIndex, false, 100, result.message, result.message);
	return result;
}

CloudCommandResult ImportConfigFromCloud(const Config& config, Console& console) {
	const int configIndex = ResolveConfigIndexForCloud(config);
	unique_lock<mutex> lock(g_cloudMutex);
	SetCloudRuntimeState(configIndex, true, 0, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_CONFIG")));

	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		UpdateConfigCloudLastResult(configIndex, configError);
		SetCloudRuntimeState(configIndex, false, 100, configError.message, configError.message);
		return configError;
	}

	const filesystem::path tempPath = BuildTempFilePath(L"MineBackup_cloud_config_import", L".ini");
	CloudCommandResult result = ExecuteCommandWithRetry(
		config,
		configIndex,
		console,
		BuildRcloneCopyToCommand(config, AppendRemotePath(config.rcloneRemotePath, { kCloudConfigFileName }), tempPath.wstring()),
		"CLOUD_STATUS_DOWNLOADING_CONFIG",
		65);

	if (result.success) {
		LoadConfigs(wstring_to_utf8(tempPath.wstring()));
		SaveConfigs();
		result.message = utf8_to_wstring(L("CLOUD_CONFIG_IMPORT_SUCCEEDED"));
	}
	else {
		result.message = utf8_to_wstring(L("CLOUD_CONFIG_IMPORT_FAILED"));
	}

	error_code ec;
	filesystem::remove(tempPath, ec);
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
		BuildRcloneCopyToCommand(config, AppendRemotePath(config.rcloneRemotePath, { kCloudHistoryFileName }), tempPath.wstring()),
		"CLOUD_STATUS_DOWNLOADING_HISTORY",
		70);

	if (result.success && ImportHistoryFromFile(tempPath.wstring(), configIndex, mergeExisting)) {
		result.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_SUCCEEDED"));
	}
	else if (result.success) {
		result.success = false;
		result.message = utf8_to_wstring(L("CLOUD_HISTORY_IMPORT_FAILED"));
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
