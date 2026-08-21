#include "JobRunner.h"

#include "text_to_text.h"

#include <thread>

using namespace std;

namespace {

constexpr size_t kMaximumProcessDiagnosticBytes = 256u * 1024u;

OperationCode Aggregate(const vector<JobStepResult>& steps) {
	bool success = false;
	bool changed = false;
	bool failure = false;
	for (const auto& step : steps) {
		if (step.code == OperationCode::Cancelled) return OperationCode::Cancelled;
		if (IsSuccessful(step.code)) {
			success = true;
			changed |= step.code != OperationCode::NoChanges;
		}
		else {
			failure = true;
		}
	}
	if (success && failure) return OperationCode::PartialSuccess;
	if (failure) return OperationCode::JobFailed;
	return changed ? OperationCode::Success : OperationCode::NoChanges;
}

Diagnostic ProcessDiagnostic(const JobStep& step, const ProcessResult& process) {
	string detail = "exitCode=" + to_string(process.exitCode);
	if (!process.error.empty()) detail += ";error=" + wstring_to_utf8(process.error);
	const auto sanitizedStderr = SanitizeUtf8(
		process.standardError, kMaximumProcessDiagnosticBytes);
	if (!sanitizedStderr.value.empty()) detail += ";stderr=" + sanitizedStderr.value;
	if (sanitizedStderr.invalidUtf8Replaced) detail += ";stderrUtf8Replaced=true";
	if (sanitizedStderr.truncated || process.outputTruncated) detail += ";outputTruncated=true";
	return {
		process.status == ProcessStatus::Succeeded ? "job.process.completed"
			: process.status == ProcessStatus::Cancelled ? "job.step.cancelled"
			: process.status == ProcessStatus::TimedOut ? "job.process.timed_out"
			: "job.process.failed",
		process.status == ProcessStatus::Succeeded ? DiagnosticSeverity::Info
			: process.status == ProcessStatus::Cancelled ? DiagnosticSeverity::Warning
			: DiagnosticSeverity::Error,
		wstring_to_utf8(step.stepId) + ";" + detail};
}

JobStepResult WorkerException(const JobStep& step, const char* message) {
	JobStepResult result;
	result.stepId = step.stepId;
	result.code = OperationCode::JobFailed;
	const auto sanitized = SanitizeUtf8(
		message ? string(message) : string{}, kMaximumProcessDiagnosticBytes);
	string detail = wstring_to_utf8(step.stepId) + ";exception=" + sanitized.value;
	if (sanitized.invalidUtf8Replaced) detail += ";exceptionUtf8Replaced=true";
	if (sanitized.truncated) detail += ";exceptionTruncated=true";
	result.diagnostics.push_back({
		"job.step.exception", DiagnosticSeverity::Error, std::move(detail)});
	return result;
}

} // namespace

JobRunner::JobRunner(JobRunnerDependencies dependencies)
	: dependencies_(std::move(dependencies)) {}

JobStepResult JobRunner::RunStep(const JobStep& step, stop_token stopToken) const {
	JobStepResult result;
	result.stepId = step.stepId;
	if (stopToken.stop_requested()) {
		result.code = OperationCode::Cancelled;
		result.diagnostics.push_back({"job.step.cancelled", DiagnosticSeverity::Warning,
			wstring_to_utf8(step.stepId)});
		return result;
	}
	if (step.type == JobStepType::Process) {
		const ProcessResult process = dependencies_.runProcess
			? dependencies_.runProcess(step.process, stopToken)
			: ProcessRunner::Run(step.process, stopToken);
		result.code = process.status == ProcessStatus::Succeeded
			? OperationCode::Success
			: process.status == ProcessStatus::Cancelled
				? OperationCode::Cancelled : OperationCode::JobFailed;
		result.diagnostics.push_back(ProcessDiagnostic(step, process));
		return result;
	}
	if (!dependencies_.resolveBackup || !dependencies_.runBackup) {
		result.code = OperationCode::InvalidProfile;
		result.diagnostics.push_back({"job.backup.runtime_missing",
			DiagnosticSeverity::Error, wstring_to_utf8(step.stepId)});
		return result;
	}
	auto request = dependencies_.resolveBackup(step.backup);
	if (!request) {
		result.code = OperationCode::TargetNotFound;
		result.diagnostics.push_back({"job.target.not_found",
			DiagnosticSeverity::Error, wstring_to_utf8(step.stepId)});
		return result;
	}
	request->comment = step.backup.comment;
	if (dependencies_.preflightBackup) {
		auto preflight = dependencies_.preflightBackup(*request);
		if (!IsSuccessful(preflight.code)) {
			result.code = preflight.code;
			result.diagnostics = std::move(preflight.diagnostics);
			return result;
		}
	}
	auto backup = dependencies_.runBackup(*request, stopToken);
	result.code = backup.code;
	result.diagnostics = std::move(backup.diagnostics);
	return result;
}

JobRunResult JobRunner::Run(const Job& job, stop_token stopToken) const {
	JobRunResult result;
	result.jobId = job.jobId;
	bool skip = false;
	vector<JobStepResult> executed;
	for (const auto& stage : job.stages) {
		JobStageResult stageResult;
		stageResult.stageId = stage.stageId;
		if (skip) {
			stageResult.skipped = true;
			stageResult.code = OperationCode::JobFailed;
			result.stages.push_back(std::move(stageResult));
			continue;
		}
		if (stopToken.stop_requested()) {
			stageResult.code = OperationCode::Cancelled;
			result.stages.push_back(std::move(stageResult));
			skip = true;
			continue;
		}
		stageResult.steps.resize(stage.steps.size());
		vector<jthread> workers;
		workers.reserve(stage.steps.size());
		for (size_t index = 0; index < stage.steps.size(); ++index) {
			workers.emplace_back([&, index](stop_token) {
				// 异常不能越过 jthread 入口；否则一个失败步骤会直接 std::terminate 整个 serve 进程。
				try {
					stageResult.steps[index] = RunStep(stage.steps[index], stopToken);
				}
				catch (const exception& exception) {
					stageResult.steps[index] = WorkerException(
						stage.steps[index], exception.what());
				}
				catch (...) {
					stageResult.steps[index] = WorkerException(
						stage.steps[index], "unknown exception");
				}
			});
		}
		for (auto& worker : workers) {
			if (worker.joinable()) worker.join();
		}
		stageResult.code = Aggregate(stageResult.steps);
		executed.insert(executed.end(), stageResult.steps.begin(), stageResult.steps.end());
		skip = !IsSuccessful(stageResult.code);
		result.stages.push_back(std::move(stageResult));
	}
	result.code = Aggregate(executed);
	if (stopToken.stop_requested()) result.code = OperationCode::Cancelled;
	if (executed.empty() && result.code != OperationCode::Cancelled) {
		result.code = OperationCode::JobFailed;
		result.diagnostics.push_back({"job.no_steps_executed", DiagnosticSeverity::Error,
			wstring_to_utf8(job.jobId)});
	}
	return result;
}
