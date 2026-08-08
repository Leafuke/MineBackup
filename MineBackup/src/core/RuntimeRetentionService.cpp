#include "RuntimeRetentionService.h"

#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include "Logging.h"

#include <algorithm>

using namespace std;

RuntimeRetentionService::RuntimeRetentionService(
	HistoryRepository& history,
	filesystem::path historyFile,
	map<int, Config> configs)
	: history_(history),
	  historyFile_(std::move(historyFile)),
	  configs_(std::move(configs)) {
}

void RuntimeRetentionService::Enforce(
	const BackupRequest& request,
	const HistoryEntry& createdEntry) {
	const Config& config = request.config;
	if (config.keepCount <= 0 || config.backupMode != 1) return;
	FolderRewindFormat::StoragePaths storage;
	if (!FolderRewindFormat::TryResolveStoragePaths(
			config.backupPath,
			createdEntry.worldName,
			createdEntry.worldPath,
			storage)) return;

	vector<filesystem::directory_entry> archives;
	error_code error;
	for (filesystem::directory_iterator iterator(storage.backupSubDir, error), end;
		!error && iterator != end; iterator.increment(error)) {
		if (iterator->is_regular_file()) archives.push_back(*iterator);
	}
	if (error || static_cast<int>(archives.size()) <= config.keepCount) return;
	sort(archives.begin(), archives.end(), [](const auto& left, const auto& right) {
		return left.last_write_time() < right.last_write_time();
	});
	const auto history = history_.EntriesForConfig(config.configId);
	int remaining = static_cast<int>(archives.size());
	for (const auto& archive : archives) {
		if (remaining <= config.keepCount) break;
		const wstring fileName = archive.path().filename().wstring();
		const auto found = find_if(history->begin(), history->end(), [&](const HistoryEntry& entry) {
			return entry.worldName == createdEntry.worldName
				&& entry.backupFile == fileName;
		});
		if (found != history->end() && found->isImportant) continue;
		error.clear();
		if (!filesystem::remove(archive.path(), error) || error) continue;
		FolderRewindMetadataStore::DeleteRecord(storage.metadataDir, fileName);
		const auto mutation = history_.Mutate(
			config.configId,
			historyFile_,
			configs_,
			true,
			[&](vector<HistoryEntry>& entries) {
				const auto before = entries.size();
				erase_if(entries, [&](const HistoryEntry& entry) {
					return entry.worldName == createdEntry.worldName
						&& entry.backupFile == fileName;
				});
				return entries.size() != before;
			});
		if (!mutation.persisted) {
			MB_LOG_WARNING(minebackup::logging::LogCategory::Backup,
				"backup.retention.history_failed",
				"Retention deleted an archive but could not persist its history removal.");
		}
		--remaining;
	}
}
