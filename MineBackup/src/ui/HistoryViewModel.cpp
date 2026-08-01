#include "HistoryViewModel.h"

#include "text_to_text.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

using namespace std;

void HistoryWindowController::Open(
	int requestedConfigIndex,
	const optional<wstring>& initialWorld,
	const wstring& fallbackWorld) {
	if (wasOpen && lockedConfigIndex >= 0) return;
	lockedConfigIndex = requestedConfigIndex;
	worldFilter = initialWorld.value_or(fallbackWorld);
	selectedKey = {};
	restoreKey = {};
	deleteKey = {};
	commentKey = {};
	textFilter[0] = '\0';
	statusFilterIndex = 0;
	importantOnly = false;
	narrowShowDetails = false;
	requestRestorePopup = false;
	requestDeletePopup = false;
	requestCommentPopup = false;
	commentBuffer[0] = '\0';
	restoreMethod = 0;
	customRestoreItems[0] = '\0';
	deleteMode = 2;
	useSafeDelete = true;
	wasOpen = true;
}

void HistoryWindowController::Close() {
	lockedConfigIndex = -1;
	wasOpen = false;
	selectedKey = {};
	restoreKey = {};
	deleteKey = {};
	commentKey = {};
	worldFilter.clear();
	narrowShowDetails = false;
	requestRestorePopup = false;
	requestDeletePopup = false;
	requestCommentPopup = false;
}

HistoryEntryView BuildHistoryEntryView(
	const Config& config,
	const HistoryEntry& entry) {
	HistoryEntryView view;
	view.entry = entry;
	view.key = {entry.worldName, entry.backupFile};
	const filesystem::path path = filesystem::path(config.backupPath)
		/ entry.worldName / entry.backupFile;
	const bool exists = filesystem::exists(path, view.fileError);
	if (view.fileError) {
		view.status = HistoryFileStatus::Inaccessible;
		return view;
	}
	if (!exists) {
		view.status = entry.isCloudArchived && !entry.cloudArchiveRemotePath.empty()
			? HistoryFileStatus::CloudOnly : HistoryFileStatus::Missing;
		return view;
	}
	view.fileSize = filesystem::file_size(path, view.fileError);
	if (view.fileError) {
		view.status = HistoryFileStatus::Inaccessible;
		return view;
	}
	view.status = view.fileSize < 10U * 1024U
		? HistoryFileStatus::SmallFile : HistoryFileStatus::Normal;
	return view;
}

vector<HistoryEntryView> BuildHistoryEntryViews(
	const Config& config,
	const vector<HistoryEntry>& entries) {
	vector<HistoryEntryView> views;
	views.reserve(entries.size());
	for (const HistoryEntry& entry : entries) {
		views.push_back(BuildHistoryEntryView(config, entry));
	}
	return views;
}

bool MatchesHistoryStatus(HistoryFileStatus status, HistoryStatusFilter filter) {
	switch (filter) {
	case HistoryStatusFilter::All: return true;
	case HistoryStatusFilter::Normal: return status == HistoryFileStatus::Normal;
	case HistoryStatusFilter::CloudOnly: return status == HistoryFileStatus::CloudOnly;
	case HistoryStatusFilter::Missing:
		return status == HistoryFileStatus::Missing
			|| status == HistoryFileStatus::Inaccessible;
	case HistoryStatusFilter::SmallFile: return status == HistoryFileStatus::SmallFile;
	}
	return true;
}

bool ContainsHistoryText(const HistoryEntry& entry, const string& needle) {
	if (needle.empty()) return true;
	string loweredNeedle = needle;
	auto lowerAscii = [](unsigned char character) {
		return static_cast<char>(tolower(character));
	};
	transform(
		loweredNeedle.begin(),
		loweredNeedle.end(),
		loweredNeedle.begin(),
		lowerAscii);
	const auto contains = [&](const wstring& value) {
		string haystack = wstring_to_utf8(value);
		transform(haystack.begin(), haystack.end(), haystack.begin(), lowerAscii);
		return haystack.find(loweredNeedle) != string::npos;
	};
	return contains(entry.backupFile)
		|| contains(entry.comment)
		|| contains(entry.worldName);
}

vector<HistoryEntryView> FilterHistoryEntryViews(
	const vector<HistoryEntryView>& views,
	const wstring& world,
	const string& text,
	HistoryStatusFilter status,
	bool importantOnly) {
	vector<HistoryEntryView> result;
	for (const HistoryEntryView& view : views) {
		if (!world.empty() && view.entry.worldName != world) continue;
		if (importantOnly && !view.entry.isImportant) continue;
		if (!ContainsHistoryText(view.entry, text)) continue;
		if (!MatchesHistoryStatus(view.status, status)) continue;
		result.push_back(view);
	}
	sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
		return left.entry.timestamp_str > right.entry.timestamp_str;
	});
	return result;
}

const HistoryEntryView* FindHistoryEntryView(
	const vector<HistoryEntryView>& views,
	const HistoryEntryKey& key) {
	const auto found = find_if(views.begin(), views.end(), [&](const auto& view) {
		return view.key == key;
	});
	return found == views.end() ? nullptr : &*found;
}

HistoryResponsiveLayout ComputeHistoryResponsiveLayout(
	float availableWidth,
	float em,
	float spacing) {
	const float safeEm = (max)(em, 1.0f);
	const float gap = spacing > 0.0f ? spacing : safeEm * 0.5f;
	const float safeWidth = (max)(availableWidth, 0.0f);
	HistoryResponsiveLayout layout;
	layout.useSplitView = safeWidth >= 38.0f * safeEm;
	if (layout.useSplitView) {
		const float usable = (max)(safeWidth - gap, 0.0f);
		layout.listWidth = (clamp)(
			usable * 0.40f,
			safeEm * 18.0f,
			usable - safeEm * 22.0f);
		layout.detailsWidth = (max)(usable - layout.listWidth, 0.0f);
	}
	else {
		layout.listWidth = safeWidth;
	}
	return layout;
}
