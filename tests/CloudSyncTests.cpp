#include "CloudSyncTests.h"

#include "CloudHistoryAnalysis.h"
#include "RcloneClient.h"
#include "TestSupport.h"

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <vector>

using namespace std;

namespace {
	ProcessResult FailedProcess(int exitCode, string error = {}) {
		ProcessResult result;
		result.status = ProcessStatus::ExitedWithError;
		result.exitCode = exitCode;
		result.standardError = std::move(error);
		return result;
	}

	void TestRcloneTransport(TestContext& test) {
		vector<ProcessSpec> invocations;
		RcloneClientOptions options;
		options.executable = L"test-rclone";
		options.workingDirectory = L"test-working-directory";
		options.timeout = chrono::seconds(17);
		options.retryCount = 2;
		options.useLowPriority = true;

		int attempts = 0;
		RcloneClient client(options, [&](const ProcessSpec& spec, stop_token) {
			invocations.push_back(spec);
			if (++attempts < 3) return FailedProcess(5, "temporary backend error");
			ProcessResult success;
			success.status = ProcessStatus::Succeeded;
			success.exitCode = 0;
			return success;
		});
		const RcloneExecutionResult copied = client.CopyTo(L"source path", L"remote:path/file");
		test.Expect(copied.command.success && copied.attemptCount == 3,
			"RcloneClient should retry the configured number of times and return the final success");
		test.Expect(invocations.size() == 3
			&& invocations.front().executable == options.executable
			&& invocations.front().arguments
				== vector<wstring>({L"copyto", L"source path", L"remote:path/file", L"--progress"})
			&& invocations.front().workingDirectory == options.workingDirectory
			&& invocations.front().timeout == options.timeout
			&& invocations.front().useLowPriority,
			"RcloneClient should build a literal argument vector and apply timeout, workdir and priority");

		stop_source cancellation;
		cancellation.request_stop();
		options.stopToken = cancellation.get_token();
		int cancelledInvocations = 0;
		const RcloneExecutionResult cancelled = RcloneClient(options,
			[&](const ProcessSpec&, stop_token) {
				++cancelledInvocations;
				return FailedProcess(1);
			}).Copy(L"source", L"remote:");
		test.Expect(cancelled.cancelled && cancelled.attemptCount == 0 && cancelledInvocations == 0,
			"RcloneClient should honor cancellation before starting or retrying a process");

		options.stopToken = {};
		options.retryCount = 0;
		const RcloneExecutionResult timedOut = RcloneClient(options,
			[](const ProcessSpec&, stop_token) {
				ProcessResult result;
				result.status = ProcessStatus::TimedOut;
				result.exitCode = -1;
				return result;
			}).CopyTo(L"source", L"remote:");
		test.Expect(timedOut.command.timedOut && !timedOut.command.success
			&& timedOut.attemptCount == 1,
			"RcloneClient should preserve timeout classification from the process executor");

		CloudCommandResult missing;
		missing.exitCode = 3;
		test.Expect(RcloneClient::IsRemoteObjectMissing(missing),
			"rclone exit code 3 should be classified as a missing remote object");
		missing.exitCode = 1;
		missing.detail = L"directory not found";
		test.Expect(RcloneClient::IsRemoteObjectMissing(missing),
			"rclone missing-object diagnostics should be classified without depending only on exit codes");
		missing.detail = L"authentication failed: unauthorized";
		test.Expect(!RcloneClient::IsRemoteObjectMissing(missing),
			"authentication failures must not be mistaken for absent remote data");
		missing.timedOut = true;
		missing.detail = L"file not found";
		test.Expect(!RcloneClient::IsRemoteObjectMissing(missing),
			"timeouts must remain transport failures even when stderr mentions a missing file");
	}

	HistoryEntry RemoteEntry(
		const Config& config,
		const wstring& worldName,
		const wstring& worldPath,
		const wstring& fileName) {
		HistoryEntry entry;
		entry.configId = config.configId;
		entry.worldName = worldName;
		entry.worldPath = worldPath;
		entry.backupFile = fileName;
		entry.timestamp_str = L"2026-01-02T03:04:05Z";
		return entry;
	}

	void TestRemoteHistoryAnalysis(TestContext& test) {
		Config config;
		config.configId = L"11111111-1111-4111-8111-111111111111";
		config.saveRoot = L"C:\\Minecraft\\saves";
		config.rcloneRemotePath = L"remote:minebackup";
		config.worlds = {
			{L"Alpha", L""},
			{L"Duplicate", L"first"},
			{L"Duplicate", L"second"},
			{L"PathWinner", L""}};

		const wstring alphaPath = (filesystem::path(config.saveRoot) / L"Alpha").wstring();
		const wstring pathWinner = (filesystem::path(config.saveRoot) / L"PathWinner").wstring();
		vector<HistoryEntry> remote{
			RemoteEntry(config, L"WrongName", pathWinner, L"path.7z"),
			RemoteEntry(config, L"Alpha", L"", L"name.7z"),
			RemoteEntry(config, L"Duplicate", L"", L"ambiguous.7z"),
			RemoteEntry(config, L"Missing", L"", L"unmapped.7z"),
			RemoteEntry(config, L"Alpha", alphaPath, L"inactive.7z")};
		vector<HistoryEntry> local{
			RemoteEntry(config, L"Alpha", alphaPath, L"name.7z")};

		CloudActiveHistoryManifest manifest;
		for (const auto index : {0, 1, 2, 3}) {
			CloudActiveHistoryEntry active;
			active.folderName = remote[index].worldName;
			active.folderPath = remote[index].worldPath;
			active.fileName = remote[index].backupFile;
			active.timestamp = remote[index].timestamp_str;
			manifest.entries.push_back(std::move(active));
		}

		const CloudHistoryAnalysisResult analysis =
			AnalyzeRemoteHistory(config, local, remote, manifest);
		test.Expect(analysis.success && analysis.totalRemoteEntries == 5
			&& analysis.matchedEntries == 2 && analysis.importableEntries == 1
			&& analysis.ambiguousEntries == 1 && analysis.unmappedEntries == 1,
			"remote analysis should apply manifest filtering and report mapped, duplicate, ambiguous and unmapped items");
		test.Expect(analysis.mappedItems.size() == 2
			&& analysis.mappedItems[0].worldName == L"PathWinner"
			&& analysis.mappedItems[1].worldName == L"Alpha",
			"remote analysis should prefer an exact normalized path and then fall back to world name");

		Config other = config;
		other.configId = L"22222222-2222-4222-8222-222222222222";
		remote.push_back(RemoteEntry(other, L"Alpha", alphaPath, L"other-config.7z"));
		const auto withoutManifest = AnalyzeRemoteHistory(config, local, remote, nullopt);
		test.Expect(withoutManifest.totalRemoteEntries == 6
			&& withoutManifest.matchedEntries == 3,
			"remote analysis should count the complete snapshot but ignore entries owned by another ConfigId");
	}
}

void RunCloudSyncTests(TestContext& test) {
	TestRcloneTransport(test);
	TestRemoteHistoryAnalysis(test);
}
