#pragma once

#include "DataModels.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace WorldIdentity {

struct Value {
	std::wstring configId;
	std::wstring relativeWorldPath;
	std::filesystem::path sourcePath;
	std::filesystem::path backupRoot;
	std::wstring storageFolderName;
	std::wstring backupFile;
};

struct StorageConflict {
	std::wstring backupRoot;
	std::wstring storageFolderName;
	std::wstring leftConfigId;
	std::wstring leftWorldPath;
	std::wstring rightConfigId;
	std::wstring rightWorldPath;
};

bool TryBuild(
	const Config& config,
	const std::wstring& requestedWorldPath,
	Value& value,
	std::string* errorText = nullptr);

bool Matches(
	const Config& config,
	const std::wstring& requestedWorldPath,
	const HistoryEntry& entry,
	const std::wstring& backupFile = L"");

bool SameHistoryEntry(
	const Config& config,
	const HistoryEntry& left,
	const HistoryEntry& right);

std::vector<StorageConflict> FindStorageConflicts(
	const std::map<int, Config>& configs);

} // namespace WorldIdentity
