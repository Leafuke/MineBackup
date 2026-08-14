#include "JobTests.h"

#include "JobDocument.h"
#include "JobRunner.h"
#include "json.hpp"
#include "text_to_text.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace std;

namespace {

constexpr wchar_t JobId[] = L"11111111-1111-4111-8111-111111111111";
constexpr wchar_t StageId[] = L"22222222-2222-4222-8222-222222222222";
constexpr wchar_t StepA[] = L"33333333-3333-4333-8333-333333333333";
constexpr wchar_t StepB[] = L"44444444-4444-4444-8444-444444444444";
constexpr wchar_t ConfigId[] = L"55555555-5555-4555-8555-555555555555";

string ValidDocument() {
	return R"({
  "schemaVersion": 1,
  "jobs": [{
    "jobId": "11111111-1111-4111-8111-111111111111",
    "name": "Nightly",
    "stages": [{
      "stageId": "22222222-2222-4222-8222-222222222222",
      "name": "Backup worlds",
      "steps": [{
        "stepId": "33333333-3333-4333-8333-333333333333",
        "name": "Backup",
        "type": "backup",
        "target": {
          "configId": "55555555-5555-4555-8555-555555555555",
          "worldPath": "world/sub"
        }
      }, {
        "stepId": "44444444-4444-4444-8444-444444444444",
        "name": "Notify",
        "type": "process",
        "executable": "notify-tool",
        "arguments": ["--message", "backup done"],
        "workingDirectory": "",
        "timeoutSeconds": 30,
        "maximumCapturedBytes": 4096,
        "lowPriority": true
      }]
    }]
  }]
})";
}

JobStep BackupStep(const wchar_t* id, const wchar_t* world) {
	JobStep step;
	step.stepId = id;
	step.name = "backup";
	step.type = JobStepType::Backup;
	step.backup.configId = ConfigId;
	step.backup.worldPath = world;
	return step;
}

} // namespace

void RunJobTests(TestContext& test, const filesystem::path& temporaryRoot) {
	const auto parsed = JobStorage::Parse(ValidDocument());
	test.Expect(parsed.IsLoaded() && parsed.document.jobs.size() == 1
			&& parsed.document.jobs.front().stages.front().steps.size() == 2,
		"Job schema v1 should parse Backup and explicit Process steps");
	test.Expect(JobStorage::Serialize(parsed.document).find("\"executable\": \"notify-tool\"")
			!= string::npos,
		"Job serialization should preserve executable and argument-vector process contracts");

	string uppercase = ValidDocument();
	uppercase.replace(uppercase.find("11111111-1111-4111-8111-111111111111"), 1, "A");
	test.Expect(JobStorage::Parse(uppercase).status == JobStorage::LoadStatus::Invalid,
		"Job identities should require canonical lowercase UUID text");
	string unknown = ValidDocument();
	unknown.replace(unknown.find("\"name\": \"Nightly\""),
		string("\"name\": \"Nightly\"").size(),
		"\"name\": \"Nightly\", \"trigger\": \"scheduled\"");
	test.Expect(JobStorage::Parse(unknown).status == JobStorage::LoadStatus::Invalid,
		"Job schema should reject embedded scheduling and other unknown fields");

	map<int, Config> configs;
	Config config;
	config.configId = ConfigId;
	config.worlds = {{L"world/sub", L"Primary"}};
	configs.emplace(1, config);
	vector<Diagnostic> referenceDiagnostics;
	test.Expect(JobStorage::ValidateReferences(parsed.document, configs, referenceDiagnostics),
		"Job backup targets should resolve by ConfigId and normalized world path");
	configs.at(1).worlds.clear();
	referenceDiagnostics.clear();
	test.Expect(!JobStorage::ValidateReferences(parsed.document, configs, referenceDiagnostics)
			&& !referenceDiagnostics.empty(),
		"Job validation should reject dangling backup targets before execution");

	Job job;
	job.jobId = JobId;
	job.name = "parallel";
	JobStage first;
	first.stageId = StageId;
	first.name = "parallel";
	first.steps = {BackupStep(StepA, L"success"), BackupStep(StepB, L"failure")};
	JobStage second;
	second.stageId = L"66666666-6666-4666-8666-666666666666";
	second.name = "skipped";
	second.steps = {BackupStep(L"77777777-7777-4777-8777-777777777777", L"later")};
	job.stages = {first, second};

	atomic<int> active{0};
	atomic<int> maximumActive{0};
	atomic<int> laterRuns{0};
	JobRunnerDependencies dependencies;
	dependencies.resolveBackup = [](const JobBackupTarget& target) -> optional<BackupRequest> {
		BackupRequest request;
		request.world = {target.configId, target.worldPath};
		return request;
	};
	dependencies.runBackup = [&](const BackupRequest& request, stop_token) {
		BackupResult result;
		const int now = ++active;
		maximumActive.store(max(maximumActive.load(), now));
		this_thread::sleep_for(chrono::milliseconds(50));
		--active;
		if (request.world.relativePath == L"later") ++laterRuns;
		result.code = request.world.relativePath == L"failure"
			? OperationCode::BackupFailed : OperationCode::Success;
		return result;
	};
	const auto run = JobRunner(std::move(dependencies)).Run(job);
	test.Expect(run.code == OperationCode::PartialSuccess
			&& run.stages.size() == 2
			&& run.stages.front().steps.size() == 2
			&& run.stages.back().skipped
			&& maximumActive.load() == 2
			&& laterRuns.load() == 0,
		"Job runner should execute a Stage in parallel and skip later Stages after mixed failure");

	Job diagnosticJob;
	diagnosticJob.jobId = JobId;
	diagnosticJob.name = "diagnostic encoding";
	JobStage diagnosticStage;
	diagnosticStage.stageId = StageId;
	diagnosticStage.name = "process";
	JobStep diagnosticStep;
	diagnosticStep.stepId = StepA;
	diagnosticStep.name = "invalid stderr";
	diagnosticStep.type = JobStepType::Process;
	diagnosticStage.steps.push_back(diagnosticStep);
	diagnosticJob.stages.push_back(diagnosticStage);
	JobRunnerDependencies diagnosticDependencies;
	diagnosticDependencies.runProcess = [](const ProcessSpec&, stop_token) {
		ProcessResult result;
		result.status = ProcessStatus::ExitedWithError;
		result.exitCode = 17;
		result.standardError.assign(300u * 1024u, 'x');
		result.standardError[0] = static_cast<char>(0xFF);
		result.outputTruncated = true;
		return result;
	};
	const auto diagnosticRun = JobRunner(std::move(diagnosticDependencies)).Run(diagnosticJob);
	const auto& diagnostic = diagnosticRun.stages.front().steps.front().diagnostics.front().detail;
	test.Expect(diagnostic.find("\xEF\xBF\xBD") != string::npos
			&& diagnostic.find("stderrUtf8Replaced=true") != string::npos
			&& diagnostic.find("outputTruncated=true") != string::npos
			&& diagnostic.size() < 270u * 1024u,
		"Job process diagnostics should replace invalid UTF-8 and stay within the diagnostic budget");
	test.Expect(nlohmann::json{{"detail", diagnostic}}.dump().find("\xEF\xBF\xBD")
		!= string::npos,
		"Sanitized process diagnostics should remain valid JSON strings");
	const auto validUnicode = SanitizeUtf8("\xE4\xB8\xAD\xF0\x9F\x98\x80", 16);
	test.Expect(validUnicode.value == "\xE4\xB8\xAD\xF0\x9F\x98\x80"
			&& !validUnicode.invalidUtf8Replaced && !validUnicode.truncated,
		"UTF-8 sanitizer should preserve valid three- and four-byte code points");

	const filesystem::path jobsPath = temporaryRoot / "jobs" / "jobs.json";
	wstring writeError;
	test.Expect(JobStorage::Save(jobsPath, parsed.document, writeError)
			&& JobStorage::Load(jobsPath).IsLoaded(),
		"jobs.json should use the shared atomic persistence boundary");
}
