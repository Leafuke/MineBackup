#include "RestoreWorkspace.h"

#include "BackupManagerInternal.h"
#include "PathRuleSet.h"

#include <algorithm>
#include <system_error>

using namespace std;

namespace RestoreWorkspace {
namespace {

filesystem::path NextSnapshotPath(const filesystem::path& target) {
	const filesystem::path base = target.parent_path()
		/ (target.filename().wstring() + L"-MineBackup-Restore-Snapshot");
	filesystem::path candidate = base;
	error_code error;
	for (int suffix = 1; filesystem::exists(candidate, error); ++suffix) {
		candidate = filesystem::path(base.wstring() + L"-" + to_wstring(suffix));
	}
	if (error) throw system_error(error, "failed to inspect restore snapshot path");
	return candidate;
}

bool EnsureDirectory(const filesystem::path& path, string& errorText) {
	error_code error;
	if (filesystem::exists(path, error)) {
		if (error) {
			errorText = "failed to inspect preserved directory: " + error.message();
			return false;
		}
		if (!filesystem::is_directory(path, error) || error) {
			errorText = "preserved directory conflicts with a non-directory: "
				+ path.string();
			return false;
		}
		return true;
	}
	filesystem::create_directories(path, error);
	if (error) {
		errorText = "failed to create preserved directory: " + error.message();
		return false;
	}
	return true;
}

bool CopyPreserved(
	const filesystem::path& source,
	const filesystem::path& target,
	vector<wstring> rules,
	string& errorText) {
	// session.lock 由恢复工作区统一保留，避免桌面端与 headless 端出现不同语义。
	if (none_of(rules.begin(), rules.end(), [](const wstring& item) {
		return item == L"session.lock";
	})) {
		rules.push_back(L"session.lock");
	}
	error_code error;
	if (!filesystem::exists(source, error)) {
		if (error) errorText = "failed to inspect restore snapshot: " + error.message();
		return !error;
	}
	const PathRuleSet matcher(rules);
	filesystem::recursive_directory_iterator iterator(source, {}, error);
	if (error) {
		errorText = "failed to enumerate restore snapshot: " + error.message();
		return false;
	}
	for (const filesystem::recursive_directory_iterator end; iterator != end;
		iterator.increment(error)) {
		if (error) {
			errorText = "failed to enumerate preserved entries: " + error.message();
			return false;
		}
		error_code entryError;
		if (!iterator->is_directory(entryError)) {
			if (entryError) {
				errorText = "failed to inspect preserved entry: " + entryError.message();
				return false;
			}
			continue;
		}
		if (!matcher.MatchesSelfOrAncestor(iterator->path(), source)) continue;
		const auto relative = filesystem::relative(iterator->path(), source, error);
		if (error) {
			errorText = "failed to resolve preserved directory: " + error.message();
			return false;
		}
		if (!EnsureDirectory(target / relative, errorText)) return false;
	}

	error.clear();
	iterator = filesystem::recursive_directory_iterator(source, {}, error);
	if (error) {
		errorText = "failed to enumerate restore snapshot: " + error.message();
		return false;
	}
	for (const filesystem::recursive_directory_iterator end; iterator != end;
		iterator.increment(error)) {
		if (error) {
			errorText = "failed to enumerate preserved files: " + error.message();
			return false;
		}
		error_code entryError;
		if (!iterator->is_regular_file(entryError)) {
			if (entryError) {
				errorText = "failed to inspect preserved file: " + entryError.message();
				return false;
			}
			continue;
		}
		if (!matcher.MatchesSelfOrAncestor(iterator->path(), source)) continue;
		const auto relative = filesystem::relative(iterator->path(), source, error);
		if (error) {
			errorText = "failed to resolve preserved file: " + error.message();
			return false;
		}
		const auto destination = target / relative;
		if (!EnsureDirectory(destination.parent_path(), errorText)) return false;
		if (filesystem::exists(destination, error)) {
			if (error) {
				errorText = "failed to inspect preserved destination: " + error.message();
				return false;
			}
			if (filesystem::is_directory(destination, error) || error) {
				errorText = "preserved file conflicts with a directory: "
					+ destination.string();
				return false;
			}
		}
		error.clear();
		filesystem::copy_file(iterator->path(), destination,
			filesystem::copy_options::overwrite_existing, error);
		if (error) {
			errorText = "failed to copy preserved file: " + error.message();
			return false;
		}
	}
	return true;
}

} // namespace

bool Prepare(
	const filesystem::path& target,
	State& state,
	string& errorText,
	Mode mode) {
	state = {};
	state.target = target;
	errorText.clear();
	error_code error;
	// 新建目标没有 snapshot 路径，必须单独记录初始存在状态，不能以 snapshot.empty() 推断目标是否原本存在。
	state.targetOriginallyExisted = filesystem::exists(target, error);
	if (error) {
		errorText = "failed to inspect restore target: " + error.message();
		return false;
	}
	state.prepared = true;
	if (!state.targetOriginallyExisted) {
		filesystem::create_directories(target, error);
		if (error) errorText = "failed to create restore target: " + error.message();
		return !error;
	}

	try {
		state.snapshot = NextSnapshotPath(target);
	}
	catch (const exception& exception) {
		state.prepared = false;
		errorText = exception.what();
		return false;
	}
	if (mode == Mode::Overlay) {
		if (!filesystem::is_directory(target, error) || error) {
			state.prepared = false;
			errorText = error
				? "failed to inspect restore target: " + error.message()
				: "restore target is not a directory";
			return false;
		}
		filesystem::copy(
			target,
			state.snapshot,
			filesystem::copy_options::recursive
				| filesystem::copy_options::overwrite_existing,
			error);
		if (error) {
			error_code cleanupError;
			filesystem::remove_all(state.snapshot, cleanupError);
			state.snapshot.clear();
			state.prepared = false;
			errorText = "failed to snapshot overlay restore target: " + error.message();
			return false;
		}
		state.snapshotIsCopy = true;
		return true;
	}
	filesystem::rename(target, state.snapshot, error);
	if (error) {
		errorText = "failed to move restore target to snapshot: " + error.message();
		state.snapshot.clear();
		return false;
	}
	filesystem::create_directories(target, error);
	if (!error) return true;
	errorText = "failed to create clean restore workspace: " + error.message();
	return false;
}

bool Commit(State& state, const vector<wstring>& preserve, string& errorText) {
	errorText.clear();
	if (!state.prepared) {
		errorText = "restore workspace is not prepared";
		return false;
	}
	try {
		if (state.snapshotIsCopy) {
			if (!state.snapshot.empty()) {
				BackupManagerInternal::ClearReadonlyAttributesRecursively(state.snapshot);
				error_code error;
				filesystem::remove_all(state.snapshot, error);
				if (error) {
					errorText = "failed to remove overlay restore snapshot: " + error.message();
					return false;
				}
			}
			state.prepared = false;
			state.snapshot.clear();
			state.snapshotIsCopy = false;
			return true;
		}
		if (state.targetOriginallyExisted) {
			if (state.snapshot.empty() || !filesystem::exists(state.snapshot)) {
				errorText = "restore snapshot is missing";
				return false;
			}
			if (!CopyPreserved(state.snapshot, state.target, preserve, errorText)) return false;
			BackupManagerInternal::ClearReadonlyAttributesRecursively(state.snapshot);
			error_code error;
			filesystem::remove_all(state.snapshot, error);
			if (error) {
				errorText = "failed to remove restore snapshot: " + error.message();
				return false;
			}
		}
		state.prepared = false;
		state.snapshot.clear();
		state.snapshotIsCopy = false;
		return true;
	}
	catch (const exception& exception) {
		errorText = exception.what();
		return false;
	}
}

bool Rollback(State& state, string& errorText) {
	errorText.clear();
	if (!state.prepared) return true;
	if (!state.targetOriginallyExisted && state.snapshot.empty()) {
		error_code error;
		if (filesystem::exists(state.target, error)) {
			if (error) {
				errorText = "failed to inspect new restore target: " + error.message();
				return false;
			}
			BackupManagerInternal::ClearReadonlyAttributesRecursively(state.target);
			filesystem::remove_all(state.target, error);
			if (error) {
				errorText = "failed to remove new restore target: " + error.message();
				return false;
			}
		}
		state.prepared = false;
		return true;
	}
	if (state.snapshot.empty()) {
		// 原目标尚未移动成功，继续回滚不会再改变用户数据。
		state.prepared = false;
		return true;
	}
	error_code error;
	if (state.snapshotIsCopy) {
		if (filesystem::exists(state.target, error)) {
			if (error) {
				errorText = "failed to inspect partial overlay target: " + error.message();
				return false;
			}
			BackupManagerInternal::ClearReadonlyAttributesRecursively(state.target);
			filesystem::remove_all(state.target, error);
			if (error) {
				errorText = "failed to clean partial overlay target: " + error.message();
				return false;
			}
		}
		filesystem::copy(
			state.snapshot,
			state.target,
			filesystem::copy_options::recursive
				| filesystem::copy_options::overwrite_existing,
			error);
		if (error) {
			errorText = "failed to restore overlay snapshot: " + error.message();
			return false;
		}
		filesystem::remove_all(state.snapshot, error);
		if (error) {
			errorText = "failed to remove restored overlay snapshot: " + error.message();
			return false;
		}
		state.snapshot.clear();
		state.snapshotIsCopy = false;
		state.prepared = false;
		return true;
	}
	if (!filesystem::exists(state.snapshot, error)) {
		if (error) errorText = "failed to inspect restore snapshot: " + error.message();
		else errorText = "restore snapshot is missing";
		return false;
	}
	if (filesystem::exists(state.target, error)) {
		if (error) {
			errorText = "failed to inspect partial restore target: " + error.message();
			return false;
		}
		BackupManagerInternal::ClearReadonlyAttributesRecursively(state.target);
		filesystem::remove_all(state.target, error);
		if (error) {
			errorText = "failed to clean partial restore target: " + error.message();
			return false;
		}
	}
	filesystem::rename(state.snapshot, state.target, error);
	if (error) {
		errorText = "failed to restore snapshot: " + error.message();
		return false;
	}
	state.snapshot.clear();
	state.prepared = false;
	return true;
}

} // namespace RestoreWorkspace
