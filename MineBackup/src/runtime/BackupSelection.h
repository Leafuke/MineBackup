#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

struct BackupRequest;

// A one-shot inclusion boundary. Scope paths are rooted at the source, unlike
// generic whitelist literals, which match whole path segments.
class BackupSelection {
public:
	static bool TryBuild(const BackupRequest& request, BackupSelection& result, std::string& error);
	bool IsPartial() const { return scoped_ || !literals_.empty() || !wildcards_.empty(); }
	bool HasWhitelist() const { return !literals_.empty() || !wildcards_.empty(); }
	bool Includes(const std::wstring& relativePath) const;

private:
	bool scoped_ = false;
	std::set<std::wstring> literals_;
	std::vector<std::wstring> wildcards_;
	std::set<std::wstring> scopeFiles_;
	std::vector<std::wstring> scopePrefixes_;
	std::vector<std::wstring> scopeWildcards_;
};
