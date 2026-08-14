#include "WorldIdentity.h"

#include "FolderRewindFormat.h"
#include "JobDocument.h"
#include "text_to_text.h"

#include <algorithm>
#include <map>
#include <system_error>

using namespace std;

namespace WorldIdentity {
namespace {

filesystem::path AbsoluteNormalized(const filesystem::path& path) {
	error_code error;
	const auto canonical = filesystem::weakly_canonical(path, error);
	if (!error) return canonical.lexically_normal();
	const auto absolute = filesystem::absolute(path, error);
	return (error ? path : absolute).lexically_normal();
}

wstring PathKey(const filesystem::path& path) {
	wstring value = AbsoluteNormalized(path).wstring();
#ifdef _WIN32
	transform(value.begin(), value.end(), value.begin(), ::towlower);
#endif
	return value;
}

wstring StorageKey(wstring value) {
#ifdef _WIN32
	transform(value.begin(), value.end(), value.begin(), ::towlower);
#endif
	return value;
}

bool SamePath(const filesystem::path& left, const filesystem::path& right) {
	return PathKey(left) == PathKey(right);
}

string Describe(const wstring& configId, const wstring& worldPath) {
	return wstring_to_utf8(configId + L":" + worldPath);
}

bool TryBuildNormalized(
	const Config& config,
	const wstring& normalized,
	Value& value,
	string* errorText) {
	const auto configured = find_if(config.worlds.begin(), config.worlds.end(),
		[&](const auto& world) { return world.first == normalized; });
	if (configured == config.worlds.end()) {
		if (errorText) *errorText = "World is not configured: "
			+ Describe(config.configId, normalized);
		return false;
	}
	const filesystem::path sourceRoot = AbsoluteNormalized(config.saveRoot);
	const filesystem::path sourcePath = AbsoluteNormalized(sourceRoot / normalized);
	const filesystem::path backupRoot = AbsoluteNormalized(config.backupPath);
	FolderRewindFormat::StoragePaths storage;
	if (!FolderRewindFormat::TryResolveStoragePaths(
			backupRoot.wstring(), normalized, sourcePath.wstring(), storage)) {
		if (errorText) *errorText = "Cannot resolve storage path: "
			+ Describe(config.configId, normalized);
		return false;
	}
	value.configId = config.configId;
	value.relativeWorldPath = normalized;
	value.sourcePath = sourcePath;
	value.backupRoot = backupRoot;
	value.storageFolderName = storage.folderName;
	return true;
}

vector<Value> ConfigWorldIdentities(const Config& config) {
	vector<Value> values;
	for (const auto& [worldPath, unused] : config.worlds) {
		(void)unused;
		Value value;
		if (TryBuildNormalized(config, worldPath, value, nullptr)) {
			values.push_back(std::move(value));
		}
	}
	return values;
}

bool TryBuildFromHistory(
	const Config& config,
	const HistoryEntry& entry,
	Value& value) {
	if (entry.configId != config.configId) return false;
	const auto identities = ConfigWorldIdentities(config);
	if (!entry.worldPath.empty()) {
		const auto found = find_if(identities.begin(), identities.end(), [&](const Value& candidate) {
			return SamePath(filesystem::path(entry.worldPath), candidate.sourcePath);
		});
		if (found != identities.end()
			&& count_if(identities.begin(), identities.end(), [&](const Value& candidate) {
				return SamePath(candidate.sourcePath, found->sourcePath);
			}) == 1) {
			value = *found;
			value.backupFile = entry.backupFile;
			return true;
		}
	}
	const auto found = find_if(identities.begin(), identities.end(), [&](const Value& candidate) {
		return StorageKey(candidate.storageFolderName) == StorageKey(entry.worldName);
	});
	if (found != identities.end()
		&& count_if(identities.begin(), identities.end(), [&](const Value& candidate) {
			return PathKey(candidate.backupRoot) == PathKey(found->backupRoot)
				&& StorageKey(candidate.storageFolderName)
					== StorageKey(found->storageFolderName);
		}) == 1) {
		value = *found;
		value.backupFile = entry.backupFile;
		return true;
	}
	if (entry.worldPath.empty()) {
		const auto legacy = find_if(identities.begin(), identities.end(), [&](const Value& candidate) {
			return candidate.relativeWorldPath == entry.worldName;
		});
		if (legacy != identities.end()
			&& count_if(identities.begin(), identities.end(), [&](const Value& candidate) {
				return candidate.relativeWorldPath == legacy->relativeWorldPath;
			}) == 1) {
			value = *legacy;
			value.backupFile = entry.backupFile;
			return true;
		}
	}
	return false;
}

} // namespace

bool TryBuild(
	const Config& config,
	const wstring& requestedWorldPath,
	Value& value,
	string* errorText) {
	if (errorText) errorText->clear();
	wstring normalized;
	if (!JobStorage::TryNormalizeWorldPath(requestedWorldPath, normalized)) {
		if (errorText) *errorText = "Invalid world path: "
			+ wstring_to_utf8(requestedWorldPath);
		return false;
	}
	// 展示名称、配置相对路径、绝对源路径和存储目录名不是同一概念。
	if (TryBuildNormalized(config, normalized, value, errorText)) return true;
	const auto identities = ConfigWorldIdentities(config);
	const auto found = find_if(identities.begin(), identities.end(), [&](const Value& candidate) {
		return StorageKey(candidate.storageFolderName) == StorageKey(normalized);
	});
	if (found != identities.end()
		&& count_if(identities.begin(), identities.end(), [&](const Value& candidate) {
			return PathKey(candidate.backupRoot) == PathKey(found->backupRoot)
				&& StorageKey(candidate.storageFolderName)
					== StorageKey(found->storageFolderName);
		}) == 1) {
		value = *found;
		if (errorText) errorText->clear();
		return true;
	}
	return false;
}

bool Matches(
	const Config& config,
	const wstring& requestedWorldPath,
	const HistoryEntry& entry,
	const wstring& backupFile) {
	if (entry.configId != config.configId
		|| (!backupFile.empty() && entry.backupFile != backupFile)) return false;
	Value target;
	if (!TryBuild(config, requestedWorldPath, target, nullptr)) return false;
	if (!entry.worldPath.empty()
		&& SamePath(filesystem::path(entry.worldPath), target.sourcePath)) return true;
	if (StorageKey(entry.worldName) == StorageKey(target.storageFolderName)) return true;
	if (!entry.worldPath.empty()) return false;
	const auto identities = ConfigWorldIdentities(config);
	const auto sameStorageCount = count_if(identities.begin(), identities.end(),
		[&](const Value& candidate) {
			return PathKey(candidate.backupRoot) == PathKey(target.backupRoot)
				&& StorageKey(candidate.storageFolderName)
					== StorageKey(target.storageFolderName);
		});
	// 仅兼容没有 worldPath 的旧历史，而且必须保证 storage key 唯一。
	return sameStorageCount == 1 && entry.worldName == target.relativeWorldPath;
}

bool SameHistoryEntry(
	const Config& config,
	const HistoryEntry& left,
	const HistoryEntry& right) {
	if (left.configId != config.configId || right.configId != config.configId
		|| left.backupFile != right.backupFile) return false;
	Value identity;
	if (TryBuildFromHistory(config, left, identity)) {
		return Matches(config, identity.relativeWorldPath, right, left.backupFile);
	}
	if (left.worldName != right.worldName) return false;
	const auto identities = ConfigWorldIdentities(config);
	const auto storageMatches = count_if(identities.begin(), identities.end(),
		[&](const Value& candidate) {
			return StorageKey(candidate.storageFolderName) == StorageKey(left.worldName);
		});
	const auto relativeMatches = count_if(identities.begin(), identities.end(),
		[&](const Value& candidate) {
			return candidate.relativeWorldPath == left.worldName;
		});
	// 只有旧历史的展示名在当前配置中唯一时，才允许无 worldPath 的兜底匹配。
	return storageMatches == 1 || relativeMatches == 1;
}

vector<StorageConflict> FindStorageConflicts(const map<int, Config>& configs) {
	struct Occupant {
		wstring configId;
		wstring worldPath;
		filesystem::path backupRoot;
		wstring storageFolderName;
	};
	map<wstring, Occupant> occupied;
	vector<StorageConflict> conflicts;
	for (const auto& [index, config] : configs) {
		(void)index;
		for (const auto& value : ConfigWorldIdentities(config)) {
			const wstring key = PathKey(value.backupRoot) + L"\n"
				+ StorageKey(value.storageFolderName);
			const Occupant current{
				value.configId, value.relativeWorldPath, value.backupRoot,
				value.storageFolderName};
			const auto [it, inserted] = occupied.emplace(key, current);
			if (inserted) continue;
			conflicts.push_back({
				it->second.backupRoot.wstring(), it->second.storageFolderName,
				it->second.configId, it->second.worldPath,
				current.configId, current.worldPath});
		}
	}
	return conflicts;
}

} // namespace WorldIdentity
