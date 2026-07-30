#pragma once

#include <filesystem>
#include <regex>
#include <string>
#include <vector>

class PathRuleSet {
public:
	explicit PathRuleSet(const std::vector<std::wstring>& rules);

	bool Matches(
		const std::filesystem::path& candidate,
		const std::filesystem::path& sourceRoot,
		const std::filesystem::path& originalRoot = {}) const;

	bool MatchesSelfOrAncestor(
		const std::filesystem::path& candidate,
		const std::filesystem::path& root) const;

private:
	struct LiteralRule {
		std::wstring normalized;
		std::filesystem::path originalPath;
		bool absolute = false;
	};

	static std::wstring Normalize(std::wstring value);
	static bool PathEqualsOrUnder(const std::wstring& path, const std::wstring& rule);
	static bool PathHasSegment(const std::wstring& path, const std::wstring& segment);

	bool MatchesSingle(
		const std::filesystem::path& candidate,
		const std::filesystem::path& sourceRoot,
		const std::filesystem::path& originalRoot) const;

	std::vector<LiteralRule> literalRules_;
	std::vector<std::wregex> regularExpressions_;
};
