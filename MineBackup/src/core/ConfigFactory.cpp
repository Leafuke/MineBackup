#include "ConfigFactory.h"

#include "FolderRewindFormat.h"
#include "PathIdentity.h"
#include "text_to_text.h"

#include <algorithm>
#include <cwctype>
#include <set>
#include <system_error>

using namespace std;

namespace {

wstring NameKey(const string& name) {
	wstring key = utf8_to_wstring(name);
	transform(key.begin(), key.end(), key.begin(), ::towlower);
	return key;
}

wstring BackupSegmentForName(const string& name) {
	wstring segment = FolderRewindFormat::SanitizePathSegment(utf8_to_wstring(name));
	if (segment.empty()) segment = L"Minecraft";
	return segment;
}

bool ExistsAtPath(const filesystem::path& path) {
	error_code error;
	return filesystem::exists(path, error) && !error;
}

} // namespace

vector<wstring> RecommendedConfigBackupBlacklist() {
	return {
		L"session.lock",
		L"voxy",
		L"DistantHorizons.sqlite",
		L"DistantHorizons.sqlite-shm",
		L"DistantHorizons.sqlite-wal"
	};
}

Config BuildRecommendedConfig(
	const ConfigDraft& draft,
	const ConfigFactoryContext& context) {
	Config config;
	config.name = draft.name;
	config.saveRoot = draft.saveRoot.wstring();
	config.worlds = draft.worlds;
	config.backupPath = draft.backupPath.wstring();
	config.zipPath = context.resolvedSevenZip.wstring();
	config.zipFormat = L"7z";
	config.zipMethod = L"LZMA2";
	config.zipLevel = 5;
	config.backupMode = 1;
	config.keepCount = 20;
	config.backupBefore = false;
	config.skipIfUnchanged = true;
	config.maxSmartBackupsPerFull = 5;
	config.cpuThreads = 0;
	config.useLowPriority = false;
	config.cloudSyncEnabled = false;
	config.backupOnGameStart = false;
	config.blacklist = RecommendedConfigBackupBlacklist();
	config.pendingLocalBinding = false;
	return config;
}

optional<ConfigDraft> BuildCustomFolderDraft(const filesystem::path& folder) {
	if (folder.empty()) return nullopt;
	const auto normalized =
		PathIdentity::NormalizeExistingOrProspectivePath(folder);
	const auto parent = normalized.parent_path();
	const auto relative = normalized.lexically_relative(parent);
	if (parent.empty() || parent == normalized || relative.empty()
		|| relative == L"." || relative.is_absolute()) {
		return nullopt;
	}
	for (const auto& component : relative) {
		if (component == L"." || component == L"..") return nullopt;
	}

	ConfigDraft draft;
	draft.name = wstring_to_utf8(normalized.filename().wstring());
	if (draft.name.empty()) draft.name = "Custom Folder";
	draft.edition = MinecraftEdition::Unknown;
	draft.saveRoot = parent;
	draft.worlds.emplace_back(relative.wstring(), normalized.filename().wstring());
	return draft;
}

vector<ConfigDraft> ResolveUniqueConfigDrafts(
	const vector<ConfigDraft>& drafts,
	const filesystem::path& defaultBackupRoot,
	const map<int, Config>& existingConfigs) {
	set<wstring> occupiedNames;
	set<wstring> occupiedBackupPaths;
	for (const auto& [index, config] : existingConfigs) {
		(void)index;
		occupiedNames.insert(NameKey(config.name));
		if (!config.backupPath.empty()) {
			occupiedBackupPaths.insert(
				PathIdentity::BuildPathIdentityKey(config.backupPath));
		}
	}

	vector<ConfigDraft> resolved;
	resolved.reserve(drafts.size());
	for (const auto& source : drafts) {
		const wstring sourceSegment = FolderRewindFormat::SanitizePathSegment(
			utf8_to_wstring(source.name));
		const string baseName = source.name.empty() || sourceSegment.empty()
			? "Minecraft" : source.name;
		for (size_t suffix = 1;; ++suffix) {
			const string candidateName = suffix == 1
				? baseName : baseName + " (" + to_string(suffix) + ")";
			const filesystem::path candidateBackup =
				defaultBackupRoot / BackupSegmentForName(candidateName);
			const wstring nameKey = NameKey(candidateName);
			const wstring backupKey =
				PathIdentity::BuildPathIdentityKey(candidateBackup);

			// 名称和真实路径同时判重，避免不同显示名清理后落入同一目录。
			if (occupiedNames.contains(nameKey)
				|| occupiedBackupPaths.contains(backupKey)
				|| ExistsAtPath(candidateBackup)) {
				continue;
			}

			ConfigDraft candidate = source;
			candidate.name = candidateName;
			candidate.backupPath = candidateBackup;
			occupiedNames.insert(std::move(nameKey));
			occupiedBackupPaths.insert(std::move(backupKey));
			resolved.push_back(std::move(candidate));
			break;
		}
	}
	return resolved;
}
