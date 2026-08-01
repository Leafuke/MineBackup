#pragma once

#include "DataModels.h"

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

// 视图保存值快照；历史容器在删除、合并或云同步后重分配也不会悬垂。
struct HistoryEntryView {
	HistoryEntry entry;
	HistoryEntryKey key;
	HistoryFileStatus status = HistoryFileStatus::Missing;
	std::uintmax_t fileSize = 0;
	std::error_code fileError;
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

	void Open(
		int requestedConfigIndex,
		const std::optional<std::wstring>& initialWorld,
		const std::wstring& fallbackWorld);
	void Close();
};

HistoryEntryView BuildHistoryEntryView(
	const Config& config,
	const HistoryEntry& entry);
std::vector<HistoryEntryView> BuildHistoryEntryViews(
	const Config& config,
	const std::vector<HistoryEntry>& entries);
std::vector<HistoryEntryView> FilterHistoryEntryViews(
	const std::vector<HistoryEntryView>& views,
	const std::wstring& world,
	const std::string& text,
	HistoryStatusFilter status,
	bool importantOnly);
const HistoryEntryView* FindHistoryEntryView(
	const std::vector<HistoryEntryView>& views,
	const HistoryEntryKey& key);
bool MatchesHistoryStatus(HistoryFileStatus status, HistoryStatusFilter filter);
bool ContainsHistoryText(const HistoryEntry& entry, const std::string& needle);
HistoryResponsiveLayout ComputeHistoryResponsiveLayout(
	float availableWidth,
	float em,
	float spacing = 0.0f);
