#include "ProfileConfigCatalogTests.h"

#include "ProfileConfigCatalog.h"
#include "ProfileConfigRepository.h"
#include "WorldIdentity.h"

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

	const auto loaded = ProfileConfigCatalogLoader::Load(config);
	test.Expect(loaded.status == ProfileCatalogStatus::Loaded
			&& loaded.catalog.FindConfig(L"config-id") != nullptr,
		"Runtime profile catalog should load stable ConfigId values");
	test.Expect(loaded.catalog.configs.at(1).worlds.size() == 1
			&& loaded.catalog.configs.at(1).worlds.front().first == L"world/subdirectory",
		"Runtime profile catalog should preserve normalized relative world paths");

	Config identityConfig = loaded.catalog.configs.at(1);
	identityConfig.saveRoot = (root / "server").wstring();
	identityConfig.backupPath = (root / "backups").wstring();
	identityConfig.worlds = {{L"world/nether", L"Nether"}};
	HistoryEntry legacyEntry;
	legacyEntry.configId = identityConfig.configId;
	legacyEntry.worldName = L"world_nether";
	legacyEntry.worldPath = (filesystem::path(identityConfig.saveRoot) / L"world/nether").wstring();
	legacyEntry.backupFile = L"[Full]-world.7z";
	test.Expect(WorldIdentity::Matches(
			identityConfig, L"world/nether", legacyEntry, legacyEntry.backupFile),
		"World identity matching should map nested configuration paths to flattened storage names");

	string collisionProfile = StableProfile();
	collisionProfile +=
		"[Config2]\n"
		"ConfigName=Collision\n"
		"ConfigId=config-id-two\n"
		"SavePath=/srv/minecraft\n"
		"WorldData=\n"
		"world_subdirectory\n"
		"Flattened world\n"
		"*\n"
		"BackupPath=/srv/backups\n";
	WriteText(config, collisionProfile);
	const auto collision = ProfileConfigCatalogLoader::Load(config);
	test.Expect(collision.status == ProfileCatalogStatus::Invalid
			&& any_of(collision.diagnostics.begin(), collision.diagnostics.end(), [](const auto& diagnostic) {
				return diagnostic.eventId == "config.storage.collision";
			}),
		"Legacy config.ini loading should reject storage-key collisions across Config sections");
	test.Expect(!filesystem::exists(tasks),
		"Read-only catalog loading should ignore legacy special sections without writing JSON");

	WriteText(config, StableProfile(false));
	const auto legacy = ProfileConfigCatalogLoader::Load(config);
	test.Expect(legacy.status == ProfileCatalogStatus::MigrationRequired,
		"Profiles without stable identities should require GUI compatibility migration");

	string invalidProfile = StableProfile();
	invalidProfile.replace(invalidProfile.find("ZipLevel=5"), 10, "ZipLevel=not-a-number");
	WriteText(config, invalidProfile);
	const auto invalid = ProfileConfigCatalogLoader::Load(config);
	test.Expect(invalid.status == ProfileCatalogStatus::Invalid,
		"Invalid operational values should make the runtime profile unusable");

	WriteText(config, StableProfile());
	WriteText(tasks, R"({"schemaVersion":999,"specialConfigs":[]})");
	const auto future = ProfileConfigCatalogLoader::Load(config);
	test.Expect(future.status == ProfileCatalogStatus::Loaded,
		"Legacy special-task files should be retained but ignored by the runtime catalog");

	const filesystem::path repositoryConfig = root / "repository.ini";
	WriteText(repositoryConfig,
		"[General]\n"
		"Language=zh_CN\n"
		"ExtensionGlobal=keep\n"
		"RestoreWhitelistItem=session.lock\n\n"
		"[Config4]\n"
		"ConfigName=Before\n"
		"ConfigId=repository-config\n"
		"SavePath=C:/before\n"
		"WorldData=\nworld\ndescription\n*\n"
		"BackupPath=C:/backup-before\n"
		"ZipLevel=5\n"
		"Theme=7\n"
		"ExtensionConfig=keep\n\n"
		"[SpCfg8]\n"
		"Name=Ignored legacy data\n"
		"SpecialConfigId=legacy-special\n");
	ProfileConfigRepository repository(repositoryConfig);
	auto snapshot = repository.Load();
	test.Expect(snapshot.status == ProfileCatalogStatus::Loaded
			&& snapshot.configs.size() == 1
			&& snapshot.restorePreserve == vector<wstring>{L"session.lock"},
		"ProfileConfigRepository should load server fields and restore preserve rules");
	auto updated = snapshot.configs;
	updated.at(4).name = "After";
	updated.at(4).saveRoot = L"C:/after";
	updated.at(4).backupPath = L"C:/backup-after";
	updated.at(4).worlds = {{L"world", L"changed"}};
	Config added;
	added.configId = L"new-config";
	added.name = "Added";
	added.saveRoot = L"C:/server";
	added.backupPath = L"C:/backups";
	added.worlds = {{L"new-world", L"New world"}};
	updated.emplace(9, added);
	const auto saved = repository.Save(
		updated, {L"session.lock", L"ops-marker.txt"}, false);
	ifstream mergedInput(repositoryConfig, ios::binary);
	const string merged((istreambuf_iterator<char>(mergedInput)), {});
	mergedInput.close();
	test.Expect(saved.success
			&& merged.find("ConfigName=After") != string::npos
			&& merged.find("ExtensionGlobal=keep") != string::npos
			&& merged.find("Theme=7") != string::npos
			&& merged.find("ExtensionConfig=keep") != string::npos
			&& merged.find("[SpCfg8]") != string::npos
			&& merged.find("RestoreWhitelistItem=ops-marker.txt") != string::npos,
		"Repository writes should update managed fields while preserving desktop and unknown fields");

	map<int, Config> onlyAdded{{1, added}};
	const auto pruned = repository.Save(onlyAdded, {L"session.lock"}, true);
	ifstream prunedInput(repositoryConfig, ios::binary);
	const string prunedText((istreambuf_iterator<char>(prunedInput)), {});
	test.Expect(pruned.success,
		"Repository prune should commit atomically");
	test.Expect(prunedText.find("ConfigId=repository-config") == string::npos,
		"Repository prune should remove missing Config sections");
	test.Expect(prunedText.find("ConfigId=new-config") != string::npos,
		"Repository prune should retain Config sections present in the desired snapshot");
	test.Expect(prunedText.find("[SpCfg8]") != string::npos,
		"Repository prune should retain legacy non-Config sections");
}

