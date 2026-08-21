#include "CloudHistoryAnalysis.h"

#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "PlatformCompat.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <map>

using namespace std;

namespace {
	wstring NormalizeWorldPathKey(const wstring& input) {
		wstring key = filesystem::path(input).lexically_normal().wstring();
#ifdef _WIN32
		for (wchar_t& ch : key) {
			if (ch == L'/') ch = L'\\';
			ch = static_cast<wchar_t>(towlower(ch));
		}
#endif
		return key;
	}

	wstring NormalizeRemotePath(wstring value) {
		for (wchar_t& ch : value) {
			if (ch == L'\\') ch = L'/';
		}
		while (!value.empty() && value.back() == L'/') value.pop_back();
		return value;
	}

	bool HasRemotePrefix(const wstring& value, const wstring& root) {
		if (value.empty() || root.empty()) return false;
		const wstring normalizedValue = NormalizeRemotePath(value);
		const wstring normalizedRoot = NormalizeRemotePath(root);
		return normalizedValue == normalizedRoot
			|| normalizedValue.rfind(normalizedRoot + L"/", 0) == 0;
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
		return any_of(config.worlds.begin(), config.worlds.end(), [&](const auto& world) {
			return entry.worldName == world.first;
		});
	}

	bool AlreadyExists(const vector<HistoryEntry>& localHistory, const HistoryEntry& candidate) {
		return any_of(localHistory.begin(), localHistory.end(), [&](const HistoryEntry& local) {
			return local.worldName == candidate.worldName
				&& local.backupFile == candidate.backupFile;
		});
	}
}

CloudHistoryAnalysisResult AnalyzeRemoteHistory(
	const Config& config,
	const vector<HistoryEntry>& localHistory,
	const vector<HistoryEntry>& remoteHistory,
	const optional<CloudActiveHistoryManifest>& activeManifest) {
	CloudHistoryAnalysisResult analysis;
	analysis.totalRemoteEntries = static_cast<int>(remoteHistory.size());

	map<wstring, vector<int>> worldNameMap;
	map<wstring, vector<int>> worldPathMap;
	for (int i = 0; i < static_cast<int>(config.worlds.size()); ++i) {
		worldNameMap[config.worlds[i].first].push_back(i);
		worldPathMap[NormalizeWorldPathKey(
			(filesystem::path(config.saveRoot) / config.worlds[i].first).wstring())].push_back(i);
	}

	for (HistoryEntry remoteEntry : remoteHistory) {
		if (!BelongsToConfiguration(config, remoteEntry)) continue;
		if (activeManifest
			&& !FolderRewindHistoryStore::ManifestContainsHistoryItem(*activeManifest, remoteEntry)) {
			continue;
		}

		vector<int> matches;
		if (!remoteEntry.worldPath.empty()) {
			auto pathIt = worldPathMap.find(NormalizeWorldPathKey(remoteEntry.worldPath));
			if (pathIt != worldPathMap.end()) matches = pathIt->second;
		}
		if (matches.empty()) {
			auto nameIt = worldNameMap.find(remoteEntry.worldName);
			if (nameIt != worldNameMap.end()) matches = nameIt->second;
		}

		if (matches.empty()) {
			++analysis.unmappedEntries;
			continue;
		}
		if (matches.size() > 1) {
			++analysis.ambiguousEntries;
			continue;
		}

		const int worldIndex = matches.front();
		remoteEntry.worldName = config.worlds[worldIndex].first;
		remoteEntry.worldPath =
			(filesystem::path(config.saveRoot) / config.worlds[worldIndex].first).wstring();
		++analysis.matchedEntries;
		if (!AlreadyExists(localHistory, remoteEntry)) ++analysis.importableEntries;
		analysis.mappedItems.push_back(std::move(remoteEntry));
	}

	analysis.success = true;
	return analysis;
}
