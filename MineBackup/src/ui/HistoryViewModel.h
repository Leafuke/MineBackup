#pragma once

#include "DataModels.h"
#include "text_to_text.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
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
	HistoryEntry* entry = nullptr;
	HistoryFileStatus status = HistoryFileStatus::Missing;
	std::uintmax_t fileSize = 0;
	std::error_code fileError;
};

inline HistoryEntryView BuildHistoryEntryView(
	const Config& config,
	HistoryEntry& entry) {
	HistoryEntryView view;
	view.entry = &entry;
	const std::filesystem::path path = std::filesystem::path(config.backupPath)
		/ entry.worldName / entry.backupFile;
	const bool exists = std::filesystem::exists(path, view.fileError);
	if (view.fileError) {
		view.status = HistoryFileStatus::Inaccessible;
		return view;
	}
	if (!exists) {
		view.status = entry.isCloudArchived && !entry.cloudArchiveRemotePath.empty()
			? HistoryFileStatus::CloudOnly : HistoryFileStatus::Missing;
		return view;
	}
	view.fileSize = std::filesystem::file_size(path, view.fileError);
	if (view.fileError) {
		view.status = HistoryFileStatus::Inaccessible;
		return view;
	}
	view.status = view.fileSize < 10U * 1024U
		? HistoryFileStatus::SmallFile : HistoryFileStatus::Normal;
	return view;
}

inline bool MatchesHistoryStatus(HistoryFileStatus status, HistoryStatusFilter filter) {
	switch (filter) {
	case HistoryStatusFilter::All: return true;
	case HistoryStatusFilter::Normal: return status == HistoryFileStatus::Normal;
	case HistoryStatusFilter::CloudOnly: return status == HistoryFileStatus::CloudOnly;
	case HistoryStatusFilter::Missing: return status == HistoryFileStatus::Missing
		|| status == HistoryFileStatus::Inaccessible;
	case HistoryStatusFilter::SmallFile: return status == HistoryFileStatus::SmallFile;
	}
	return true;
}

inline bool ContainsHistoryText(const HistoryEntry& entry, const std::string& needle) {
	if (needle.empty()) return true;
	auto contains = [&](const std::wstring& value) {
		std::string haystack = wstring_to_utf8(value);
		std::string loweredNeedle = needle;
		auto lowerAscii = [](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		};
		std::transform(haystack.begin(), haystack.end(), haystack.begin(), lowerAscii);
		std::transform(loweredNeedle.begin(), loweredNeedle.end(), loweredNeedle.begin(), lowerAscii);
		return haystack.find(loweredNeedle) != std::string::npos;
	};
	return contains(entry.backupFile) || contains(entry.comment) || contains(entry.worldName);
}

inline std::vector<HistoryEntryView> BuildFilteredHistoryViews(
	const Config& config,
	std::vector<HistoryEntry>& entries,
	const std::wstring& world,
	const std::string& text,
	HistoryStatusFilter status,
	bool importantOnly) {
	std::vector<HistoryEntryView> result;
	for (HistoryEntry& entry : entries) {
		if (!world.empty() && entry.worldName != world) continue;
		if (importantOnly && !entry.isImportant) continue;
		if (!ContainsHistoryText(entry, text)) continue;
		HistoryEntryView view = BuildHistoryEntryView(config, entry);
		if (MatchesHistoryStatus(view.status, status)) result.push_back(view);
	}
	std::sort(result.begin(), result.end(), [](const HistoryEntryView& left,
		const HistoryEntryView& right) {
		return left.entry->timestamp_str > right.entry->timestamp_str;
	});
	return result;
}
