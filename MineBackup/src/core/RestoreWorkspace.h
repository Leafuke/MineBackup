#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace RestoreWorkspace {

struct State {
	std::filesystem::path target;
	std::filesystem::path snapshot;
	bool targetOriginallyExisted = false;
	bool prepared = false;
};

bool Prepare(
	const std::filesystem::path& target,
	State& state,
	std::string& errorText);

bool Commit(
	State& state,
	const std::vector<std::wstring>& preserve,
	std::string& errorText);

bool Rollback(State& state, std::string& errorText);

} // namespace RestoreWorkspace
