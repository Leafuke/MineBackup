#include "BackupManager.h"

#include "AppPaths.h"
#include "AppState.h"
#include "BackupManagerInternal.h"
#include "BackupService.h"
#include "Broadcast.h"
#include "CloudSyncService.h"
#include "HistoryManager.h"
#include "KnotLinkService.h"
#include "MigrationCoordinator.h"
#include "PlatformCompat.h"
#include "RuntimeIntegration.h"
#include "TaskCoordinator.h"
#include "text_to_text.h"

#include <chrono>
#include <thread>

using namespace std;

namespace {

int ResolveLegacyConfigIndex(const MyFolder& folder) {
	return folder.configIndex >= 0
		? folder.configIndex
		: g_appState.currentConfigIndex;
}

HotBackupPreparation PrepareDesktopHotBackup(
	const BackupRequest& request,
	stop_token stopToken) {
	HotBackupPreparation result;
	if (stopToken.stop_requested()) {
		result.status = HotBackupStatus::Rejected;
		result.diagnostics.push_back({
			"backup.cancelled", DiagnosticSeverity::Warning,
			"Cancellation was requested before the hot-backup handshake."});
		return result;
	}

	const bool modAvailable = PerformModHandshake(
		"backup", wstring_to_utf8(request.world.relativePath));
	if (!modAvailable) {
		result.status = HotBackupStatus::Degraded;
		result.diagnostics.push_back({
			"knotlink.hot_backup.unavailable", DiagnosticSeverity::Warning,
			"KnotLink is unavailable; using the existing 7-Zip -ssw fallback."});
		return result;
	}

	this_thread::sleep_for(chrono::milliseconds(100));
	BroadcastEvent("pre_hot_backup", {
		{"config", to_string(request.legacyConfigIndex)},
		{"config_id", wstring_to_utf8(request.config.configId)},
		{"world", wstring_to_utf8(request.world.relativePath)}});
	const bool saved = g_appState.knotLinkMod.waitForFlag(
		&KnotLinkModInfo::worldSaveComplete,
		chrono::milliseconds(10000));
	result.status = saved && !stopToken.stop_requested()
		? HotBackupStatus::Coordinated
		: HotBackupStatus::Rejected;
	if (!saved) {
		result.diagnostics.push_back({
			"knotlink.hot_backup.timeout", DiagnosticSeverity::Error,
			"The world-save handshake timed out."});
	}
	return result;
}

} // namespace

BackupResult RunDesktopBackup(
	const MyFolder& folder,
	const wstring& comment,
	stop_token stopToken) {
	const int configIndex = ResolveLegacyConfigIndex(folder);
	BackupRequest request;
	request.config = folder.config;
	request.world = {folder.config.configId, folder.name};
	request.sourcePath = folder.path;
	request.displayName = folder.desc.empty() ? folder.name : folder.desc;
	request.comment = comment;
	request.legacyConfigIndex = configIndex;

	BackupServiceDependencies dependencies;
	dependencies.paths = GetAppPaths();
	dependencies.ensureMigration = [configIndex](const BackupRequest& value) {
		return MigrationCoordinator::EnsureWorldMigrated(
			value.config,
			configIndex,
			value.world.relativePath,
			value.sourcePath.wstring());
	};
	dependencies.isFileLocked = [](const filesystem::path& path) {
		return IsFileLocked(path.wstring());
	};
	dependencies.hotBackup = make_shared<CallbackHotBackupBridge>(PrepareDesktopHotBackup);
	dependencies.addHistory = [configIndex](const HistoryEntry& entry) {
		return UpsertHistoryEntry(configIndex, entry, false);
	};
	dependencies.removeHistory = [configIndex](
		const wstring& worldName,
		const wstring& backupFile) {
		RemoveHistoryEntry(configIndex, worldName, backupFile);
		return true;
	};
	dependencies.enforceRetention = [configIndex](
		const BackupRequest& value,
		const HistoryEntry& entry) {
		(void)entry;
		FolderRewindFormat::StoragePaths storage;
		if (!FolderRewindFormat::TryResolveStoragePaths(
				value.config.backupPath,
				value.world.relativePath,
				value.sourcePath.wstring(),
				storage)) return;
		BackupManagerInternal::LimitBackupFiles(
			value.config,
			configIndex,
			storage.backupSubDir.wstring(),
			value.config.keepCount);
	};
	dependencies.cloudPost = make_shared<CallbackCloudPostHook>([configIndex](
		const BackupRequest& value,
		const HistoryEntry& entry,
		stop_token) {
		CloudPostResult result;
		if (!value.config.cloudSyncEnabled) return result;
		MyFolder cloudFolder{
			value.sourcePath.wstring(),
			entry.worldName,
			value.displayName,
			value.config,
			configIndex,
			-1};
		if (QueueUploadAfterBackup(
				value.config,
				configIndex,
				cloudFolder,
				entry.backupFile,
				entry.comment)) {
			result.status = CloudPostStatus::Succeeded;
			result.diagnostics.push_back({
				"cloud.upload.queued", DiagnosticSeverity::Info, {}});
		}
		else {
			result.status = CloudPostStatus::Failed;
			result.diagnostics.push_back({
				"cloud.upload.queue_failed", DiagnosticSeverity::Error,
				"The desktop cloud upload could not be queued."});
		}
		return result;
	});
	dependencies.eventSink = make_shared<CallbackRuntimeEventSink>([](const BackupRuntimeEvent& event) {
		BroadcastEvent(event.eventId, event.fields);
	});

	BackupService service(std::move(dependencies));
	return service.Run(request, stopToken);
}

BackupOutcome DoBackup(const MyFolder& folder, const wstring& comment) {
	return RunDesktopBackup(
		folder,
		comment,
		TaskCoordinator::CurrentStopToken()).outcome;
}
