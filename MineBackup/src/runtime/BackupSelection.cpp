#include "BackupSelection.h"

#include "BackupService.h"
#include "text_to_text.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <locale>
#include <sstream>

using namespace std;

namespace {

wstring Trim(wstring value) {
	const auto first = value.find_first_not_of(L" \t\r\n");
	if (first == wstring::npos) return {};
	return value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
}

wstring Normalize(wstring value) {
	value = Trim(std::move(value));
	replace(value.begin(), value.end(), L'\\', L'/');
	transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return towlower(ch); });
	while (!value.empty() && value.back() == L'/') value.pop_back();
	return value;
}

bool EqualsOrUnder(const wstring& path, const wstring& rule) {
	return path == rule || (path.size() > rule.size() && path.starts_with(rule) && path[rule.size()] == L'/');
}

bool WildcardMatches(const wstring& pattern, const wstring& path) {
	// No regex engine: untrusted wildcard inputs cannot cause regex backtracking.
	size_t p = 0, s = 0, star = wstring::npos, retry = 0;
	while (s < path.size()) {
		if (p < pattern.size() && (pattern[p] == L'?' || pattern[p] == path[s])) { ++p; ++s; }
		else if (p < pattern.size() && pattern[p] == L'*') { star = p++; retry = s; }
		else if (star != wstring::npos) { p = star + 1; s = ++retry; }
		else return false;
	}
	while (p < pattern.size() && pattern[p] == L'*') ++p;
	return p == pattern.size();
}

bool SafeRelative(const wstring& path) {
	if (path.empty() || path.front() == L'/' || path.find(L':') != wstring::npos) return false;
	size_t start = 0;
	while (start <= path.size()) {
		const auto end = path.find(L'/', start);
		const auto part = path.substr(start, end == wstring::npos ? end : end - start);
		if (part.empty() || part == L"." || part == L"..") return false;
		if (any_of(part.begin(), part.end(), [](wchar_t ch) { return ch < 32; })) return false;
		if (end == wstring::npos) break;
		start = end + 1;
	}
	return true;
}

bool Contained(const filesystem::path& root, const filesystem::path& path) {
	error_code ec;
	const auto relative = filesystem::relative(path, root, ec);
	if (ec || relative.is_absolute() || relative.has_root_name()) return false;
	for (const auto& part : relative) if (part == L"..") return false;
	return true;
}

bool ParseCoordinate(const wstring& text, double& coordinate) {
	wistringstream stream(Trim(text));
	stream.imbue(locale::classic());
	stream >> coordinate;
	if (!stream || !isfinite(coordinate) || abs(coordinate) > 30000000.) return false;
	stream >> ws;
	return stream.eof();
}

bool ParseAreas(const wstring& input, set<pair<int, int>>& regions, string& error) {
	if (wstring_to_utf8(input).size() > 32768) { error = "region_input_too_large"; return false; }
	wstring lines = input;
	replace(lines.begin(), lines.end(), L'\r', L'\n');
	wistringstream stream(lines);
	wstring line;
	size_t count = 0;
	while (getline(stream, line)) {
		line = Trim(std::move(line));
		if (line.empty() || line.front() == L'#') continue;
		if (++count > 128) { error = "region_too_many_lines"; return false; }
		vector<wstring> parts;
		size_t start = 0;
		for (;;) {
			const auto end = line.find(L',', start);
			parts.push_back(line.substr(start, end == wstring::npos ? end : end - start));
			if (end == wstring::npos) break;
			start = end + 1;
		}
		double block[4]{};
		if (parts.size() != 4) { error = "invalid_region_area"; return false; }
		for (size_t i = 0; i < 4; ++i) {
			if (!ParseCoordinate(parts[i], block[i])) { error = "invalid_region_area"; return false; }
		}
		const int x1 = static_cast<int>(floor(block[0] / 512.));
		const int z1 = static_cast<int>(floor(block[1] / 512.));
		const int x2 = static_cast<int>(floor(block[2] / 512.));
		const int z2 = static_cast<int>(floor(block[3] / 512.));
		const int minX = min(x1, x2), maxX = max(x1, x2), minZ = min(z1, z2), maxZ = max(z1, z2);
		if ((static_cast<long long>(maxX) - minX + 1) * (static_cast<long long>(maxZ) - minZ + 1) > 4096) {
			error = "region_limit_exceeded"; return false;
		}
		for (int x = minX; x <= maxX; ++x) for (int z = minZ; z <= maxZ; ++z) {
			regions.emplace(x, z);
			if (regions.size() > 4096) { error = "region_limit_exceeded"; return false; }
		}
	}
	if (regions.empty()) { error = "region_area_required"; return false; }
	return true;
}

bool ParseDimensions(wstring input, set<int>& dimensions, string& error) {
	if (Trim(input).empty()) { dimensions.insert(0); return true; }
	if (input.size() > 4096) { error = "invalid_dimension"; return false; }
	replace(input.begin(), input.end(), L';', L',');
	replace(input.begin(), input.end(), L'|', L',');
	size_t start = 0;
	for (;;) {
		const auto end = input.find(L',', start);
		const auto value = Normalize(input.substr(start, end == wstring::npos ? end : end - start));
		if (value == L"overworld" || value == L"world" || value == L"minecraft:overworld") dimensions.insert(0);
		else if (value == L"nether" || value == L"the_nether" || value == L"minecraft:the_nether" || value == L"dim-1") dimensions.insert(1);
		else if (value == L"end" || value == L"the_end" || value == L"minecraft:the_end" || value == L"dim1") dimensions.insert(2);
		else { error = "invalid_dimension"; return false; }
		if (end == wstring::npos) break;
		start = end + 1;
	}
	return true;
}

} // namespace

bool BackupSelection::TryBuild(const BackupRequest& request, BackupSelection& result, string& error) {
	BackupSelection next;
	error.clear();
	if (request.backupWhitelist.size() > 65536) { error = "whitelist_too_many_rules"; return false; }
	for (const auto& raw : request.backupWhitelist) {
		auto rule = Normalize(raw);
		if (rule.empty()) continue;
		if (rule.size() > 4096) { error = "whitelist_rule_too_long"; return false; }
		if (rule.starts_with(L"regex:")) { error = "whitelist_regex_unsupported"; return false; }
		if (!SafeRelative(rule)) { error = "whitelist_relative_rule_required"; return false; }
		if (rule.find_first_of(L"*?") != wstring::npos) next.wildcards_.push_back(std::move(rule));
		else next.literals_.insert(std::move(rule));
	}
	const auto scope = Normalize(request.backupScope);
	const bool full = scope.empty() || scope == L"full" || scope == L"all" || scope == L"default" || scope == L"none";
	if (full) {
		if (!Trim(request.scopeDimensions).empty() || !Trim(request.scopeAreas).empty()) {
			error = "selected_regions_scope_required"; return false;
		}
		result = std::move(next); return true;
	}
	if (scope != L"selected-regions") { error = "unsupported_backup_scope"; return false; }
	if (request.auxiliarySource) { error = "world_scope_required"; return false; }
	set<int> dimensions;
	set<pair<int, int>> regions;
	if (!ParseDimensions(request.scopeDimensions, dimensions, error) || !ParseAreas(request.scopeAreas, regions, error)) return false;
	error_code ec;
	const auto world = filesystem::canonical(request.sourcePath, ec);
	if (ec || !filesystem::is_directory(world, ec) || ec) { error = "invalid_scope_source"; return false; }
	set<bool> families;
	for (const int dimension : dimensions) {
		vector<pair<filesystem::path, bool>> candidates;
		const auto modern = world / "dimensions" / "minecraft" / (dimension == 0 ? "overworld" : dimension == 1 ? "the_nether" : "the_end");
		auto isDirectory = [](const filesystem::path& path) { error_code ignored; return filesystem::is_directory(path, ignored) && !ignored; };
		if (isDirectory(modern)) candidates.emplace_back(modern, true);
		if (dimension == 0) {
			if (isDirectory(world / "region") || isDirectory(world / "entities") || isDirectory(world / "poi") || !isDirectory(modern)) candidates.emplace_back(world, false);
		}
		else {
			const auto child = dimension == 1 ? "DIM-1" : "DIM1";
			const auto vanilla = world / child;
			if (isDirectory(vanilla)) candidates.emplace_back(vanilla, false);
			const auto paper = world.parent_path() / (world.filename().wstring() + (dimension == 1 ? L"_nether" : L"_the_end")) / child;
			if (isDirectory(paper)) candidates.emplace_back(paper, false);
		}
		bool outside = false;
		vector<pair<filesystem::path, bool>> inside;
		for (const auto& candidate : candidates) {
			if (!Contained(world, candidate.first)) { outside = true; continue; }
			// The scanner does not descend through directory symlinks. Refuse an
			// apparently valid dimension that would silently omit all of its files.
			auto ancestor = world;
			for (const auto& component : candidate.first.lexically_relative(world)) {
				ancestor /= component;
				if (filesystem::is_symlink(ancestor, ec) || ec) {
					error = "dimension_symlink_unsupported"; return false;
				}
			}
			inside.push_back(candidate);
		}
		if (inside.empty()) { error = outside ? "dimension_outside_source" : "dimension_missing"; return false; }
		if (inside.size() != 1) { error = "dimension_layout_ambiguous"; return false; }
		families.insert(inside.front().second);
		if (families.size() > 1) { error = "dimension_layout_mixed"; return false; }
		auto root = Normalize(inside.front().first.lexically_relative(world).generic_wstring());
		if (root == L".") root.clear();
		if (!root.empty()) root += L'/';
		next.scopePrefixes_.push_back(root + L"data");
		for (const auto& [x, z] : regions) {
			const auto name = L"r." + to_wstring(x) + L"." + to_wstring(z) + L".mca";
			for (const auto* directory : {L"region/", L"entities/", L"poi/"}) next.scopeFiles_.insert(root + directory + name);
		}
		for (const auto* directory : {L"region/", L"entities/", L"poi/"}) next.scopeWildcards_.push_back(root + directory + L"c.*.*.mcc");
	}
	for (const auto* essential : {L"level.dat", L"level.dat_old", L"icon.png", L"resources.zip", L"resourcepacks", L"advancements", L"data", L"datapacks", L"generated", L"playerdata", L"players", L"stats", L"serverconfig"}) next.scopePrefixes_.push_back(essential);
	next.scoped_ = true;
	result = std::move(next); return true;
}

bool BackupSelection::Includes(const wstring& relativePath) const {
	const auto path = Normalize(relativePath);
	if (!SafeRelative(path)) return false;
	if (scoped_ && !scopeFiles_.contains(path)
		&& none_of(scopePrefixes_.begin(), scopePrefixes_.end(), [&](const auto& rule) { return EqualsOrUnder(path, rule); })
		&& none_of(scopeWildcards_.begin(), scopeWildcards_.end(), [&](const auto& rule) {
			return path.substr(0, path.find_last_of(L'/')) == rule.substr(0, rule.find_last_of(L'/')) && WildcardMatches(rule, path);
		})) return false;
	if (literals_.empty() && wildcards_.empty()) return true;
	// Literal rules match a complete contiguous run of source-relative segments.
	for (size_t start = 0; start < path.size();) {
		for (auto end = path.find(L'/', start);; end = path.find(L'/', end + 1)) {
			if (literals_.contains(path.substr(start, end == wstring::npos ? end : end - start))) return true;
			if (end == wstring::npos) break;
		}
		const auto end = path.find(L'/', start);
		if (end == wstring::npos) break;
		start = end + 1;
	}
	const auto filename = path.substr(path.find_last_of(L'/') == wstring::npos ? 0 : path.find_last_of(L'/') + 1);
	return any_of(wildcards_.begin(), wildcards_.end(), [&](const auto& rule) { return WildcardMatches(rule, path) || WildcardMatches(rule, filename); });
}
