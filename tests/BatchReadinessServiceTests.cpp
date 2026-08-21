#include "BatchReadinessService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
	if (condition) return;
	++failures;
	std::cerr << "FAIL: " << message << '\n';
}

void Touch(const std::filesystem::path& path) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << "level";
}

ConfigDraft ValidDraft(
	const std::filesystem::path& root,
	std::string name = "Minecraft") {
	ConfigDraft draft;
	draft.name = std::move(name);
	draft.edition = MinecraftEdition::Java;
	draft.saveRoot = root / "saves";
	draft.worlds = {{L"World", L"World"}};
	draft.backupPath = root / "backups" / draft.name;
	Touch(draft.saveRoot / "World" / "level.dat");
	return draft;
}

bool HasIssue(const BatchReadinessResult& result, const std::string& code) {
	return std::any_of(result.report.issues.begin(), result.report.issues.end(),
		[&](const auto& issue) { return issue.code == code; });
}

BatchReadinessDependencies ReadyTool(int& resolveCount) {
	BatchReadinessDependencies dependencies;
	dependencies.resolveSevenZip = [&resolveCount](std::stop_token) {
		++resolveCount;
		ExternalToolResolution resolution;
		resolution.available = true;
		resolution.executable = std::filesystem::temp_directory_path() / "fake-7zip";
		return resolution;
	};
	return dependencies;
}

} // namespace

int main() {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path())
		/ ("MineBackupReadiness-" + std::to_string(stamp));
	std::filesystem::create_directories(root);
	AppPaths paths;
	paths.dataRoot = root / "data";

	int resolves = 0;
	BatchReadinessService service(paths, ReadyTool(resolves));
	auto valid = ValidDraft(root / "valid");
	const auto validResult = service.CheckBatch({valid}, {});
	Expect(validResult.report.ready && resolves == 1
			&& !validResult.resolvedSevenZip.empty(),
		"a valid batch should resolve 7-Zip exactly once and become ready");
	Expect(!std::filesystem::exists(valid.backupPath),
		"a successful write probe should remove its file and newly-created empty directories");

	resolves = 0;
	auto missing = ValidDraft(root / "missing-source");
	std::filesystem::remove_all(missing.saveRoot);
	const auto missingResult = service.CheckBatch({missing}, {});
	Expect(!missingResult.report.ready && HasIssue(missingResult, "source_missing")
			&& resolves == 1,
		"a missing source should block without repeating tool resolution");

	auto noWorlds = ValidDraft(root / "no-worlds");
	noWorlds.worlds.clear();
	const auto noWorldsResult = service.CheckBatch({noWorlds}, {});
	Expect(HasIssue(noWorldsResult, "source_no_worlds"),
		"a draft without Inspector worlds should be blocking");

	auto unsafe = ValidDraft(root / "unsafe-relative");
	unsafe.worlds = {{L"../outside", L"Outside"}};
	const auto unsafeResult = service.CheckBatch({unsafe}, {});
	Expect(HasIssue(unsafeResult, "world_relative_path_unsafe"),
		"parent traversal in a world relative path should be blocking");

	auto inside = ValidDraft(root / "inside-source");
	inside.backupPath = inside.saveRoot / "World" / "backups";
	const auto insideResult = service.CheckBatch({inside}, {});
	Expect(HasIssue(insideResult, "backup_inside_source")
			&& HasIssue(insideResult, "backup_inside_world"),
		"a backup target equal to or below source/world should be blocking");

	const auto linkedRoot = root / "linked-source";
	auto linked = ValidDraft(linkedRoot);
	const auto linkedTarget = linked.saveRoot / "linked-backups";
	std::error_code linkError;
	std::filesystem::create_directories(linkedTarget, linkError);
	linkError.clear();
	std::filesystem::create_directory_symlink(linkedTarget, root / "backup-link", linkError);
	if (!linkError) {
		linked.backupPath = root / "backup-link";
		const auto linkedResult = service.CheckBatch({linked}, {});
		Expect(HasIssue(linkedResult, "backup_inside_source"),
			"symlink resolution should detect a backup target inside the source");
	}

	auto duplicateA = ValidDraft(root / "duplicate", "A");
	auto duplicateB = duplicateA;
	duplicateB.name = "B";
	duplicateB.backupPath = root / "duplicate-backups" / "B";
	const auto duplicateResult = service.CheckBatch({duplicateA, duplicateB}, {});
	Expect(HasIssue(duplicateResult, "source_duplicate_in_batch"),
		"two drafts managing the same canonical saves root should be blocking");

	auto backupCollisionA = ValidDraft(root / "backup-collision-a", "A");
	auto backupCollisionB = ValidDraft(root / "backup-collision-b", "B");
	backupCollisionB.backupPath = backupCollisionA.backupPath;
	const auto backupCollisionResult = service.CheckBatch(
		{backupCollisionA, backupCollisionB}, {});
	Expect(HasIssue(backupCollisionResult, "backup_path_batch_collision"),
		"two drafts using the same canonical backup root should be blocking");

	Config existing;
	existing.saveRoot = duplicateA.saveRoot.wstring();
	existing.backupPath = duplicateA.backupPath.wstring();
	const auto existingResult = service.CheckBatch({duplicateA}, {{2, existing}});
	Expect(HasIssue(existingResult, "source_already_configured")
			&& HasIssue(existingResult, "backup_path_existing_collision"),
		"existing source and backup identities should block duplicate configuration");

	auto collision = ValidDraft(root / "storage-collision");
#ifdef _WIN32
	// Windows 路径身份忽略大小写，同一路径的两种拼写不得映射到同一存储目录。
	collision.worlds = {{L"World", L"One"}, {L"world", L"Two"}};
#else
	// 非 Windows 可创建这些名称，清理后的存储目录仍会发生碰撞。
	collision.worlds = {{L"World?", L"One"}, {L"World*", L"Two"}};
	Touch(collision.saveRoot / L"World?" / "level.dat");
	Touch(collision.saveRoot / L"World*" / "level.dat");
#endif
	const auto collisionResult = service.CheckBatch({collision}, {});
	Expect(HasIssue(collisionResult, "storage_identity_collision"),
		"sanitized world names sharing a storage folder should be blocking");

	resolves = 0;
	auto probeDependencies = ReadyTool(resolves);
	probeDependencies.probeBackupDirectory = [](const std::filesystem::path&, std::stop_token) {
		return BackupWriteProbeResult{false, false, L"injected denial"};
	};
	const auto deniedResult = BatchReadinessService(paths, probeDependencies)
		.CheckBatch({ValidDraft(root / "denied")}, {});
	Expect(HasIssue(deniedResult, "backup_write_probe_failed"),
		"an unwritable destination should be blocking");

	BatchReadinessDependencies missingTool;
	int missingToolResolves = 0;
	missingTool.resolveSevenZip = [&](std::stop_token) {
		++missingToolResolves;
		ExternalToolResolution resolution;
		resolution.diagnostic = L"missing";
		return resolution;
	};
	const auto toolResult = BatchReadinessService(paths, missingTool)
		.CheckBatch({ValidDraft(root / "missing-tool")}, {});
	Expect(HasIssue(toolResult, "seven_zip_unavailable") && missingToolResolves == 1,
		"missing 7-Zip should block after one batch resolution");

	std::stop_source stopped;
	stopped.request_stop();
	resolves = 0;
	const auto cancelled = service.CheckBatch({valid}, {}, stopped.get_token());
	Expect(!cancelled.report.ready && HasIssue(cancelled, "readiness_cancelled")
			&& resolves == 0,
		"cancellation should be reported separately without resolving tools");

	const auto emptyResult = service.CheckBatch({}, {});
	Expect(HasIssue(emptyResult, "readiness_empty_batch"),
		"an empty selection should never pass final readiness");

	std::error_code error;
	std::filesystem::remove_all(root, error);
	if (failures != 0) return 1;
	std::cout << "All batch readiness tests passed\n";
	return 0;
}
