#include "SpecialTaskRunner.h"

#include "FolderRewindFormat.h"
#include "text_to_text.h"

#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>

using namespace std;

namespace {

tm LocalTime(time_t value) {
	tm result{};
#ifdef _WIN32
	localtime_s(&result, &value);
#else
	localtime_r(&value, &result);
#endif
	return result;
}

SpecialTaskResult MakeTaskResult(
	const SpecialTask& task,
	OperationCode code,
	string eventId,
	DiagnosticSeverity severity,
	string detail = {}) {
	SpecialTaskResult result;
	result.taskId = task.taskId;
	result.code = code;
	result.diagnostics.push_back({
		std::move(eventId), severity, std::move(detail)});
	return result;
}

} // namespace

ISpecialTaskClock::TimePoint SystemSpecialTaskClock::Now() const {
	return chrono::system_clock::now();
}

bool SystemSpecialTaskClock::WaitUntil(
	TimePoint deadline,
	stop_token stopToken) {
	mutex waitMutex;
	condition_variable_any changed;
	stop_callback callback(stopToken, [&] { changed.notify_all(); });
	unique_lock lock(waitMutex);
	while (!stopToken.stop_requested() && chrono::system_clock::now() < deadline) {
		changed.wait_until(lock, deadline);
	}
	return stopToken.stop_requested();
}

SpecialTaskRunner::SpecialTaskRunner(
	SpecialTaskRunnerDependencies dependencies,
	shared_ptr<ISpecialTaskClock> clock)
	: dependencies_(std::move(dependencies)),
	  clock_(clock ? std::move(clock) : make_shared<SystemSpecialTaskClock>()) {
}

optional<ISpecialTaskClock::TimePoint> SpecialTaskRunner::NextScheduledTime(
	const SpecialTaskTrigger& trigger,
	ISpecialTaskClock::TimePoint now) {
	if (trigger.type != SpecialTaskTriggerType::Scheduled
		|| trigger.month < 0 || trigger.month > 12
		|| trigger.day < 0 || trigger.day > 31
		|| trigger.hour < 0 || trigger.hour > 23
		|| trigger.minute < 0 || trigger.minute > 59) return nullopt;
	const time_t current = chrono::system_clock::to_time_t(now);
	const tm currentLocal = LocalTime(current);
	for (int offset = 0; offset <= 800; ++offset) {
		tm candidate = currentLocal;
		candidate.tm_mday += offset;
		candidate.tm_hour = trigger.hour;
		candidate.tm_min = trigger.minute;
		candidate.tm_sec = 0;
		candidate.tm_isdst = -1;
		const time_t candidateTime = mktime(&candidate);
		if (candidateTime == static_cast<time_t>(-1)) continue;
		const tm normalized = LocalTime(candidateTime);
		if (trigger.month != 0 && normalized.tm_mon + 1 != trigger.month) continue;
		if (trigger.day != 0 && normalized.tm_mday != trigger.day) continue;
		const auto point = chrono::system_clock::from_time_t(candidateTime);
		if (point > now) return point;
	}
	return nullopt;
}

SpecialRunResult SpecialTaskRunner::Run(
	const SpecialConfig& config,
	stop_token stopToken) const {
	SpecialRunResult run;
	vector<const SpecialTask*> enabled;
	map<wstring, BackupRequest> resolvedBackups;
	for (const auto& task : config.specialTasks) {
		if (!task.enabled) continue;
		enabled.push_back(&task);
		if (task.type == SpecialTaskType::Script) {
			run.code = OperationCode::InvalidProfile;
			run.diagnostics.push_back({
				"special.script.unsupported", DiagnosticSeverity::Error,
				"Script tasks are preserved but are not executable in schema v1."});
			return run;
		}
		if (task.type == SpecialTaskType::Command
			&& task.trigger.type != SpecialTaskTriggerType::Once) {
			run.code = OperationCode::InvalidProfile;
			run.diagnostics.push_back({
				"special.command.trigger_invalid", DiagnosticSeverity::Error,
				"Command tasks support only the once trigger."});
			return run;
		}
		if (task.type != SpecialTaskType::Backup) continue;
		if (!dependencies_.resolveBackup) {
			run.code = OperationCode::InvalidProfile;
			run.diagnostics.push_back({
				"special.target.resolver_missing", DiagnosticSeverity::Error, {}});
			return run;
		}
		auto request = dependencies_.resolveBackup(task.target);
		if (!request.has_value()) {
			run.code = OperationCode::TargetNotFound;
			run.diagnostics.push_back({
				"special.target.not_found", DiagnosticSeverity::Error,
				"Task target does not resolve to a configured world."});
			return run;
		}
		request->config.zipLevel = config.zipLevel;
		if (config.keepCount > 0) request->config.keepCount = config.keepCount;
		if (config.cpuThreads > 0) request->config.cpuThreads = config.cpuThreads;
		request->config.useLowPriority = config.useLowPriority;
		if (dependencies_.preflightBackup) {
			auto preflight = dependencies_.preflightBackup(*request);
			if (!IsSuccessful(preflight.code)) {
				run.code = preflight.code;
				run.diagnostics = std::move(preflight.diagnostics);
				return run;
			}
		}
		resolvedBackups.emplace(task.taskId, std::move(*request));
	}
	if (enabled.empty()) {
		run.code = OperationCode::Success;
		run.diagnostics.push_back({
			"special.no_enabled_tasks", DiagnosticSeverity::Info, {}});
		return run;
	}
	if (stopToken.stop_requested()) {
		run.code = OperationCode::Cancelled;
		run.diagnostics.push_back({
			"special.cancelled", DiagnosticSeverity::Warning, {}});
		return run;
	}

	mutex resultsMutex;
	auto record = [&](SpecialTaskResult result) {
		lock_guard lock(resultsMutex);
		run.tasks.push_back(std::move(result));
	};
	auto executeOnce = [&](const SpecialTask& task, stop_token token) {
		if (token.stop_requested()) {
			record(MakeTaskResult(
				task, OperationCode::Cancelled, "task.cancelled", DiagnosticSeverity::Warning));
			return;
		}
		if (task.type == SpecialTaskType::Command) {
			ShellTaskSpec spec;
			spec.command = task.command;
			spec.workingDirectory = task.workingDirectory;
			const ProcessResult process = dependencies_.runCommand
				? dependencies_.runCommand(spec, token)
				: ProcessRunner::RunShellTask(spec, token);
			const OperationCode code = process.status == ProcessStatus::Succeeded
				? OperationCode::Success
				: (process.status == ProcessStatus::Cancelled
					? OperationCode::Cancelled : OperationCode::TaskFailed);
			record(MakeTaskResult(
				task,
				code,
				code == OperationCode::Success ? "task.command.completed"
					: (code == OperationCode::Cancelled ? "task.cancelled" : "task.command.failed"),
				code == OperationCode::Success ? DiagnosticSeverity::Info
					: (code == OperationCode::Cancelled ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error),
				process.error.empty() ? process.standardError : wstring_to_utf8(process.error)));
			return;
		}
		const auto found = resolvedBackups.find(task.taskId);
		if (found == resolvedBackups.end() || !dependencies_.runBackup) {
			record(MakeTaskResult(
				task, OperationCode::InvalidProfile,
				"task.backup.runtime_missing", DiagnosticSeverity::Error));
			return;
		}
		BackupResult backup = dependencies_.runBackup(found->second, token);
		SpecialTaskResult result;
		result.taskId = task.taskId;
		result.code = backup.code;
		result.diagnostics = std::move(backup.diagnostics);
		record(std::move(result));
	};
	auto executeTask = [&](const SpecialTask& task, stop_token token) {
		if (task.trigger.type == SpecialTaskTriggerType::Once) {
			executeOnce(task, token);
			return;
		}
		while (!token.stop_requested()) {
			optional<ISpecialTaskClock::TimePoint> deadline;
			if (task.trigger.type == SpecialTaskTriggerType::Interval) {
				if (task.trigger.intervalMinutes <= 0) {
					record(MakeTaskResult(
						task, OperationCode::InvalidProfile,
						"task.interval.invalid", DiagnosticSeverity::Error));
					return;
				}
				deadline = clock_->Now() + chrono::minutes(task.trigger.intervalMinutes);
			}
			else {
				deadline = NextScheduledTime(task.trigger, clock_->Now());
				if (!deadline.has_value()) {
					record(MakeTaskResult(
						task, OperationCode::InvalidProfile,
						"task.schedule.invalid", DiagnosticSeverity::Error));
					return;
				}
			}
			if (clock_->WaitUntil(*deadline, token) || token.stop_requested()) break;
			executeOnce(task, token);
		}
	};

	vector<jthread> parallelOnce;
	vector<jthread> recurring;
	for (const SpecialTask* task : enabled) {
		if (task->trigger.type != SpecialTaskTriggerType::Once) {
			recurring.emplace_back([&, task](stop_token) {
				executeTask(*task, stopToken);
			});
		}
		else if (task->executionMode == SpecialTaskExecutionMode::Parallel) {
			parallelOnce.emplace_back([&, task](stop_token) {
				executeTask(*task, stopToken);
			});
		}
		else {
			for (auto& thread : parallelOnce) if (thread.joinable()) thread.join();
			parallelOnce.clear();
			executeTask(*task, stopToken);
		}
	}
	for (auto& thread : parallelOnce) if (thread.joinable()) thread.join();
	for (auto& thread : recurring) if (thread.joinable()) thread.join();
	map<wstring, size_t> taskOrder;
	for (size_t index = 0; index < enabled.size(); ++index) {
		taskOrder.emplace(enabled[index]->taskId, index);
	}
	stable_sort(run.tasks.begin(), run.tasks.end(), [&](const auto& left, const auto& right) {
		return taskOrder[left.taskId] < taskOrder[right.taskId];
	});

	if (stopToken.stop_requested() && !recurring.empty()) {
		run.code = OperationCode::Cancelled;
		run.diagnostics.push_back({
			"special.cancelled", DiagnosticSeverity::Warning, {}});
	}
	else {
		run.code = AggregateSpecialTaskCodes(run.tasks);
	}
	return run;
}
