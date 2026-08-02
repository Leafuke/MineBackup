#pragma once

#include "DataModels.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

enum class HistoryStatusFilter {
	All,
	Normal,
	CloudOnly,
	Missing,
	SmallFile
};

enum class HistoryFileStatus {
	Normal,
	CloudOnly,
	Missing,
	SmallFile,
	Inaccessible
};

struct HistoryEntryKey {
	std::wstring worldName;
	std::wstring backupFile;

	bool Empty() const { return worldName.empty() || backupFile.empty(); }
	friend bool operator==(const HistoryEntryKey&, const HistoryEntryKey&) = default;
};

struct HistoryEntryView {
	std::size_t entryIndex = 0;
	HistoryFileStatus status = HistoryFileStatus::Missing;
	std::uintmax_t fileSize = 0;
};

struct HistoryResponsiveLayout {
	bool useSplitView = false;
	float listWidth = 0.0f;
	float detailsWidth = 0.0f;
};

struct HistoryWindowController {
	int lockedConfigIndex = -1;
	bool wasOpen = false;
	HistoryEntryKey selectedKey;
	HistoryEntryKey restoreKey;
	HistoryEntryKey deleteKey;
	HistoryEntryKey commentKey;
	std::wstring worldFilter;
	char textFilter[256]{};
	int statusFilterIndex = 0;
	bool importantOnly = false;
	bool narrowShowDetails = false;
	bool requestRestorePopup = false;
	bool requestDeletePopup = false;
	bool requestCommentPopup = false;
	char commentBuffer[1024]{};
	int restoreMethod = 0;
	char customRestoreItems[2048]{};
	int deleteMode = 2;
	bool useSafeDelete = true;
	std::vector<HistoryEntryView> cachedViews;
	std::vector<std::size_t> filteredViewIndices;
	std::vector<std::wstring> cachedWorlds;
	std::chrono::steady_clock::time_point nextStatusScan{};
	std::uint64_t statusKeyFingerprint = 0;
	std::uint64_t filterEntryFingerprint = 0;
	std::uint64_t filterStatusGeneration = 0;
	std::uint64_t statusGeneration = 0;
	std::uint64_t worldKeyFingerprint = 0;
	std::wstring statusBackupPath;
	std::wstring cachedFilterWorld;
	std::string cachedFilterText;
	HistoryStatusFilter cachedStatusFilter = HistoryStatusFilter::All;
	bool cachedImportantOnly = false;
	bool statusCacheValid = false;
	bool filterCacheValid = false;
	bool worldCacheValid = false;

	void Open(
		int requestedConfigIndex,
		const std::optional<std::wstring>& initialWorld,
		const std::wstring& fallbackWorld);
	void Close();
	void ReleaseCaches();
	void InvalidateFileStatusCache();
	void InvalidateFilterCache();
};

HistoryEntryView ScanHistoryEntry(
	const Config& config,
	const HistoryEntry& entry,
	std::size_t entryIndex);
const std::vector<HistoryEntryView>& RefreshHistoryEntryViews(
	HistoryWindowController& controller,
	const Config& config,
	const std::vector<HistoryEntry>& entries,
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
const std::vector<std::size_t>& FilterHistoryEntryViews(
	HistoryWindowController& controller,
	const std::vector<HistoryEntry>& entries,
	const std::vector<HistoryEntryView>& views,
	const std::wstring& world,
	const std::string& text,
	HistoryStatusFilter status,
	bool importantOnly);
const HistoryEntryView* FindHistoryEntryView(
	const std::vector<HistoryEntryView>& views,
	const std::vector<HistoryEntry>& entries,
	const HistoryEntryKey& key);
const std::vector<std::wstring>& RefreshHistoryWorlds(
	HistoryWindowController& controller,
	const std::vector<HistoryEntry>& entries);
const HistoryEntry* ResolveHistoryEntryView(
	const std::vector<HistoryEntry>& entries,
	const HistoryEntryView& view);
std::size_t RemoveUnavailableHistoryEntries(
	std::vector<HistoryEntry>& entries,
	const std::vector<HistoryEntryView>& views);
bool MatchesHistoryStatus(HistoryFileStatus status, HistoryStatusFilter filter);
bool ContainsHistoryText(const HistoryEntry& entry, const std::string& needle);
HistoryResponsiveLayout ComputeHistoryResponsiveLayout(
	float availableWidth,
	float em,
	float spacing = 0.0f);
