#include "RestoreServiceTests.h"

#include "ExternalToolManager.h"
#include "FolderRewindFormat.h"
#include "FolderRewindMetadataStore.h"
#include <chrono>
#include "RestoreService.h"
#include "RestoreWorkspace.h"
#include "WorldIdentity.h"

#include <fstream>
#include <memory>

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

struct FakeArchiveState {
	bool unsafeMembers = false;
	int tests = 0;
	int extracts = 0;
	bool failExtract = false;
	bool cancelExtract = false;
	bool cancelAfterSuccess = false;
	shared_ptr<stop_source> cancelSource;
};

ArchiveRunner FakeRunner(const shared_ptr<FakeArchiveState>& state, stop_token token) {
	ExternalToolResolution resolution;
	resolution.available = true;
	resolution.executable = L"fake-7zz";
	resolution.source = ExternalToolSource::Managed;
	return ArchiveRunner(std::move(resolution), token,
		[state](const ProcessSpec& spec, stop_token stopToken) {
			ProcessResult result;
			if (stopToken.stop_requested()) {
				result.status = ProcessStatus::Cancelled;
				return result;
			}
			if (!spec.arguments.empty() && spec.arguments.front() == L"l") { result.status=ProcessStatus::Succeeded; result.standardOutput=state->unsafeMembers ? "----------\nPath = C:\\world\\level.dat\n\n" : "----------\nPath = level.dat\n\n"; return result; }
			if (!spec.arguments.empty() && spec.arguments.front() == L"t") {
				++state->tests;
				result.status = ProcessStatus::Succeeded;
				return result;
			}
			if (!spec.arguments.empty() && spec.arguments.front() == L"x") {
				++state->extracts;
				if (spec.arguments.size() < 2
					|| !filesystem::is_regular_file(spec.arguments[1])) {
					result.status = ProcessStatus::FailedToStart;
					return result;
				}
				filesystem::path destination;
				for (const auto& argument : spec.arguments) {
					if (argument.rfind(L"-o", 0) == 0) destination = argument.substr(2);
				}
				Write(destination / "level.dat", "restored");
				Write(destination / "session.lock", "archive-session-lock");
				if ((state->cancelExtract || state->cancelAfterSuccess) && state->cancelSource) {
					state->cancelSource->request_stop();
				}
				result.status = state->failExtract
					? ProcessStatus::ExitedWithError
					: state->cancelExtract ? ProcessStatus::Cancelled : ProcessStatus::Succeeded;
				result.exitCode = state->failExtract ? 2 : 0;
				return result;
			}
			result.status = ProcessStatus::FailedToStart;
			return result;
		});
}

RestoreRequest FixtureRequest(const filesystem::path& root) {
	RestoreRequest request;
	request.config.configId = L"config-restore";
	request.config.saveRoot = (root / "saves").wstring();
	request.config.backupPath = (root / "backups").wstring();
	request.config.zipPath = L"fake-7zz";
	request.config.worlds = {{L"world",L""},{L"missing-world",L""},{L"missing-overlay-world",L""},{L"cancelled-world",L""}};
	request.world = {request.config.configId, L"world"};
	request.archive = L"[Full]-World.7z";
	request.restorePreserve = {L"session.lock"};
	return request;
}

} // namespace

void RunRestoreServiceTests(
	TestContext& test,
	const filesystem::path& temporaryRoot) {
 for (auto mode : {RestoreWorkspace::Mode::Clean, RestoreWorkspace::Mode::Overlay}) {
  const auto target = temporaryRoot / (mode == RestoreWorkspace::Mode::Clean ? "cleanup-clean" : "cleanup-overlay");
  Write(target / "a-old.txt", "original"); Write(target / "z-locked.txt", "locked");
  RestoreWorkspace::State workspace; string error;
  test.Expect(RestoreWorkspace::Prepare(target, workspace, error, mode), "cleanup fixture prepares snapshot");
  Write(target / "a-old.txt", "restored");
  RestoreWorkspace::CommitOptions options;
  options.removeSnapshot = [](const filesystem::path& snapshot, error_code& ec) {
   filesystem::remove(snapshot / "a-old.txt"); ec = make_error_code(errc::permission_denied);
  };
  const auto committed = RestoreWorkspace::Commit(workspace, {}, error, options);
  test.Expect(committed.status == RestoreWorkspace::CommitStatus::CleanupWarning
   && filesystem::exists(committed.retainedSnapshot / "z-locked.txt") && !workspace.CanRollback(),
   "partial snapshot cleanup is a committed warning with retained recovery path");
  test.Expect(RestoreWorkspace::Rollback(workspace, error) && Read(target / "a-old.txt") == "restored",
   "post-commit rollback never replaces restored world with partial snapshot");
 }

	const filesystem::path root = temporaryRoot / "restore-service";
	const filesystem::path world = root / "saves" / "world";
	Write(world / "level.dat", "before");
	Write(world / "old.txt", "old");
	Write(world / "session.lock", "preserve");
	Write(root / "backups" / "world" / "[Full]-World.7z", "archive");

	auto state = make_shared<FakeArchiveState>();
	bool occupied = false;
	RestoreServiceDependencies dependencies;
	dependencies.paths.runtimeRoot = root / "runtime";
	dependencies.isWorldOccupied = [&](const filesystem::path&) { return occupied; };
	dependencies.archiveRunnerFactory = [state](
		const filesystem::path&, const AppPaths&, stop_token token) {
		return FakeRunner(state, token);
	};
	RestoreService service(dependencies);
	auto request = FixtureRequest(root);

	const auto verified = service.Verify(request);
	test.Expect(verified.code == OperationCode::Success
			&& verified.archiveChain.size() == 1
			&& verified.checkedArchiveCount == 1
			&& state->tests == 1 && state->extracts == 0,
		"Restore verification should plan the local archive chain and run 7z t without mutation");
	const auto dryRun = service.Run(request, true);
	test.Expect(dryRun.code == OperationCode::Success && dryRun.dryRun
			&& Read(world / "level.dat") == "before" && state->extracts == 0,
		"Restore dry-run should complete verification without modifying the world");

	const auto restored = service.Run(request, false);
	test.Expect(restored.code == OperationCode::Success
			&& Read(world / "level.dat") == "restored"
			&& !filesystem::exists(world / "old.txt")
			&& Read(world / "session.lock") == "preserve",
		"Clean restore should replace the world and overwrite archive files with preserved entries");

	bool safetyRetentionDeferred = false;
	bool safetyRetentionRan = false;
	RestoreServiceDependencies safetyDependencies = dependencies;
	safetyDependencies.backupBeforeRestore = [&](
		const BackupRequest&, stop_token, BackupExecutionOptions options) {
			safetyRetentionDeferred = options.deferRetention;
			if (!options.deferRetention) {
				error_code ignored;
				filesystem::remove(root / "backups" / "world" / "[Full]-World.7z", ignored);
			}
			BackupResult backup;
			backup.code = OperationCode::Success;
			backup.outcome = BackupOutcome::Created;
			HistoryEntry entry;
			entry.configId = request.config.configId;
			entry.worldName = L"world";
			entry.worldPath = world.wstring();
			entry.backupFile = L"[Full]-Safety.7z";
			backup.historyEntry = entry;
			return backup;
		};
	safetyDependencies.enforceRetention = [&](const BackupRequest&, const HistoryEntry&, stop_token) {
			safetyRetentionRan = true;
		};
		RestoreService safetyService(std::move(safetyDependencies));
		request.config.backupBefore = true;
		const auto safetyRestored = safetyService.Run(request, false);
		test.Expect(safetyRestored.code == OperationCode::Success
				&& safetyRetentionDeferred && safetyRetentionRan
				&& Read(world / "level.dat") == "restored",
			"Restore should keep the verified target while deferring safety-backup retention until commit");
		request.config.backupBefore = false;

	Write(world / "level.dat", "rollback-source");
	Write(world / "old.txt", "rollback-old");
	state->failExtract = true;
	const auto failed = service.Run(request, false);
	test.Expect(failed.code == OperationCode::RestoreFailed
			&& failed.rollbackAttempted && failed.rollbackSucceeded
			&& Read(world / "level.dat") == "rollback-source"
			&& Read(world / "old.txt") == "rollback-old",
		"Clean restore should roll back the original world after extraction failure");
	state->failExtract = false;

	Write(world / "level.dat", "overlay-source");
	request.world = {request.config.configId, L"world"};
	request.archive = L"[Full]-World.7z";
	request.mode = RestoreMode::Overwrite;
	state->failExtract = true;
	const auto failedOverlay = service.Run(request, false);
	test.Expect(failedOverlay.code == OperationCode::RestoreFailed
			&& failedOverlay.rollbackAttempted && failedOverlay.rollbackSucceeded
			&& Read(world / "level.dat") == "overlay-source",
		"Failed overwrite restore should roll back the original world overlay");
	state->failExtract = false;

	const filesystem::path missingWorld = root / "saves" / "missing-world";
	Write(root / "backups" / "missing-world" / "[Full]-World.7z", "archive");
	request.world = {request.config.configId, L"missing-world"};
	request.archive = L"[Full]-World.7z";
	request.mode = RestoreMode::Clean;
	state->failExtract = true;
	const auto missingTargetFailed = service.Run(request, false);
	test.Expect(missingTargetFailed.code == OperationCode::RestoreFailed
			&& missingTargetFailed.rollbackAttempted
			&& missingTargetFailed.rollbackSucceeded
			&& !filesystem::exists(missingWorld),
		"Failed clean restore must remove a newly created target when no world existed before");

	const filesystem::path missingOverlayWorld = root / "saves" / "missing-overlay-world";
	Write(root / "backups" / "missing-overlay-world" / "[Full]-World.7z", "archive");
	request.world = {request.config.configId, L"missing-overlay-world"};
	request.archive = L"[Full]-World.7z";
	request.mode = RestoreMode::Overwrite;
	state->failExtract = true;
	const auto missingOverlayFailed = service.Run(request, false);
	test.Expect(missingOverlayFailed.code == OperationCode::RestoreFailed
			&& missingOverlayFailed.rollbackAttempted
			&& missingOverlayFailed.rollbackSucceeded
			&& !filesystem::exists(missingOverlayWorld),
		"Failed overwrite restore must remove a newly created target when no world existed before");
	state->failExtract = false;

	const filesystem::path cancelledWorld = root / "saves" / "cancelled-world";
	Write(root / "backups" / "cancelled-world" / "[Full]-World.7z", "archive");
	request.world = {request.config.configId, L"cancelled-world"};
	state->failExtract = false;
	request.mode = RestoreMode::Clean;
	state->cancelExtract = true;
	state->cancelSource = make_shared<stop_source>();
	const auto cancelled = service.Run(request, false, state->cancelSource->get_token());
	test.Expect(cancelled.code == OperationCode::Cancelled
			&& cancelled.rollbackAttempted
			&& cancelled.rollbackSucceeded
			&& !filesystem::exists(cancelledWorld),
		"Cancelled clean restore must remove a newly created target when no world existed before");
	state->cancelExtract = false;
	state->cancelSource.reset();
	for (const auto mode : {RestoreMode::Clean, RestoreMode::Overwrite}) {
		Write(cancelledWorld / "level.dat", "original");
		Write(cancelledWorld / "keep.txt", "untouched");
		request.mode = mode;
		state->cancelAfterSuccess = true;
		state->cancelSource = make_shared<stop_source>();
		const auto raced = service.Run(request, false, state->cancelSource->get_token());
		test.Expect(raced.code == OperationCode::Cancelled && raced.rollbackAttempted
			&& raced.rollbackSucceeded && Read(cancelledWorld / "level.dat") == "original"
			&& Read(cancelledWorld / "keep.txt") == "untouched",
			"cancellation after successful extraction must roll back the complete original world");
	}
	state->cancelAfterSuccess = false;
	state->cancelSource.reset();

	occupied = true;
	const int extractsBeforeOccupied = state->extracts;
	const auto occupiedResult = service.Run(request, false);
	test.Expect(occupiedResult.code == OperationCode::RestoreFailed
			&& state->extracts == extractsBeforeOccupied,
		"Cold restore should refuse an occupied world after archive verification");
	occupied = false;

	request.world = {request.config.configId, L"world"};
	Write(root / "backups" / "world" / "[Smart]-World.7z", "incremental");
	request.archive = L"[Smart]-World.7z";
	const auto missingMetadata = service.Verify(request);
	test.Expect(missingMetadata.code == OperationCode::VerificationFailed
			&& !missingMetadata.diagnostics.empty()
			&& missingMetadata.diagnostics.front().eventId == "restore.metadata.missing",
		"Smart restore should reject a missing exact metadata chain");
 auto nested = FixtureRequest(root); nested.config.worlds = {{L"nested/world",L""}}; nested.world.relativePath=L"nested/world";
 Write(root/"saves"/"nested"/"world"/"level.dat","before"); Write(root/"saves"/"nested_world"/"unrelated.txt","keep");
 Write(root/"backups"/"nested_world"/nested.archive,"archive");
 HistoryEntry entry; entry.configId=nested.config.configId; entry.worldName=L"nested_world"; entry.worldPath=(root/"saves"/"nested"/"world").wstring();
 WorldIdentity::Value identity;
 test.Expect(WorldIdentity::TryResolveHistory(nested.config,entry,identity) && identity.relativeWorldPath==L"nested/world", "history resolves configured nested source");
 test.Expect(service.Run(nested,false).code==OperationCode::Success && Read(root/"saves"/"nested_world"/"unrelated.txt")=="keep", "nested restore does not touch flattened sibling");
 nested.world.relativePath=L"nested_world";
 test.Expect(service.Run(nested,false).code==OperationCode::RestoreFailed, "unconfigured storage alias cannot become restore target");
 entry.worldPath=(root/"saves"/"deleted-world").wstring();
 test.Expect(!WorldIdentity::TryResolveHistory(nested.config,entry,identity)
  && !WorldIdentity::Matches(nested.config,L"nested/world",entry),
  "recorded unconfigured source cannot fall back to a matching storage alias");
 auto other=entry; other.worldPath=(root/"saves"/"nested"/"world").wstring();
 test.Expect(!WorldIdentity::SameHistoryEntry(nested.config,entry,other),
  "history mutations cannot redirect a removed source to another world");
 entry.worldPath.clear(); test.Expect(WorldIdentity::TryResolveHistory(nested.config,entry,identity),"unique legacy history alias remains supported");
 nested.config.worlds.push_back({L"nested_world",L""});
 test.Expect(!WorldIdentity::TryResolveHistory(nested.config,entry,identity),"ambiguous history aliases are rejected");

 {
  ExternalToolResolution resolution; resolution.available=true; resolution.executable=L"fake";
  ArchiveRunner largeListing(resolution,{},[](const ProcessSpec& spec,stop_token){
   ProcessResult result; result.status=ProcessStatus::Succeeded;
   const string member="Path = region/r.0.0.mca\nSize = 1024\n\n";
   result.standardOutput="----------\n";
   for(int i=0;i<160000;++i) result.standardOutput+=member;
   if(result.standardOutput.size()>spec.maximumCapturedBytes) {
    result.outputTruncated=true; result.standardOutput.resize(spec.maximumCapturedBytes);
   }
   return result;
  });
  string error;
  test.Expect(largeListing.ValidateMembers("large.7z",error),
   "valid large archive listings must not be rejected by the process log capture limit");
 }
 string pathError;
 for(const string bad:{"C:\\world\\level.dat","//server/world/level.dat","/world/level.dat","../world/level.dat","folder/../../level.dat"})
  test.Expect(!ArchiveRunner::ValidateMemberListing("----------\nPath = "+bad+"\n",pathError),"absolute and traversing archive members are rejected");
 test.Expect(!ArchiveRunner::ValidateMemberListing("invalid",pathError),"unrecognized archive listings fail closed");
 test.Expect(ArchiveRunner::ValidateMemberListing("----------\nPath = region/r.0.0.mca\n",pathError),"relative archive layouts remain supported");

 {
  auto preparedRequest=FixtureRequest(root); preparedRequest.config.backupBefore=true;
  RestoreExecutionOptions options; RestoreSafetyBackup safety; safety.request.config=preparedRequest.config; safety.request.world=preparedRequest.world; safety.request.sourcePath=world;
  safety.result.code=OperationCode::NoChanges; safety.result.outcome=BackupOutcome::NoChanges; options.preparedSafetyBackup=safety;
  RestoreServiceDependencies deps=dependencies; int backupCalls=0; deps.backupBeforeRestore=[&](const BackupRequest&,stop_token,BackupExecutionOptions){++backupCalls;return BackupResult{};};
  test.Expect(RestoreService(deps).Run(preparedRequest,false,{},options).code==OperationCode::Success && backupCalls==0,"prepared successful safety backup is reused without another backup");
  options.preparedSafetyBackup->result.code=OperationCode::BackupFailed; Write(world/"level.dat","protect");
  test.Expect(RestoreService(deps).Run(preparedRequest,false,{},options).code==OperationCode::RestoreFailed && Read(world/"level.dat")=="protect","failed prepared safety backup blocks restore before mutation");
 }


    {
        const auto root = temporaryRoot / "preflight";
        auto req = FixtureRequest(root); req.config.backupBefore = true;
        const auto world = root / "saves" / "world";
        const auto backups = root / "backups" / "world";
        Write(world / "level.dat", "protected");
        Write(backups / "[Full]-Base.7z", "full"); Write(backups / "[Smart]-Old.7z", "smart");
        filesystem::last_write_time(backups / "[Full]-Base.7z", filesystem::file_time_type::clock::now() - chrono::hours(2));
        filesystem::last_write_time(backups / "[Smart]-Old.7z", filesystem::file_time_type::clock::now() - chrono::hours(1));
        req.archive = L"[Smart]-Old.7z";
        auto fake = make_shared<FakeArchiveState>();
        RestoreServiceDependencies deps; deps.paths.runtimeRoot = root / "runtime";
        deps.archiveRunnerFactory = [fake](const auto&, const auto&, stop_token token) { return FakeRunner(fake, token); };
        int backupsCalled = 0, repairCalled = 0;
        deps.backupBeforeRestore = [&](const auto&,stop_token,BackupExecutionOptions) {
            ++backupsCalled; BackupResult result; result.code = OperationCode::Success; return result;
        };
        const auto custom = RestoreService(deps).Verify(req, {}, RestoreVerificationMode::LegacyForward);
        const auto reverse = RestoreService(deps).Verify(req, {}, RestoreVerificationMode::Reverse);
        test.Expect(IsSuccessful(custom.code) && custom.archiveChain.size() == 2 && !custom.usesExactSmartPlan
            && IsSuccessful(reverse.code) && reverse.archiveChain.size() == 1,
            "Metadata-free legacy Smart custom/reverse plans remain accepted without Clean requirements");
        test.Expect(!IsSuccessful(RestoreService(deps).Run(req, false).code) && backupsCalled == 0
            && Read(world / "level.dat") == "protected", "Managed Clean restore remains strict and never prepares safety backup on missing metadata");
        Write(backups / "[Full]-Safety.7z", "new safety");
        test.Expect(reverse.archiveChain.size() == 1 && reverse.archiveChain.front().filename() == req.archive,
            "Pinned reverse preflight plan cannot include a subsequently created safety backup");
        FolderRewindFormat::StoragePaths storage;
        FolderRewindFormat::TryResolveStoragePaths(req.config.backupPath, L"world", world.wstring(), storage);
        auto writeMetadata = [&] {
            FolderRewindFormat::ChangeRecord full; full.archiveFileName = L"[Full]-Base.7z"; full.backupType = L"Full";
            full.basedOnFullBackup = full.archiveFileName; full.fullFileList = {L"level.dat"};
            FolderRewindFormat::ChangeRecord smart; smart.archiveFileName = L"[Smart]-Old.7z"; smart.backupType = L"Smart";
            smart.previousBackupFileName = full.archiveFileName; smart.basedOnFullBackup = full.archiveFileName;
            smart.modifiedFiles = {L"level.dat"}; smart.fullFileList = full.fullFileList;
            test.Expect(FolderRewindMetadataStore::SaveRecord(storage.metadataDir, full)
                && FolderRewindMetadataStore::SaveRecord(storage.metadataDir, smart), "Repair fixture commits a valid metadata chain");
        };
        deps.repairArchiveChain = [&](const auto&,stop_token) { ++repairCalled; writeMetadata(); };
        const auto repaired = RestoreService(deps).Verify(req);
        test.Expect(IsSuccessful(repaired.code) && repaired.usesExactSmartPlan && repairCalled == 1
            && backupsCalled == 0 && fake->extracts == 0 && Read(world / "level.dat") == "protected",
            "Repairable Smart preflight repairs metadata once before any safety backup or world mutation");
        filesystem::remove(backups / "[Full]-Base.7z");
        deps.repairArchiveChain = [&](const auto&,stop_token) { ++repairCalled; Write(backups / "[Full]-Base.7z", "downloaded full"); };
        test.Expect(IsSuccessful(RestoreService(deps).Verify(req).code) && repairCalled == 2,
            "Missing cloud predecessor is repaired before Smart chain verification");
        filesystem::remove(backups / "[Full]-Base.7z");
        deps.repairArchiveChain = [&](const auto&,stop_token) { ++repairCalled; };
        const auto failed = RestoreService(deps).Run(req, false);
        test.Expect(!IsSuccessful(failed.code) && repairCalled == 3 && backupsCalled == 0 && !failed.rollbackAttempted
            && Read(world / "level.dat") == "protected", "Failed repair is attempted once and leaves the world untouched");
        stop_source cancellation;
        deps.repairArchiveChain = [&](const auto&,stop_token) { ++repairCalled; cancellation.request_stop(); };
        test.Expect(RestoreService(deps).Run(req, false, cancellation.get_token()).code == OperationCode::Cancelled
            && backupsCalled == 0 && Read(world / "level.dat") == "protected", "Cancellation during repair blocks backup and workspace preparation");
        const auto callsBeforeInvalid = repairCalled;
        req.world.relativePath = L"unconfigured";
        test.Expect(!IsSuccessful(RestoreService(deps).Verify(req).code) && repairCalled == callsBeforeInvalid,
            "Unconfigured worlds cannot invoke cloud or migration repair");
        req.world.relativePath = L"world"; req.archive = L"../[Smart]-Escape.7z";
        test.Expect(!IsSuccessful(RestoreService(deps).Verify(req).code) && repairCalled == callsBeforeInvalid,
            "Out-of-storage archive paths cannot invoke repair");
        req.archive = L"[Smart]-Old.7z"; Write(backups / "[Full]-Base.7z", "full");
        fake->unsafeMembers = true;
        for (auto mode : {RestoreVerificationMode::Managed, RestoreVerificationMode::LegacyForward, RestoreVerificationMode::Reverse})
            test.Expect(!IsSuccessful(RestoreService(deps).Verify(req, {}, mode).code) && repairCalled == callsBeforeInvalid,
                "All restore modes reject unsafe archive members without repair or safety backup");
        FolderRewindMetadataStore::DeleteRecord(storage.metadataDir, L"[Smart]-Old.7z");
        test.Expect(!IsSuccessful(RestoreService(deps).Verify(req).code) && repairCalled == callsBeforeInvalid,
            "Unsafe selected archive with missing metadata is rejected before repair can modify history");
        fake->unsafeMembers = false;
        deps.repairArchiveChain = [&](const auto&,stop_token) { ++repairCalled; writeMetadata(); fake->unsafeMembers = true; };
        test.Expect(!IsSuccessful(RestoreService(deps).Run(req, false).code) && backupsCalled == 0
            && Read(world / "level.dat") == "protected", "Repaired materials are rechecked for unsafe member paths before backup");
        fake->unsafeMembers = false;
        FolderRewindFormat::ChangeRecord full; full.archiveFileName = L"[Full]-Base.7z"; full.backupType = L"Full";
        FolderRewindMetadataStore::SaveRecord(storage.metadataDir, full); // Older metadata has no exact file list.
        deps.repairArchiveChain = {};
        req.mode = RestoreMode::Overwrite;
        test.Expect(IsSuccessful(RestoreService(deps).Verify(req).code), "Overlay preflight does not require the Clean full-file ownership plan");
        req.mode = RestoreMode::Clean;
        test.Expect(!IsSuccessful(RestoreService(deps).Verify(req).code), "Clean preflight still rejects an incomplete exact file plan");
    }

}
