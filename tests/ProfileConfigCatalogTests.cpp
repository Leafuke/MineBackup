#include "ProfileConfigCatalogTests.h"

#include "ProfileConfigCatalog.h"

#include <fstream>

using namespace std;

namespace {

void WriteText(const filesystem::path& path, const string& content) {
	filesystem::create_directories(path.parent_path());
	ofstream(path, ios::binary | ios::trunc) << content;
}

string StableProfile(bool includeIds = true) {
	return
		"[Config1]\n"
		"ConfigName=Server\n" +
		string(includeIds ? "ConfigId=config-id\n" : "") +
		"SavePath=/srv/minecraft\n"
		"WorldData=\n"
		"world/subdirectory\n"
		"Primary world\n"
		"*\n"
		"BackupPath=/srv/backups\n"
		"ZipProgram=/usr/bin/7zz\n"
		"ZipLevel=5\n"
		"SmartBackup=2\n"
		"UseLowPriority=1\n"
		"SkipIfUnchanged=1\n"
		"CloudSyncEnabled=0\n"
		"[SpCfg2]\n"
		"Name=Nightly\n" +
		string(includeIds ? "SpecialConfigId=special-id\n" : "") +
		"Command=echo legacy\n";
}

} // namespace

void RunProfileConfigCatalogTests(
	TestContext& test,
	const filesystem::path& temporaryRoot) {
	const filesystem::path root = temporaryRoot / "profile-catalog";
	const filesystem::path config = root / "config.ini";
	const filesystem::path tasks = root / "special-tasks.json";
	WriteText(config, StableProfile());

	const auto loaded = ProfileConfigCatalogLoader::Load(config, tasks);
	test.Expect(loaded.status == ProfileCatalogStatus::Loaded
			&& loaded.catalog.FindConfig(L"config-id") != nullptr
			&& loaded.catalog.FindSpecialConfig(L"special-id") != nullptr,
		"Runtime profile catalog should load stable ConfigId and SpecialConfigId values");
	test.Expect(loaded.catalog.configs.at(1).worlds.size() == 1
			&& loaded.catalog.configs.at(1).worlds.front().first == L"world/subdirectory",
		"Runtime profile catalog should preserve normalized relative world paths");
	test.Expect(loaded.catalog.specialTaskMigrationPending
			&& !filesystem::exists(tasks),
		"Read-only catalog loading should report legacy task migration without writing JSON");

	WriteText(config, StableProfile(false));
	const auto legacy = ProfileConfigCatalogLoader::Load(config, tasks);
	test.Expect(legacy.status == ProfileCatalogStatus::MigrationRequired,
		"Profiles without stable identities should require GUI compatibility migration");

	WriteText(config, StableProfile() + "ZipLevel=not-a-number\n");
	const auto invalid = ProfileConfigCatalogLoader::Load(config, tasks);
	test.Expect(invalid.status == ProfileCatalogStatus::Invalid,
		"Invalid operational values should make the runtime profile unusable");

	WriteText(config, StableProfile());
	WriteText(tasks, R"({"schemaVersion":999,"specialConfigs":[]})");
	const auto future = ProfileConfigCatalogLoader::Load(config, tasks);
	test.Expect(future.status == ProfileCatalogStatus::Invalid
			&& !future.diagnostics.empty(),
		"Future special-task schemas should be rejected without legacy fallback");
}

