#include "ProfileManifestTests.h"

#include "AppPaths.h"
#include "FolderRewindFormat.h"
#include "FolderRewindHistoryStore.h"
#include "HistoryRepository.h"
#include "ProfileConfigRepository.h"
#include "ProfileManifest.h"
#include "text_to_text.h"

#include <fstream>

using namespace std;

namespace {

void Write(const filesystem::path& path, const string& content) {
	filesystem::create_directories(path.parent_path());
	ofstream(path, ios::binary | ios::trunc) << content;
}

string Read(const filesystem::path& path) {
	ifstream input(path, ios::binary);
	return string((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
}

AppPaths TestPaths(const filesystem::path& root) {
	AppPaths paths;
	paths.configRoot = root / "config";
	paths.dataRoot = root / "data";
	paths.stateRoot = root / "state";
	paths.runtimeRoot = root / "runtime";
	return paths;
}

} // namespace

void RunProfileManifestTests(
	TestContext& test,
	const filesystem::path& temporaryRoot) {
	const filesystem::path root = temporaryRoot / "profile-manifest";
	const filesystem::path manifestPath = root / "declarative" / "manifest.json";
	const auto templateManifest = ProfileManifest::CreateTemplate();
	Write(manifestPath, ProfileManifest::Serialize(templateManifest));
	const auto loaded = ProfileManifest::Load(manifestPath);
	test.Expect(loaded.IsLoaded()
			&& loaded.manifest.configs.size() == 1
			&& loaded.manifest.jobs.jobs.size() == 1
			&& filesystem::path(loaded.manifest.configs.front().saveRoot).is_absolute()
			&& filesystem::path(loaded.manifest.configs.front().saveRoot)
				== (manifestPath.parent_path() / "server").lexically_normal(),
		"Manifest v1 should resolve local paths relative to the manifest directory");

	auto collidingManifest = templateManifest;
	collidingManifest.configs.front().worlds = {{L"world/nether", L"Nether"}};
	Config collidingConfig = collidingManifest.configs.front();
	collidingConfig.configId = FolderRewindFormat::GenerateGuidString();
	collidingConfig.name = "Colliding world";
	collidingConfig.worlds = {{L"world_nether", L"Legacy spelling"}};
	collidingManifest.configs.push_back(std::move(collidingConfig));
	test.Expect(ProfileManifest::Parse(
			ProfileManifest::Serialize(collidingManifest), manifestPath.parent_path()).status
			== ProfileManifestStatus::Invalid,
		"Manifest loading should reject nested and flattened worlds sharing one storage key");

	string unknown = ProfileManifest::Serialize(templateManifest);
	unknown.replace(unknown.find("\"schemaVersion\": 1"),
		string("\"schemaVersion\": 1").size(),
		"\"schemaVersion\": 1, \"schedule\": []");
	test.Expect(ProfileManifest::Parse(unknown, manifestPath.parent_path()).status
			== ProfileManifestStatus::Invalid,
		"Manifest schema should reject embedded Schedule and unknown top-level fields");

	const AppPaths paths = TestPaths(root / "profile");
	auto plan = ProfileManifest::Plan(paths, loaded.manifest, false);
	test.Expect(plan.code == OperationCode::Success && plan.diff.size() == 3,
		"Planning against an empty Profile should add Config and Job plus restore rules");
	const auto applied = ProfileManifest::Apply(paths, plan);
	test.Expect(applied.code == OperationCode::Success
			&& filesystem::exists(paths.ConfigFile())
			&& filesystem::exists(paths.JobsFile()),
		"Manifest apply should atomically establish config.ini and jobs.json");
	const auto repeated = ProfileManifest::Plan(paths, loaded.manifest, false);
	test.Expect(repeated.code == OperationCode::Success && repeated.diff.empty(),
		"Applying the same manifest should produce an idempotent empty diff");
	auto mergedCollisionManifest = loaded.manifest;
	mergedCollisionManifest.configs.front().worlds = {{L"world/nether", L"Nether"}};
	Config existingProfileCollision = mergedCollisionManifest.configs.front();
	existingProfileCollision.configId = FolderRewindFormat::GenerateGuidString();
	existingProfileCollision.name = "Merged collision";
	existingProfileCollision.worlds = {{L"world_nether", L"Flattened"}};
	mergedCollisionManifest.configs.push_back(std::move(existingProfileCollision));
	const auto mergedCollisionPlan = ProfileManifest::Plan(
		paths, mergedCollisionManifest, false);
	test.Expect(mergedCollisionPlan.code != OperationCode::Success
			&& any_of(mergedCollisionPlan.diagnostics.begin(),
				mergedCollisionPlan.diagnostics.end(), [](const auto& diagnostic) {
					return diagnostic.eventId == "manifest.config.storage_collision";
				}),
		"Manifest planning should reject collisions introduced when merging into an existing profile");

	string configText = Read(paths.ConfigFile());
	const auto insertAt = configText.find("\n", configText.find("[Config"));
	configText.insert(insertAt + 1, "Theme=7\nDesktopExtension=preserved\n");
	Write(paths.ConfigFile(), configText);
	auto changedManifest = loaded.manifest;
	changedManifest.configs.front().name = "Updated server";
	auto updatePlan = ProfileManifest::Plan(paths, changedManifest, false);
	test.Expect(updatePlan.code == OperationCode::Success
			&& any_of(updatePlan.diff.begin(), updatePlan.diff.end(), [](const auto& item) {
				return item.kind == "config" && item.action == ProfileDiffAction::Update;
			}),
		"Manifest diff should identify an owned Config field update");
	test.Expect(ProfileManifest::Apply(paths, updatePlan).code == OperationCode::Success
			&& Read(paths.ConfigFile()).find("Theme=7") != string::npos
			&& Read(paths.ConfigFile()).find("DesktopExtension=preserved") != string::npos,
		"Manifest apply should preserve GUI and unknown Config fields");

	auto prunedManifest = changedManifest;
	prunedManifest.configs.clear();
	prunedManifest.jobs.jobs.clear();
	auto prunePlan = ProfileManifest::Plan(paths, prunedManifest, true);
	test.Expect(prunePlan.code == OperationCode::Success
			&& count_if(prunePlan.diff.begin(), prunePlan.diff.end(), [](const auto& item) {
				return item.action == ProfileDiffAction::Remove;
			}) == 2,
		"Explicit prune should report Config and Job removals without deleting data");

	// 构造一个即将被 prune 的 Config，并确认其 history 会先转换为稳定 ConfigId 再保留。
	const auto currentProfile = ProfileConfigRepository(paths.ConfigFile()).Load();
	Config orphanConfig = currentProfile.configs.at(1);
	orphanConfig.configId = L"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
	orphanConfig.name = "Orphan history config";
	orphanConfig.saveRoot = (root / "orphan-server").wstring();
	orphanConfig.backupPath = (root / "orphan-backups").wstring();
	map<int, Config> configsWithOrphan = currentProfile.configs;
	configsWithOrphan.emplace(2, orphanConfig);
	const auto configWithOrphanWrite = ProfileConfigRepository(
		paths.ConfigFile()).Save(configsWithOrphan, currentProfile.restorePreserve, true);
	HistoryEntry orphanEntry;
	orphanEntry.configId = orphanConfig.configId;
	orphanEntry.worldPath = (filesystem::path(orphanConfig.saveRoot) / L"world").wstring();
	orphanEntry.worldName = L"world";
	orphanEntry.backupFile = L"[Full]-Orphan.7z";
	orphanEntry.backupType = L"Full";
	HistoryRepository orphanHistory;
	FolderRewindHistoryStore::HistoryByConfigId orphanItems;
	orphanItems[orphanConfig.configId] = {orphanEntry};
	const bool orphanHistoryWrite = orphanHistory.ReplaceAll(
		std::move(orphanItems), paths.HistoryFile(), configsWithOrphan, true);
	test.Expect(configWithOrphanWrite.success && orphanHistoryWrite,
		"Prune fixture should persist an orphan Config and its history before removal");

	const auto preservePlan = ProfileManifest::Plan(paths, changedManifest, true);
	test.Expect(preservePlan.code == OperationCode::Success
			&& any_of(preservePlan.diff.begin(), preservePlan.diff.end(), [](const auto& item) {
				return item.kind == "config" && item.action == ProfileDiffAction::Remove
					&& item.orphanHistoryCount == 1;
			}),
		"Prune planning should report history owned by the removed Config as orphan data");
	const auto preserveApplied = ProfileManifest::Apply(paths, preservePlan);
	HistoryRepository reloadedHistory;
	const auto orphanReloaded = preserveApplied.code == OperationCode::Success
			&& reloadedHistory.Load(paths.HistoryFile(), preservePlan.configs)
			&& reloadedHistory.EntriesForConfig(orphanConfig.configId)->size() == 1;
	HistoryEntry activeEntry;
	activeEntry.configId = changedManifest.configs.front().configId;
	activeEntry.worldPath = changedManifest.configs.front().saveRoot + L"/world";
	activeEntry.worldName = L"world";
	activeEntry.backupFile = L"[Full]-Active.7z";
	activeEntry.backupType = L"Full";
	const auto mutation = orphanReloaded
		? reloadedHistory.Mutate(
			activeEntry.configId,
			paths.HistoryFile(),
			preservePlan.configs,
			true,
			[&](vector<HistoryEntry>& entries) {
				entries.push_back(activeEntry);
				return true;
			})
		: HistoryMutationResult{};
	HistoryRepository afterMutation;
	test.Expect(orphanReloaded
			&& mutation.changed && mutation.persisted
			&& afterMutation.Load(paths.HistoryFile(), preservePlan.configs)
			&& afterMutation.EntriesForConfig(orphanConfig.configId)->size() == 1
			&& Read(paths.HistoryFile()).find(wstring_to_utf8(orphanConfig.configId)) != string::npos,
		"Prune apply and runtime reload should preserve orphan history by ConfigId");

	const auto profileBeforeLegacyPrune = ProfileConfigRepository(paths.ConfigFile()).Load();
	const auto legacyConfigWrite = ProfileConfigRepository(paths.ConfigFile()).Save(
		configsWithOrphan,
		profileBeforeLegacyPrune.restorePreserve,
		true);
	Write(paths.HistoryFile(),
		R"([{"configIndex":2,"timestamp":"2026-08-14T00:00:00",)"
		R"("worldPath":"orphan/world","worldName":"world",)"
		R"("backupFile":"[Full]-Legacy.7z","backupType":"Full"}])");
	const auto legacyPrunePlan = ProfileManifest::Plan(paths, changedManifest, true);
	const auto legacyPruneApplied = ProfileManifest::Apply(paths, legacyPrunePlan);
	HistoryRepository legacyReloaded;
	test.Expect(legacyConfigWrite.success
			&& legacyPruneApplied.code == OperationCode::Success
			&& legacyReloaded.Load(paths.HistoryFile(), legacyPrunePlan.configs)
			&& legacyReloaded.EntriesForConfig(orphanConfig.configId)->size() == 1
			&& Read(paths.HistoryFile()).find("\"ConfigId\": \""
				+ wstring_to_utf8(orphanConfig.configId) + "\"") != string::npos,
		"Prune apply should migrate legacy config-index history before removing its Config");

	const auto exported = ProfileManifest::Export(paths);
	test.Expect(exported.IsLoaded()
			&& exported.manifest.configs.front().name == "Updated server"
			&& ProfileManifest::Parse(ProfileManifest::Serialize(exported.manifest), root).IsLoaded(),
		"Profile export should round-trip the shared Config and Job server fields");
}
