#pragma once

#include <filesystem>
#include <functional>
#include <stop_token>
#include <system_error>
#include <string>
#include <vector>

namespace RestoreWorkspace {

enum class Mode {
	Clean,
	Overlay
};

enum class Phase { Empty, Prepared, Committed, RolledBack };

struct State {
	std::filesystem::path target;
	std::filesystem::path snapshot;
	bool targetOriginallyExisted = false;
	bool snapshotIsCopy = false;
	Phase phase = Phase::Empty;
	bool CanRollback() const { return phase == Phase::Prepared; }
};

bool ValidateSafeTree(const std::filesystem::path& root, std::string& error, std::stop_token token = {});

bool CopyLegacyPreservedToStaging(
	const std::filesystem::path& source, const std::filesystem::path& staging,
	const std::vector<std::wstring>& rules, std::string& error, std::stop_token token = {});

bool Prepare(
	const std::filesystem::path& target,
	State& state,
	std::string& errorText,
	Mode mode = Mode::Clean);

enum class CommitStatus { NotCommitted, Committed, CleanupWarning };
struct CommitResult {
	CommitStatus status = CommitStatus::NotCommitted;
	std::filesystem::path retainedSnapshot;
	std::string error;
	bool WasCommitted() const { return status != CommitStatus::NotCommitted; }
};
struct CommitOptions {
	// Optional deterministic cleanup fault injection; empty in production.
	std::function<void(const std::filesystem::path&, std::error_code&)> removeSnapshot;
	std::stop_token stopToken;
};

CommitResult Commit(
	State& state,
	const std::vector<std::wstring>& preserve,
	std::string& errorText,
	const CommitOptions& options = {});

bool Rollback(State& state, std::string& errorText);

} // namespace RestoreWorkspace
