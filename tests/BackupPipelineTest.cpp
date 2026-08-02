#include "BackupPipelineTest.h"

#include "ArchiveRunner.h"
#include "BackupChangeDetector.h"
#include "FolderRewindMetadataStore.h"
#include "PathRuleSet.h"

#include <fstream>

using namespace std;

namespace {

void WriteFile(const filesystem::path& path, const string& content) {
	filesystem::create_directories(path.parent_path());
	ofstream(path, ios::binary | ios::trunc) << content;
}

void TestPathRules(TestContext& test, const filesystem::path& root) {
	const filesystem::path source = root / "rules" / "snapshot";
	const filesystem::path original = root / "rules" / "world";
	const filesystem::path sessionLock = source / "region" / "session.lock";
	const filesystem::path cacheFile = source / "cache" / "nested" / "item.bin";
	const filesystem::path mappedAbsolute = original / "playerdata";

	PathRuleSet rules({
		L"session.lock",
		L"cache",
		mappedAbsolute.wstring(),
		L"regex:(^|[\\\\/])poi[\\\\/].*\\.mca$"});

	test.Expect(rules.Matches(sessionLock, source, original),
		"file-name path rule should match a nested file");
	test.Expect(rules.Matches(cacheFile, source, original),
		"path-segment rule should match a nested directory");
	test.Expect(rules.Matches(source / "playerdata" / "player.dat", source, original),
		"absolute rule should be remapped from the original root to a snapshot root");
	test.Expect(rules.Matches(source / "poi" / "r.0.0.mca", source, original),
		"regular-expression rule should match a relative path");
	test.Expect(!rules.Matches(source / "cache-old" / "item.bin", source, original),
		"path-segment rules must not match a longer segment");

	PathRuleSet whitelist({L"datapacks/keep"});
	test.Expect(whitelist.MatchesSelfOrAncestor(
		source / "datapacks" / "keep" / "pack.mcmeta", source),
		"restore whitelist should preserve descendants of a listed path");
}

void TestChangeDetector(TestContext& test, const filesystem::path& root) {
	const filesystem::path source = root / "scan" / "world";
	const filesystem::path metadata = root / "scan" / "metadata";
	const filesystem::path backups = root / "scan" / "backups";
	filesystem::create_directories(backups);
	WriteFile(source / "level.dat", "first");
	WriteFile(source / "region" / "r.0.0.mca", "region");

	BackupChangeDetector detector;
	const auto initial = detector.Scan(source, metadata, backups);
	test.Expect(initial.status == BackupScanStatus::MetadataInvalid,
		"missing metadata should request a full backup");
	test.Expect(initial.currentState.size() == 2,
		"full-backup fallback should still capture the current state");

	const wstring baseName = L"[Full]-World.7z";
	WriteFile(backups / baseName, "archive");
	FolderRewindFormat::MetadataState state;
	state.lastBackupFileName = baseName;
	state.basedOnFullBackup = baseName;
	state.fileStates = initial.currentState;
	test.Expect(FolderRewindMetadataStore::SaveState(metadata, state),
		"change detector fixture metadata should be persisted");

	const auto unchanged = detector.Scan(source, metadata, backups);
	test.Expect(unchanged.status == BackupScanStatus::NoChange,
		"identical file state should not create a smart backup");

	filesystem::remove(source / "region" / "r.0.0.mca");
	WriteFile(source / "level.dat", "modified-content");
	WriteFile(source / "data" / "new.dat", "new");
	const auto changed = detector.Scan(source, metadata, backups);
	test.Expect(changed.status == BackupScanStatus::ChangesDetected,
		"added, modified and deleted files should be detected");
	test.Expect(changed.changes.addedFiles == vector<wstring>{L"data/new.dat"},
		"added paths should be normalized and sorted");
	test.Expect(changed.changes.modifiedFiles == vector<wstring>{L"level.dat"},
		"modified paths should be reported");
	test.Expect(changed.changes.deletedFiles == vector<wstring>{L"region/r.0.0.mca"},
		"deleted paths should be reported");

	filesystem::remove(backups / baseName);
	const auto missingBase = detector.Scan(source, metadata, backups);
	test.Expect(missingBase.status == BackupScanStatus::BaseBackupMissing,
		"missing base archive should force a new full chain");
}

void TestArchiveRunner(TestContext& test) {
	ExternalToolResolution resolution;
	resolution.available = true;
	resolution.executable = L"C:\\tools\\7za.exe";
	resolution.source = ExternalToolSource::Managed;

	ProcessSpec captured;
	int executions = 0;
	ArchiveRunner runner(
		resolution,
		{},
		[&](const ProcessSpec& spec, stop_token) {
			captured = spec;
			++executions;
			ProcessResult result;
			result.status = ProcessStatus::Succeeded;
			result.exitCode = 0;
			return result;
		});

	Config config;
	config.zipFormat = L"7z";
	config.zipMethod = L"LZMA2";
	config.cpuThreads = 4;
	const auto arguments = ArchiveRunner::BuildCreateArguments(config, 7, L"backup.7z");
	test.Expect(arguments.size() == 7 && arguments[0] == L"a"
			&& arguments[1] == L"-t7z" && arguments[2] == L"-m0=LZMA2"
			&& arguments[3] == L"-mx=7" && arguments[4] == L"-mmt4"
			&& arguments[6] == L"backup.7z",
		"archive create arguments should preserve the configured format and compression");
	test.Expect(runner.Execute(arguments, L"C:\\world", true).status == ProcessStatus::Succeeded,
		"injected archive process should report success");
	test.Expect(executions == 1 && captured.executable == resolution.executable
			&& captured.workingDirectory == L"C:\\world" && captured.useLowPriority,
		"archive runner should pass resolved executable and execution options once");
}

} // namespace

void RunBackupPipelineTests(TestContext& test, const filesystem::path& root) {
	TestPathRules(test, root);
	TestChangeDetector(test, root);
	TestArchiveRunner(test);
}
