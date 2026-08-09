#include "RestoreServiceTests.h"

#include "ExternalToolManager.h"
#include "RestoreService.h"

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
	int tests = 0;
	int extracts = 0;
	bool failExtract = false;
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
			if (!spec.arguments.empty() && spec.arguments.front() == L"t") {
				++state->tests;
				result.status = ProcessStatus::Succeeded;
				return result;
			}
			if (!spec.arguments.empty() && spec.arguments.front() == L"x") {
				++state->extracts;
				filesystem::path destination;
				for (const auto& argument : spec.arguments) {
					if (argument.rfind(L"-o", 0) == 0) destination = argument.substr(2);
				}
				Write(destination / "level.dat", "restored");
				result.status = state->failExtract
					? ProcessStatus::ExitedWithError : ProcessStatus::Succeeded;
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
	request.world = {request.config.configId, L"world"};
	request.archive = L"[Full]-World.7z";
	request.restorePreserve = {L"session.lock"};
	return request;
}

} // namespace

void RunRestoreServiceTests(
	TestContext& test,
	const filesystem::path& temporaryRoot) {
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
		"Clean restore should replace the world and copy configured preserved entries");

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

	occupied = true;
	const int extractsBeforeOccupied = state->extracts;
	const auto occupiedResult = service.Run(request, false);
	test.Expect(occupiedResult.code == OperationCode::RestoreFailed
			&& state->extracts == extractsBeforeOccupied,
		"Cold restore should refuse an occupied world after archive verification");
	occupied = false;

	Write(root / "backups" / "world" / "[Smart]-World.7z", "incremental");
	request.archive = L"[Smart]-World.7z";
	const auto missingMetadata = service.Verify(request);
	test.Expect(missingMetadata.code == OperationCode::VerificationFailed
			&& !missingMetadata.diagnostics.empty()
			&& missingMetadata.diagnostics.front().eventId == "restore.metadata.missing",
		"Smart restore should reject a missing exact metadata chain");
}
