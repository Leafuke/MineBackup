#include "RuntimeRetentionService.h"

#include "ChainSafeRetention.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "Logging.h"
#include "WorldIdentity.h"

#include <algorithm>
#include <set>

using namespace std;

RuntimeRetentionService::RuntimeRetentionService(
	HistoryRepository& history,
	filesystem::path historyFile,
	map<int, Config> configs,
	AppPaths paths,
	ArchiveRunner::ProcessExecutor processExecutor,
	ToolResolver toolResolver)
	: history_(history),
	  historyFile_(std::move(historyFile)),
	  configs_(std::move(configs)),
	  paths_(std::move(paths)),
	  processExecutor_(std::move(processExecutor)),
	  toolResolver_(toolResolver ? std::move(toolResolver) : ExternalToolManager::ResolveSevenZip) {
}

void RuntimeRetentionService::Enforce(
	const BackupRequest& request,
	const HistoryEntry& createdEntry,
	stop_token stopToken) {
	const Config& config = request.config;
	const bool overwrite=config.backupMode==3;
	const int limit=overwrite ? 1 : config.keepCount;
	if (limit <= 0 || stopToken.stop_requested()) return;
	FolderRewindFormat::StoragePaths storage;
	if (!FolderRewindFormat::TryResolveStoragePaths(
			config.backupPath,
			createdEntry.worldName,
			createdEntry.worldPath,
			storage)) return;

	vector<HistoryEntry> currentHistory = *history_.EntriesForConfig(config.configId);
	ArchiveRunner archiveRunner(
		toolResolver_(config.zipPath, paths_, stopToken), stopToken, processExecutor_);
	set<wstring> blocked;
	for (;;) {
		if (stopToken.stop_requested()) return;
		vector<filesystem::directory_entry> archives;
		error_code error;
		for (filesystem::directory_iterator iterator(storage.backupSubDir, error), end;
			!error && iterator != end; iterator.increment(error)) {
			if (iterator->is_regular_file() && (!overwrite || iterator->path().filename().wstring().starts_with(L"[Overwrite]"))) archives.push_back(*iterator);
		}
		if (error || static_cast<int>(archives.size()) <= limit) return;
		sort(archives.begin(), archives.end(), [](const auto& left, const auto& right) {
			return left.last_write_time() < right.last_write_time();
		});
		bool progress = false;
		for (const auto& archive : archives) {
			if (stopToken.stop_requested()) return;
			const wstring fileName = archive.path().filename().wstring();
			if (blocked.contains(fileName)) continue;
			const auto found = find_if(currentHistory.begin(), currentHistory.end(),
				[&](const HistoryEntry& entry) {
					return WorldIdentity::Matches(config, storage.folderName, entry, fileName);
				});
			if (found == currentHistory.end() || found->isImportant) {
				blocked.insert(fileName);
				continue;
			}

   if (overwrite) {
    if (fileName==createdEntry.backupFile) { blocked.insert(fileName); continue; }
    bool uncertain=false,referenced=false;
    for (const auto& item : filesystem::directory_iterator(storage.backupSubDir)) {
     if(!item.is_regular_file()) continue;
     FolderRewindFormat::ChangeRecord record;
     if(!FolderRewindMetadataStore::LoadRecord(storage.metadataDir,item.path().filename().wstring(),record)) {uncertain=true;break;}
     if(record.archiveFileName!=fileName && (record.previousBackupFileName==fileName || record.basedOnFullBackup==fileName)) referenced=true;
    }
    if(uncertain || referenced) {
     MB_LOG_WARNING(minebackup::logging::LogCategory::Backup,
      "backup.retention.overwrite_preserved",
      "Retained Overwrite archive {}: {}", archive.path().string(),
      uncertain ? "archive metadata is unavailable" : "archive is referenced by a backup chain");
     blocked.insert(fileName); continue;
    }
   }
			ChainSafeRetention::Request retentionRequest;
			retentionRequest.config = config;
			retentionRequest.entry = *found;
			retentionRequest.history = currentHistory;
			retentionRequest.backupDirectory = storage.backupSubDir;
			retentionRequest.metadataDirectory = storage.metadataDir;
			retentionRequest.paths = paths_;
			retentionRequest.archiveRunner = &archiveRunner;
			retentionRequest.stopToken = stopToken;
			retentionRequest.commitHistory = [&](const ChainSafeRetention::HistoryChanges& changes) {
				const auto mutation = history_.Mutate(
					config.configId, historyFile_, configs_, true,
					[&](vector<HistoryEntry>& entries) {
						return ChainSafeRetention::ApplyHistoryChanges(config, entries, changes);
					});
				if (mutation.changed && mutation.persisted) currentHistory = *history_.EntriesForConfig(config.configId);
				return mutation.changed && mutation.persisted;
			};
			const auto retention = ChainSafeRetention::Remove(std::move(retentionRequest));
			if (retention.warning) {
				MB_LOG_WARNING(minebackup::logging::LogCategory::Backup,
					"backup.retention.warning", "%s", retention.detail.c_str());
				// 链合并失败时停止本轮保留，不能继续删除更新的备份来掩盖不变量破坏。
				return;
			}
			if (retention.changed) {
				progress = true;
				break;
			}
			blocked.insert(fileName);
		}
		if (!progress) return;
	}
}
