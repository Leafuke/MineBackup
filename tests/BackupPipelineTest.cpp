#include "BackupPipelineTest.h"

#include "ArchiveRunner.h"
#include "BackupChangeDetector.h"
#include "FolderRewindMetadataStore.h"
#include "PathRuleSet.h"

#include <fstream>
#include <chrono>
#include <cerrno>
#include <stdexcept>

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

 const auto level=source/"level.dat";
 const auto tick=chrono::floor<chrono::seconds>(filesystem::last_write_time(level));
 filesystem::last_write_time(level,tick+chrono::milliseconds(200));
 state.fileStates=detector.Scan(source,metadata,backups).currentState;
 FolderRewindMetadataStore::SaveState(metadata,state);
 WriteFile(level,"other"); filesystem::last_write_time(level,tick+chrono::milliseconds(400));
 const auto subsecond=detector.Scan(source,metadata,backups);
 test.Expect(subsecond.changes.modifiedFiles==vector<wstring>{L"level.dat"},"same-size subsecond changes are backed up");
 state.fileStates=subsecond.currentState; FolderRewindMetadataStore::SaveState(metadata,state);
 for(int i=0;i<10;++i) test.Expect(detector.Scan(source,metadata,backups).status==BackupScanStatus::NoChange,"precise unchanged timestamps remain stable across scans");
 auto& legacyTime=state.fileStates[L"level.dat"].lastWriteTimeUtc;
 test.Expect(legacyTime.size()==30 && legacyTime[19]==L'.',"file timestamp retains nine fractional UTC digits");
 legacyTime=legacyTime.substr(0,19)+L"Z"; FolderRewindMetadataStore::SaveState(metadata,state);
 const auto legacy=detector.Scan(source,metadata,backups);
 test.Expect(legacy.status==BackupScanStatus::ChangesDetected,"legacy second precision conservatively refreshes file state");
 state.fileStates=legacy.currentState; FolderRewindMetadataStore::SaveState(metadata,state);
 test.Expect(detector.Scan(source,metadata,backups).status==BackupScanStatus::NoChange,"legacy refresh converges to unchanged state");

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

void TestExcludedScanPaths(TestContext& test, const filesystem::path& root) {
	const auto source = root / "excluded-scan" / "world";
	const auto metadata = root / "excluded-scan" / "metadata";
	const auto backups = root / "excluded-scan" / "backups";
	WriteFile(source / "level.dat", "world data");
	WriteFile(source / "cache" / "nested" / "item.dat", "excluded data");
	filesystem::create_directories(backups);
	BackupChangeDetector detector;
	vector<filesystem::path> visited;
	auto excludeCache = [&](const filesystem::path& relative) {
		visited.push_back(relative);
		return relative == "cache";
	};
#ifndef _WIN32
	// Legal Linux/macOS names are not necessarily portable archive names.
	WriteFile(source / "cache" / "namespace:data.txt", "excluded nonportable name");
	const auto rejected = detector.Scan(source, metadata, backups);
	test.Expect(rejected.status == BackupScanStatus::ScanFailed
		&& rejected.failureDetail.find("namespace:data.txt") != string::npos,
		"unsupported included paths should fail with a specific safe diagnostic");
	const auto outside = root / "excluded-scan" / "outside.txt";
	WriteFile(outside, "outside data");
	error_code linkError;
	filesystem::create_symlink(outside, source / "cache" / "linked.txt", linkError);
#endif
	const auto initial = detector.Scan(source, metadata, backups, excludeCache);
	test.Expect(initial.status == BackupScanStatus::MetadataInvalid
		&& initial.currentState.size() == 1 && initial.currentState.contains(L"level.dat"),
		"excluded directories must be pruned before inspecting unsupported names or outside symlinks");
	test.Expect(visited.size() == 2,
		"a pruned subtree must not visit or stat its descendants");

	const wstring baseName = L"[Full]-World.7z";
	WriteFile(backups / baseName, "archive");
	FolderRewindFormat::MetadataState state;
	state.lastBackupFileName = baseName;
	state.basedOnFullBackup = baseName;
	state.fileStates = initial.currentState;
	test.Expect(FolderRewindMetadataStore::SaveState(metadata, state),
		"filtered scan state should persist as the actual archived checkpoint");
	test.Expect(detector.Scan(source, metadata, backups, excludeCache).status == BackupScanStatus::NoChange,
		"excluded changes must not force unnecessary Smart backups");

	filesystem::remove_all(source / "cache");
	WriteFile(source / "cache" / "now-included.dat", "newly included data");
	const auto included = detector.Scan(source, metadata, backups);
	test.Expect(included.status == BackupScanStatus::ChangesDetected
		&& included.changes.addedFiles == vector<wstring>{L"cache/now-included.dat"},
		"removing an exclusion must add previously omitted files to the next Smart backup");
	state.fileStates = included.currentState;
	test.Expect(FolderRewindMetadataStore::SaveState(metadata, state),
		"newly included checkpoint should be saved");
	filesystem::remove(source / "cache" / "now-included.dat");
	const auto deleted = detector.Scan(source, metadata, backups);
	test.Expect(deleted.changes.deletedFiles == vector<wstring>{L"cache/now-included.dat"},
		"ordinary Smart deletion detection must remain intact after an exclusion changes");
#ifndef _WIN32
	WriteFile(source / "cache" / "hidden.dat", "unreadable excluded subtree");
	error_code permissionError;
	filesystem::permissions(source / "cache", filesystem::perms::none, permissionError);
	if (!permissionError) {
		const auto unreadable = detector.Scan(source, metadata, backups, excludeCache);
		filesystem::permissions(source / "cache", filesystem::perms::owner_all, permissionError);
		test.Expect(unreadable.status != BackupScanStatus::ScanFailed,
			"an explicitly excluded unreadable directory must not fail traversal");
	}
	filesystem::remove_all(source / "cache");
	const string invalidName = string("invalid-") + char(0xff) + ".dat";
	const auto invalidPath = source / filesystem::path(invalidName);
	bool fixtureWritten = false;
	int fixtureErrno = 0;
	{
		// APFS can reject byte sequences that Linux filesystems preserve. Check
		// the actual write instead of assuming the invalid-byte fixture exists.
		errno = 0;
		ofstream output(invalidPath, ios::binary | ios::trunc);
		if (output.is_open()) {
			output << "invalid UTF-8 filename";
			output.close();
			fixtureWritten = output.good();
		}
		fixtureErrno = errno;
	}
	const bool rejectedEncoding = fixtureErrno == EILSEQ || fixtureErrno == EINVAL;
	test.Expect(fixtureWritten || rejectedEncoding,
		"invalid-byte fixture creation must succeed or explicitly reject the filename encoding");
	bool enumeratedExactBytes = false;
	error_code enumerationError;
	filesystem::directory_iterator names(source, enumerationError), namesEnd;
	while (!enumerationError && names != namesEnd) {
		if (names->path().filename().native() == invalidName) enumeratedExactBytes = true;
		names.increment(enumerationError);
	}
	test.Expect(!enumerationError, "invalid-byte fixture capability probe must enumerate its directory");
	if (fixtureWritten && enumeratedExactBytes) {
		const auto invalidUtf8 = detector.Scan(source, metadata, backups);
		test.Expect(invalidUtf8.status == BackupScanStatus::ScanFailed
			&& !invalidUtf8.failureDetail.empty()
			&& invalidUtf8.failureDetail.find(char(0xff)) == string::npos,
			"scan failure details must sanitize invalid UTF-8 path bytes before JSON rendering");
	}
	else if (!enumerationError && (rejectedEncoding || fixtureWritten)) {
		cout << "[SKIP] invalid UTF-8 filename scan fixture: filesystem "
			<< (fixtureWritten ? "does not preserve the requested raw filename bytes" : "rejects the filename encoding")
			<< " (write errno " << fixtureErrno << ")\n";
	}
	// No invalid filename has to exist for this deterministic diagnostic test:
	// the missing child guarantees a failure even on byte-preserving filesystems.
	const auto missingInvalidPath = detector.Scan(invalidPath / "missing-child", metadata, backups);
	test.Expect(missingInvalidPath.status == BackupScanStatus::ScanFailed
		&& missingInvalidPath.failureDetail.find("Source directory is unavailable") != string::npos
		&& missingInvalidPath.failureDetail.find(char(0xff)) == string::npos,
		"missing-source diagnostics must sanitize invalid path bytes without a filesystem fixture");
#endif
	const auto ruleSource = root / "excluded-rule-exception";
	WriteFile(ruleSource / "level.dat", "valid filename for deterministic callback coverage");
	const auto rejectedRule = detector.Scan(ruleSource, metadata, backups,
		[](const filesystem::path&) -> bool { throw runtime_error("invalid path encoding"); });
	test.Expect(rejectedRule.status == BackupScanStatus::ScanFailed
		&& rejectedRule.failureDetail.find("Could not evaluate backup exclusions") != string::npos
		&& rejectedRule.failureDetail.find(char(0xff)) == string::npos,
		"exclusion evaluation failures must return safe diagnostics rather than escape the scanner");
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
	TestExcludedScanPaths(test, root);
	TestArchiveRunner(test);
}
