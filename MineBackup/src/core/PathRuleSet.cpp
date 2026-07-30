#include "PathRuleSet.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

using namespace std;

PathRuleSet::PathRuleSet(const vector<wstring>& rules) {
	literalRules_.reserve(rules.size());
	regularExpressions_.reserve(rules.size());
	for (const auto& original : rules) {
		if (original.empty()) continue;
		if (original.rfind(L"regex:", 0) == 0) {
			try {
				regularExpressions_.emplace_back(
					original.substr(6),
					regex_constants::icase | regex_constants::ECMAScript);
			}
			catch (const regex_error&) {
				// 兼容旧配置：无效正则一直被忽略，不阻断备份或还原。
			}
			continue;
		}

		LiteralRule rule;
		rule.normalized = Normalize(original);
		rule.originalPath = filesystem::path(original);
		rule.absolute = rule.originalPath.is_absolute();
		if (!rule.normalized.empty()) literalRules_.push_back(std::move(rule));
	}
}

wstring PathRuleSet::Normalize(wstring value) {
	transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
		return static_cast<wchar_t>(towlower(ch));
	});
	replace(value.begin(), value.end(), L'\\', L'/');
	while (!value.empty() && value.back() == L'/') value.pop_back();
	return value;
}

bool PathRuleSet::PathEqualsOrUnder(const wstring& path, const wstring& rule) {
	if (rule.empty()) return false;
	if (path == rule) return true;
	return path.size() > rule.size()
		&& path.rfind(rule, 0) == 0
		&& path[rule.size()] == L'/';
}

bool PathRuleSet::PathHasSegment(const wstring& path, const wstring& segment) {
	if (segment.empty() || segment.find(L'/') != wstring::npos) return false;
	size_t start = 0;
	while (start <= path.size()) {
		const size_t end = path.find(L'/', start);
		if (path.substr(start, end == wstring::npos ? wstring::npos : end - start) == segment) {
			return true;
		}
		if (end == wstring::npos) break;
		start = end + 1;
	}
	return false;
}

bool PathRuleSet::Matches(
	const filesystem::path& candidate,
	const filesystem::path& sourceRoot,
	const filesystem::path& originalRoot) const {
	return MatchesSingle(candidate, sourceRoot, originalRoot);
}

bool PathRuleSet::MatchesSingle(
	const filesystem::path& candidate,
	const filesystem::path& sourceRoot,
	const filesystem::path& originalRoot) const {
	const wstring absolute = Normalize(candidate.wstring());
	const wstring fileName = Normalize(candidate.filename().wstring());

	error_code ec;
	const filesystem::path relative = filesystem::relative(candidate, sourceRoot, ec);
	const wstring relativeNormalized = ec ? L"" : Normalize(relative.wstring());

	for (const auto& expression : regularExpressions_) {
		if (regex_search(candidate.wstring(), expression)
			|| (!ec && !relative.empty() && regex_search(relative.wstring(), expression))) {
			return true;
		}
	}

	for (const auto& rule : literalRules_) {
		if (fileName == rule.normalized
			|| PathEqualsOrUnder(relativeNormalized, rule.normalized)
			|| PathHasSegment(relativeNormalized, rule.normalized)) {
			return true;
		}
		if (!rule.absolute) continue;

		if (PathEqualsOrUnder(absolute, rule.normalized)) return true;
		if (originalRoot.empty()) continue;

		error_code relativeError;
		const auto relativeRule = filesystem::relative(rule.originalPath, originalRoot, relativeError);
		if (relativeError || relativeRule.empty() || relativeRule.is_absolute()) continue;

		const filesystem::path remapped = sourceRoot / relativeRule;
		if (PathEqualsOrUnder(absolute, Normalize(remapped.wstring()))) return true;
	}
	return false;
}

bool PathRuleSet::MatchesSelfOrAncestor(
	const filesystem::path& candidate,
	const filesystem::path& root) const {
	if (MatchesSingle(candidate, root, root)) return true;

	error_code ec;
	const filesystem::path absoluteRoot = filesystem::absolute(root, ec).lexically_normal();
	if (ec) return false;

	filesystem::path current = candidate;
	if (!filesystem::is_directory(candidate, ec)) current = candidate.parent_path();
	while (!current.empty()) {
		const filesystem::path absoluteCurrent = filesystem::absolute(current, ec).lexically_normal();
		if (ec || absoluteCurrent == absoluteRoot) break;
		if (MatchesSingle(absoluteCurrent, root, root)) return true;
		current = absoluteCurrent.parent_path();
	}
	return false;
}
