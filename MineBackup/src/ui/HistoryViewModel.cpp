#include "HistoryViewModel.h"

#include "text_to_text.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>

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
	wstring().swap(worldFilter);
	narrowShowDetails = false;
	requestRestorePopup = false;
	requestDeletePopup = false;
	requestCommentPopup = false;
	ReleaseCaches();
}

void HistoryWindowController::ReleaseCaches() {
	vector<HistoryEntryView>().swap(cachedViews);
	vector<size_t>().swap(filteredViewIndices);
	vector<wstring>().swap(cachedWorlds);
	wstring().swap(statusBackupPath);
	wstring().swap(cachedFilterWorld);
	string().swap(cachedFilterText);
	statusCacheValid = false;
	filterCacheValid = false;
	worldCacheValid = false;
}

void HistoryWindowController::InvalidateFileStatusCache() {
	statusCacheValid = false;
	filterCacheValid = false;
}

void HistoryWindowController::InvalidateFilterCache() {
	filterCacheValid = false;
}

namespace {

constexpr auto kHistoryStatusCacheDuration = chrono::seconds(1);
constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

void HashBytes(uint64_t& hash, const void* data, size_t size) {
	const auto* bytes = static_cast<const unsigned char*>(data);
	for (size_t index = 0; index < size; ++index) {
		hash ^= bytes[index];
		hash *= kFnvPrime;
	}
}

void HashString(uint64_t& hash, const wstring& value) {
	HashBytes(hash, value.data(), value.size() * sizeof(wchar_t));
	const wchar_t separator = L'\0';
	HashBytes(hash, &separator, sizeof(separator));
}

uint64_t HistoryKeyFingerprint(const vector<HistoryEntry>& entries) {
	uint64_t hash = kFnvOffset;
	for (const HistoryEntry& entry : entries) {
		HashString(hash, entry.worldName);
		HashString(hash, entry.backupFile);
	}
	const size_t entryCount = entries.size();
	HashBytes(hash, &entryCount, sizeof(entryCount));
	return hash;
}

uint64_t HistoryFilterFingerprint(const vector<HistoryEntry>& entries) {
	uint64_t hash = HistoryKeyFingerprint(entries);
	for (const HistoryEntry& entry : entries) {
		HashString(hash, entry.timestamp_str);
		HashString(hash, entry.comment);
		HashBytes(hash, &entry.isImportant, sizeof(entry.isImportant));
	}
	return hash;
}

} // namespace

HistoryEntryView ScanHistoryEntry(
	const Config& config,
	const HistoryEntry& entry,
	size_t entryIndex) {
	HistoryEntryView view;
	view.entryIndex = entryIndex;
	const filesystem::path path = filesystem::path(config.backupPath)
		/ entry.worldName / entry.backupFile;
	error_code fileError;
	const bool exists = filesystem::exists(path, fileError);
	if (fileError) {
		view.status = HistoryFileStatus::Inaccessible;
		return view;
	}
	if (!exists) {
		view.status = entry.isCloudArchived && !entry.cloudArchiveRemotePath.empty()
			? HistoryFileStatus::CloudOnly : HistoryFileStatus::Missing;
		return view;
	}
	view.fileSize = filesystem::file_size(path, fileError);
	if (fileError) {
		view.status = HistoryFileStatus::Inaccessible;
		return view;
	}
	view.status = view.fileSize < 10U * 1024U
		? HistoryFileStatus::SmallFile : HistoryFileStatus::Normal;
	return view;
}

const vector<HistoryEntryView>& RefreshHistoryEntryViews(
	HistoryWindowController& controller,
	const Config& config,
	const vector<HistoryEntry>& entries,
	chrono::steady_clock::time_point now) {
	const uint64_t keyFingerprint = HistoryKeyFingerprint(entries);
	const bool keysChanged = !controller.statusCacheValid
		|| controller.statusKeyFingerprint != keyFingerprint
		|| controller.cachedViews.size() != entries.size();
	const bool pathChanged = controller.statusBackupPath != config.backupPath;
	if (keysChanged || pathChanged || now >= controller.nextStatusScan) {
		controller.cachedViews.clear();
		controller.cachedViews.reserve(entries.size());
		for (size_t index = 0; index < entries.size(); ++index) {
			controller.cachedViews.push_back(ScanHistoryEntry(config, entries[index], index));
		}
		controller.statusKeyFingerprint = keyFingerprint;
		controller.statusBackupPath = config.backupPath;
		controller.nextStatusScan = now + kHistoryStatusCacheDuration;
		controller.statusCacheValid = true;
		++controller.statusGeneration;
		controller.filterCacheValid = false;
	}
	return controller.cachedViews;
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

const vector<size_t>& FilterHistoryEntryViews(
	HistoryWindowController& controller,
	const vector<HistoryEntry>& entries,
	const vector<HistoryEntryView>& views,
	const wstring& world,
	const string& text,
	HistoryStatusFilter status,
	bool importantOnly) {
	const uint64_t entryFingerprint = HistoryFilterFingerprint(entries);
	if (controller.filterCacheValid
		&& controller.filterEntryFingerprint == entryFingerprint
		&& controller.filterStatusGeneration == controller.statusGeneration
		&& controller.cachedFilterWorld == world
		&& controller.cachedFilterText == text
		&& controller.cachedStatusFilter == status
		&& controller.cachedImportantOnly == importantOnly) {
		return controller.filteredViewIndices;
	}

	controller.filteredViewIndices.clear();
	controller.filteredViewIndices.reserve(views.size());
	for (size_t viewIndex = 0; viewIndex < views.size(); ++viewIndex) {
		const HistoryEntryView& view = views[viewIndex];
		const HistoryEntry* entry = ResolveHistoryEntryView(entries, view);
		if (!entry) continue;
		if (!world.empty() && entry->worldName != world) continue;
		if (importantOnly && !entry->isImportant) continue;
		if (!ContainsHistoryText(*entry, text)) continue;
		if (!MatchesHistoryStatus(view.status, status)) continue;
		controller.filteredViewIndices.push_back(viewIndex);
	}
	sort(controller.filteredViewIndices.begin(), controller.filteredViewIndices.end(),
		[&](size_t left, size_t right) {
			return entries[views[left].entryIndex].timestamp_str
				> entries[views[right].entryIndex].timestamp_str;
	});
	controller.filterEntryFingerprint = entryFingerprint;
	controller.filterStatusGeneration = controller.statusGeneration;
	controller.cachedFilterWorld = world;
	controller.cachedFilterText = text;
	controller.cachedStatusFilter = status;
	controller.cachedImportantOnly = importantOnly;
	controller.filterCacheValid = true;
	return controller.filteredViewIndices;
}

const HistoryEntryView* FindHistoryEntryView(
	const vector<HistoryEntryView>& views,
	const vector<HistoryEntry>& entries,
	const HistoryEntryKey& key) {
	const auto found = find_if(views.begin(), views.end(), [&](const auto& view) {
		const HistoryEntry* entry = ResolveHistoryEntryView(entries, view);
		return entry && entry->worldName == key.worldName
			&& entry->backupFile == key.backupFile;
	});
	return found == views.end() ? nullptr : &*found;
}

const vector<wstring>& RefreshHistoryWorlds(
	HistoryWindowController& controller,
	const vector<HistoryEntry>& entries) {
	const uint64_t keyFingerprint = HistoryKeyFingerprint(entries);
	if (controller.worldCacheValid
		&& controller.worldKeyFingerprint == keyFingerprint) {
		return controller.cachedWorlds;
	}
	controller.cachedWorlds.clear();
	controller.cachedWorlds.reserve(entries.size());
	for (const HistoryEntry& entry : entries) {
		controller.cachedWorlds.push_back(entry.worldName);
	}
	sort(controller.cachedWorlds.begin(), controller.cachedWorlds.end());
	controller.cachedWorlds.erase(
		unique(controller.cachedWorlds.begin(), controller.cachedWorlds.end()),
		controller.cachedWorlds.end());
	controller.worldKeyFingerprint = keyFingerprint;
	controller.worldCacheValid = true;
	return controller.cachedWorlds;
}

const HistoryEntry* ResolveHistoryEntryView(
	const vector<HistoryEntry>& entries,
	const HistoryEntryView& view) {
	return view.entryIndex < entries.size() ? &entries[view.entryIndex] : nullptr;
}

size_t RemoveUnavailableHistoryEntries(
	vector<HistoryEntry>& entries,
	const vector<HistoryEntryView>& views) {
	vector<unsigned char> remove(entries.size(), 0);
	for (const HistoryEntryView& view : views) {
		if (view.entryIndex >= remove.size()) continue;
		if (view.status == HistoryFileStatus::Missing
			|| view.status == HistoryFileStatus::Inaccessible) {
			remove[view.entryIndex] = 1;
		}
	}
	const size_t oldSize = entries.size();
	size_t destination = 0;
	for (size_t source = 0; source < oldSize; ++source) {
		if (remove[source]) continue;
		if (destination != source) entries[destination] = std::move(entries[source]);
		++destination;
	}
	entries.resize(destination);
	return oldSize - destination;
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
