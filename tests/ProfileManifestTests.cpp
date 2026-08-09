#include "ProfileManifestTests.h"

#include "AppPaths.h"
#include "ProfileConfigRepository.h"
#include "ProfileManifest.h"

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

	const auto exported = ProfileManifest::Export(paths);
	test.Expect(exported.IsLoaded()
			&& exported.manifest.configs.front().name == "Updated server"
			&& ProfileManifest::Parse(ProfileManifest::Serialize(exported.manifest), root).IsLoaded(),
		"Profile export should round-trip the shared Config and Job server fields");
}
