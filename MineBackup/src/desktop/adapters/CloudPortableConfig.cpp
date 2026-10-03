#include "CloudSyncService.h"
#include "CloudSyncInternal.h"

#include "AtomicFileWriter.h"
#include "FolderRewindFormat.h"
#if MINEBACKUP_ENABLE_V15_MIGRATION
#include "V15MigrationAdapter.h"
#endif
#include "i18n.h"
#include "text_to_text.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>

using namespace std;
using namespace CloudSyncInternal;

namespace {
	class TemporaryCloudFile {
	public:
		TemporaryCloudFile(const wchar_t* prefix, const wchar_t* extension)
			: path(BuildTempFilePath(prefix, extension)) {}
		~TemporaryCloudFile() {
			error_code ignored;
			filesystem::remove(path, ignored);
		}

		filesystem::path path;
	};

	CloudCommandResult DownloadPortableDocument(
		const Config& config,
		int configIndex,
		const wchar_t* tempPrefix,
		int progress,
		bool allowMissing,
		PortableConfigDocument& document) {
		TemporaryCloudFile tempFile(tempPrefix, L".json");
		CloudCommandResult result = ExecuteCommandWithRetry(
			config,
			configIndex,
			BuildRcloneCopyToCommand(
				config,
				FolderRewindFormat::AppendRemotePath(
					config.rcloneRemotePath,
					{L"portable-config.json"}),
				tempFile.path.wstring()),
			"CLOUD_STATUS_DOWNLOADING_CONFIG",
			progress);
		if (!result.success) {
			if (allowMissing && IsRemoteObjectMissing(result)) {
				result.success = true;
				result.exitCode = 0;
				result.detail.clear();
			}
			return result;
		}

		wstring parseError;
		error_code sizeError;
		const auto fileSize = filesystem::file_size(tempFile.path, sizeError);
		if (sizeError || fileSize > PortableConfigDocument::MaximumBytes) {
			parseError = L"portable-config.json is unavailable or exceeds the 1 MiB safety limit.";
		}
		else {
			ifstream input(tempFile.path, ios::binary);
			const string content(
				(istreambuf_iterator<char>(input)),
				istreambuf_iterator<char>());
			PortableConfigDocument::Parse(content, document, parseError);
		}
		if (!parseError.empty()) {
			result.success = false;
			result.message = utf8_to_wstring(L("CLOUD_CONFIG_IMPORT_FAILED"));
			result.detail = std::move(parseError);
		}
		return result;
	}
}

PortableConfigTransferPreparation PreparePortableConfigUpload(
	const Config& config,
	const map<int, Config>& localConfigs) {
	PortableConfigTransferPreparation preparation;
	const int configIndex = ResolveConfigIndexForCloud(config);
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_CONFIG")));
	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		preparation.result = configError;
		operation.Finish(preparation.result);
		return preparation;
	}

	PortableConfigDocument remote;
	CloudCommandResult download = DownloadPortableDocument(
		config,
		configIndex,
		L"MineBackup_portable_config_prepare",
		35,
		true,
		remote);
	if (!download.success) {
		preparation.result = download;
		operation.Finish(preparation.result);
		return preparation;
	}

	const auto merged = PortableConfigDocument::MergeForUpload(localConfigs, remote, preparation.preview);
	preparation.payload = merged.Serialize();
	if (preparation.payload.size() > PortableConfigDocument::MaximumBytes) {
		preparation.payload.clear();
		preparation.result.success = false;
		preparation.result.message = utf8_to_wstring(L("CLOUD_CONFIG_EXPORT_FAILED"));
		preparation.result.detail = L"The merged portable configuration exceeds the 1 MiB safety limit.";
		operation.Finish(preparation.result);
		return preparation;
	}
	preparation.result.success = true;
	preparation.result.exitCode = 0;
	preparation.result.message = L"Portable configuration upload preview is ready.";
	operation.Finish(preparation.result, false);
	return preparation;
}

PortableConfigTransferPreparation PreparePortableConfigImport(
	const Config& config,
	const map<int, Config>& localConfigs) {
	PortableConfigTransferPreparation preparation;
	const int configIndex = ResolveConfigIndexForCloud(config);
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_CONFIG")));
	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		preparation.result = configError;
		operation.Finish(preparation.result);
		return preparation;
	}

	PortableConfigDocument remote;
	preparation.result = DownloadPortableDocument(
		config,
		configIndex,
		L"MineBackup_portable_config_import",
		65,
		false,
		remote);
	if (preparation.result.success) {
		preparation.payload = remote.Serialize();
		preparation.preview = PortableConfigDocument::PreviewImport(localConfigs, remote);
	}
	preparation.result.message = preparation.result.success
		? L"Portable configuration import preview is ready."
		: utf8_to_wstring(L("CLOUD_CONFIG_IMPORT_FAILED"));
	operation.Finish(preparation.result);
	return preparation;
}

#if MINEBACKUP_ENABLE_V15_MIGRATION
PortableConfigTransferPreparation PrepareLegacyPortableConfigImport(
	const Config& config,
	const map<int, Config>& localConfigs) {
	PortableConfigTransferPreparation preparation;
	const int configIndex = ResolveConfigIndexForCloud(config);
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_DOWNLOADING_CONFIG")));
	CloudCommandResult configError;
	if (!EnsureCloudConfigured(config, configError)) {
		preparation.result = configError;
		operation.Finish(configError.message, false, false);
		return preparation;
	}
	TemporaryCloudFile tempFile(L"MineBackup_legacy_remote_config", L".ini");
	preparation.result = ExecuteCommandWithRetry(
		config, configIndex,
		BuildRcloneCopyToCommand(config,
			FolderRewindFormat::AppendRemotePath(config.rcloneRemotePath, {L"config.ini"}), tempFile.path.wstring()),
		"CLOUD_STATUS_DOWNLOADING_CONFIG", 50);
	if (preparation.result.success) {
		PortableConfigDocument filtered;
		wstring filterError;
		if (!V15MigrationAdapter::ImportLegacyRemoteIni(tempFile.path, filtered, filterError)) {
			preparation.result.success = false;
			preparation.result.detail = filterError;
		}
		else {
			preparation.payload = filtered.Serialize();
			preparation.preview = PortableConfigDocument::PreviewImport(localConfigs, filtered);
		}
	}
	preparation.result.message = preparation.result.success
		? L"Legacy remote config.ini import preview is ready."
		: utf8_to_wstring(L("CLOUD_CONFIG_IMPORT_FAILED"));
	operation.Finish(preparation.result);
	return preparation;
}
#endif

CloudCommandResult CommitPortableConfigUpload(
	const Config& config,
	const string& payload) {
	const int configIndex = ResolveConfigIndexForCloud(config);
	unique_lock<mutex> lock(g_cloudMutex);
	CloudOperationScope operation(configIndex, utf8_to_wstring(L("CLOUD_STATUS_UPLOADING_CONFIG")));
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
			TemporaryCloudFile tempFile(L"MineBackup_portable_config_upload", L".json");
			AtomicFileWriter::WriteOptions options;
			options.keepBackup = false;
			const auto write = AtomicFileWriter::WriteText(tempFile.path, verified.Serialize(), options);
			if (!write.success) {
				result = MakeConfigErrorResult("CLOUD_CONFIG_EXPORT_FAILED", write.error);
			}
			else {
				result = ExecuteCommandWithRetry(
					config, configIndex,
					BuildRcloneCopyToCommand(config, tempFile.path.wstring(),
						FolderRewindFormat::AppendRemotePath(config.rcloneRemotePath, {L"portable-config.json"})),
					"CLOUD_STATUS_UPLOADING_CONFIG", 70);
			}
		}
	}
	result.message = result.success
		? utf8_to_wstring(L("CLOUD_CONFIG_EXPORT_SUCCEEDED"))
		: utf8_to_wstring(L("CLOUD_CONFIG_EXPORT_FAILED"));
	operation.Finish(result);
	return result;
}
