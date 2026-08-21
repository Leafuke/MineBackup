#include "BatchReadinessService.h"

#include "FolderRewindFormat.h"
#include "Logging.h"
#include "PathIdentity.h"
#include "WorldIdentity.h"
#include "text_to_text.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

using namespace std;

namespace {

void AddIssue(
	ReadinessReport& report,
	string code,
	ReadinessSeverity severity,
	filesystem::path path = {},
	wstring detail = {}) {
	report.issues.push_back({
		std::move(code), severity, std::move(path), std::move(detail)});
}

bool HasBlocking(const ReadinessReport& report) {
	return any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
		return issue.severity == ReadinessSeverity::Blocking;
	});
}

bool IsSafeRelativeWorldPath(const filesystem::path& path) {
	if (path.empty() || path.is_absolute()) return false;
	for (const auto& component : path) {
		if (component == L".." || component == L".") return false;
	}
	return true;
}

wstring ErrorDetail(error_code error) {
	return error ? utf8_to_wstring(error.message()) : wstring{};
}

class BackupProbeCleanup {
public:
	filesystem::path probeFile;
	vector<filesystem::path> createdDirectories;

	~BackupProbeCleanup() {
		error_code ignored;
		if (!probeFile.empty()) filesystem::remove(probeFile, ignored);
		// 从目标向上逐级移除；remove 只会删除空目录，绝不触碰预先存在目录。
		for (const auto& directory : createdDirectories) {
			ignored.clear();
			filesystem::remove(directory, ignored);
		}
	}
};

BackupWriteProbeResult ProbeBackupDirectory(
	const filesystem::path& requested,
	stop_token stopToken) {
	BackupWriteProbeResult result;
	if (stopToken.stop_requested()) {
		result.cancelled = true;
		return result;
	}
	BackupProbeCleanup cleanup;
	error_code error;
	filesystem::path cursor = requested;
	while (!cursor.empty() && !filesystem::exists(cursor, error)) {
		if (error) {
			result.detail = ErrorDetail(error);
			return result;
		}
		cleanup.createdDirectories.push_back(cursor);
		const auto parent = cursor.parent_path();
		if (parent == cursor) break;
		cursor = parent;
	}
	if (error) {
		result.detail = ErrorDetail(error);
		return result;
	}
	if (filesystem::exists(requested, error)
		&& (!filesystem::is_directory(requested, error) || error)) {
		result.detail = error ? ErrorDetail(error) : L"The backup target is not a directory.";
		return result;
	}
	filesystem::create_directories(requested, error);
	if (error || !filesystem::is_directory(requested, error)) {
		result.detail = error ? ErrorDetail(error) : L"The backup directory could not be created.";
		return result;
	}
	if (stopToken.stop_requested()) {
		result.cancelled = true;
		return result;
	}

	cleanup.probeFile = requested
		/ (L".minebackup-readiness-" + FolderRewindFormat::GenerateGuidString());
	{
		ofstream output(cleanup.probeFile, ios::binary | ios::trunc);
		if (!output) {
			result.detail = L"The backup directory rejected a write probe.";
			return result;
		}
		output << "MineBackup readiness probe";
		output.flush();
		if (!output.good()) {
			result.detail = L"The backup write probe could not be flushed.";
			return result;
		}
		output.close();
		if (!output.good()) {
			result.detail = L"The backup write probe could not be closed safely.";
			return result;
		}
	}
	filesystem::remove(cleanup.probeFile, error);
	if (error) {
		result.detail = L"The backup write probe could not be removed: " + ErrorDetail(error);
		return result;
	}
	cleanup.probeFile.clear();
	result.success = true;
	return result;
}

} // namespace

BatchReadinessService::BatchReadinessService(
	AppPaths appPaths,
	BatchReadinessDependencies dependencies)
	: appPaths_(std::move(appPaths)), dependencies_(std::move(dependencies)) {
	if (!dependencies_.resolveSevenZip) {
		dependencies_.resolveSevenZip = [paths = appPaths_](stop_token stopToken) {
			return ExternalToolManager::ResolveSevenZip({}, paths, stopToken);
		};
	}
	if (!dependencies_.probeBackupDirectory) {
		dependencies_.probeBackupDirectory = ProbeBackupDirectory;
	}
}

BatchReadinessResult BatchReadinessService::CheckBatch(
	const vector<ConfigDraft>& drafts,
	const map<int, Config>& existingConfigs,
	stop_token stopToken) const {
	BatchReadinessResult result;
	if (stopToken.stop_requested()) {
		AddIssue(result.report, "readiness_cancelled", ReadinessSeverity::Info);
		return result;
	}
	if (drafts.empty()) {
		AddIssue(result.report, "readiness_empty_batch", ReadinessSeverity::Blocking);
		return result;
	}

	ExternalToolResolution tool;
	try {
		tool = dependencies_.resolveSevenZip(stopToken);
	}
	catch (...) {
		tool.diagnostic = L"7-Zip resolution failed unexpectedly.";
	}
	if (stopToken.stop_requested()) {
		AddIssue(result.report, "readiness_cancelled", ReadinessSeverity::Info);
		return result;
	}
	if (!tool.available || tool.executable.empty()) {
		AddIssue(result.report, "seven_zip_unavailable", ReadinessSeverity::Blocking,
			{}, tool.diagnostic);
	}
	else {
		result.resolvedSevenZip = tool.executable;
	}

	set<wstring> existingSaveRoots;
	set<wstring> existingBackupPaths;
	for (const auto& [index, config] : existingConfigs) {
		(void)index;
		if (!config.saveRoot.empty()) {
			existingSaveRoots.insert(PathIdentity::BuildPathIdentityKey(config.saveRoot));
		}
		if (!config.backupPath.empty()) {
			existingBackupPaths.insert(PathIdentity::BuildPathIdentityKey(config.backupPath));
		}
	}
	set<wstring> batchSaveRoots;
	set<wstring> batchBackupPaths;
	map<int, Config> combinedConfigs = existingConfigs;
	set<wstring> newConfigIds;
	int syntheticIndex = -1;

	for (size_t draftIndex = 0; draftIndex < drafts.size(); ++draftIndex) {
		if (stopToken.stop_requested()) {
			AddIssue(result.report, "readiness_cancelled", ReadinessSeverity::Info);
			return result;
		}
		const ConfigDraft& draft = drafts[draftIndex];
		const filesystem::path saveRoot =
			PathIdentity::NormalizeExistingOrProspectivePath(draft.saveRoot);
		error_code error;
		if (draft.saveRoot.empty() || !filesystem::exists(saveRoot, error)) {
			AddIssue(result.report, "source_missing", ReadinessSeverity::Blocking,
				draft.saveRoot, ErrorDetail(error));
		}
		else if (!filesystem::is_directory(saveRoot, error) || error) {
			AddIssue(result.report, "source_not_directory", ReadinessSeverity::Blocking,
				draft.saveRoot, ErrorDetail(error));
		}

		const wstring saveKey = PathIdentity::BuildPathIdentityKey(saveRoot);
		if (existingSaveRoots.contains(saveKey)) {
			AddIssue(result.report, "source_already_configured", ReadinessSeverity::Blocking,
				draft.saveRoot);
		}
		if (!batchSaveRoots.insert(saveKey).second) {
			AddIssue(result.report, "source_duplicate_in_batch", ReadinessSeverity::Blocking,
				draft.saveRoot);
		}

		if (draft.worlds.empty()) {
			AddIssue(result.report, "source_no_worlds", ReadinessSeverity::Blocking,
				draft.saveRoot);
		}
		vector<filesystem::path> absoluteWorlds;
		for (const auto& [relativeValue, description] : draft.worlds) {
			(void)description;
			const filesystem::path relative(relativeValue);
			if (!IsSafeRelativeWorldPath(relative)) {
				AddIssue(result.report, "world_relative_path_unsafe", ReadinessSeverity::Blocking,
					relative);
				continue;
			}
			const auto world = PathIdentity::NormalizeExistingOrProspectivePath(saveRoot / relative);
			absoluteWorlds.push_back(world);
			if (!PathIdentity::IsEqualOrDescendant(world, saveRoot)) {
				AddIssue(result.report, "world_relative_path_unsafe", ReadinessSeverity::Blocking,
					world);
				continue;
			}
			error.clear();
			if (!filesystem::is_directory(world, error) || error) {
				AddIssue(result.report, "world_missing", ReadinessSeverity::Blocking,
					world, ErrorDetail(error));
				continue;
			}
			if (!filesystem::is_regular_file(world / L"level.dat", error) || error) {
				AddIssue(result.report,
					draft.edition == MinecraftEdition::Bedrock
						? "bedrock_world_invalid" : "java_world_invalid",
					ReadinessSeverity::Blocking, world, ErrorDetail(error));
			}
		}

		const filesystem::path backupPath =
			PathIdentity::NormalizeExistingOrProspectivePath(draft.backupPath);
		if (draft.backupPath.empty() || !draft.backupPath.is_absolute()) {
			AddIssue(result.report, "backup_path_not_absolute", ReadinessSeverity::Blocking,
				draft.backupPath);
		}
		else {
			bool backupPathSafe = true;
			const wstring backupKey = PathIdentity::BuildPathIdentityKey(backupPath);
			if (existingBackupPaths.contains(backupKey)) {
				AddIssue(result.report, "backup_path_existing_collision",
					ReadinessSeverity::Blocking, backupPath);
				backupPathSafe = false;
			}
			if (!batchBackupPaths.insert(backupKey).second) {
				AddIssue(result.report, "backup_path_batch_collision",
					ReadinessSeverity::Blocking, backupPath);
				backupPathSafe = false;
			}
			if (PathIdentity::IsEqualOrDescendant(backupPath, saveRoot)) {
				AddIssue(result.report, "backup_inside_source", ReadinessSeverity::Blocking,
					backupPath);
				backupPathSafe = false;
			}
			for (const auto& world : absoluteWorlds) {
				if (PathIdentity::IsEqualOrDescendant(backupPath, world)) {
					AddIssue(result.report, "backup_inside_world", ReadinessSeverity::Blocking,
						backupPath);
					backupPathSafe = false;
				}
			}

			// 只对当前 Draft 已确认安全的目标执行写探针；其他 Draft 的错误不应遮蔽诊断。
			if (backupPathSafe) {
				BackupWriteProbeResult probe;
				try {
					probe = dependencies_.probeBackupDirectory(backupPath, stopToken);
				}
				catch (...) {
					probe.detail = L"The backup write probe failed unexpectedly.";
				}
				if (probe.cancelled || stopToken.stop_requested()) {
					AddIssue(result.report, "readiness_cancelled", ReadinessSeverity::Info);
					return result;
				}
				if (!probe.success) {
					AddIssue(result.report, "backup_write_probe_failed",
						ReadinessSeverity::Blocking, backupPath, probe.detail);
				}
			}
		}

		Config config = BuildRecommendedConfig(draft, {result.resolvedSevenZip});
		config.configId = L"readiness-" + to_wstring(draftIndex);
		newConfigIds.insert(config.configId);
		while (combinedConfigs.contains(syntheticIndex)) --syntheticIndex;
		combinedConfigs.emplace(syntheticIndex--, std::move(config));
	}

	for (const auto& conflict : WorldIdentity::FindStorageConflicts(combinedConfigs)) {
		if (newConfigIds.contains(conflict.leftConfigId)
			|| newConfigIds.contains(conflict.rightConfigId)) {
			AddIssue(result.report, "storage_identity_collision", ReadinessSeverity::Blocking,
				conflict.backupRoot);
		}
	}

	result.report.ready = !stopToken.stop_requested() && !HasBlocking(result.report);
	const size_t blocking = static_cast<size_t>(count_if(
		result.report.issues.begin(), result.report.issues.end(), [](const auto& issue) {
			return issue.severity == ReadinessSeverity::Blocking;
		}));
	MB_LOG_INFO(minebackup::logging::LogCategory::Validation,
		"minecraft.readiness.completed", "drafts={} blocking_issues={}",
		drafts.size(), blocking);
	return result;
}
