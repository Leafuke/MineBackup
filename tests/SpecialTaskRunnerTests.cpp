#include "SpecialTaskRunnerTests.h"

#include "SpecialTaskRunner.h"

#include <atomic>
#include <mutex>

using namespace std;

namespace {

class ImmediateClock final : public ISpecialTaskClock {
public:
	explicit ImmediateClock(TimePoint now) : now_(now) {}
	TimePoint Now() const override { return now_; }
	bool WaitUntil(TimePoint deadline, stop_token stopToken) override {
		++waitCount;
		lastDeadline = deadline;
		return stopToken.stop_requested();
	}

	TimePoint now_;
	TimePoint lastDeadline{};
	atomic<int> waitCount{0};
};

SpecialTask BackupTask(wstring id, SpecialTaskTriggerType trigger) {
	SpecialTask task;
	task.taskId = std::move(id);
	task.name = "backup";
	task.type = SpecialTaskType::Backup;
	task.target = {L"config", L"world"};
	task.trigger.type = trigger;
	return task;
}

optional<BackupRequest> ResolveFixture(const SpecialTaskTarget& target) {
	if (target.configId != L"config" || target.worldPath != L"world") return nullopt;
	BackupRequest request;
	request.config.configId = target.configId;
	request.world = {target.configId, target.worldPath};
	request.sourcePath = L"world";
	return request;
}

} // namespace

void RunSpecialTaskRunnerTests(TestContext& test) {
	atomic<int> backupRuns{0};
	SpecialTaskRunnerDependencies dependencies;
	dependencies.resolveBackup = ResolveFixture;
	dependencies.runBackup = [&](const BackupRequest&, stop_token) {
		++backupRuns;
		BackupResult result;
		result.code = OperationCode::Success;
		result.outcome = BackupOutcome::Created;
		return result;
	};
	dependencies.runCommand = [](const ShellTaskSpec&, stop_token) {
		ProcessResult result;
		result.status = ProcessStatus::ExitedWithError;
		result.exitCode = 7;
		return result;
	};

	SpecialConfig mixed;
	mixed.specialTasks.push_back(BackupTask(L"backup-once", SpecialTaskTriggerType::Once));
	SpecialTask command;
	command.taskId = L"command-once";
	command.name = "command";
	command.type = SpecialTaskType::Command;
	command.command = L"exit 7";
	mixed.specialTasks.push_back(command);
	SpecialTaskRunner mixedRunner(dependencies);
	const SpecialRunResult mixedResult = mixedRunner.Run(mixed);
	test.Expect(mixedResult.code == OperationCode::PartialSuccess
			&& mixedResult.tasks.size() == 2
			&& backupRuns == 1,
		"SpecialTaskRunner should aggregate mixed one-shot outcomes as partial success");

	SpecialConfig scripted;
	SpecialTask script;
	script.taskId = L"script";
	script.name = "script";
	script.type = SpecialTaskType::Script;
	script.command = L"preserved";
	scripted.specialTasks.push_back(script);
	const SpecialRunResult unsupported = mixedRunner.Run(scripted);
	test.Expect(unsupported.code == OperationCode::InvalidProfile
			&& backupRuns == 1
			&& !unsupported.diagnostics.empty()
			&& unsupported.diagnostics.front().eventId == "special.script.unsupported",
		"Script preflight should reject the complete run before executing any task");

	stop_source recurringStop;
	const auto fixedNow = chrono::system_clock::now();
	auto clock = make_shared<ImmediateClock>(fixedNow);
	SpecialTaskRunnerDependencies recurringDependencies = dependencies;
	recurringDependencies.runBackup = [&](const BackupRequest&, stop_token) {
		++backupRuns;
		recurringStop.request_stop();
		BackupResult result;
		result.code = OperationCode::Success;
		result.outcome = BackupOutcome::Created;
		return result;
	};
	SpecialConfig recurring;
	auto interval = BackupTask(L"interval", SpecialTaskTriggerType::Interval);
	interval.trigger.intervalMinutes = 15;
	recurring.specialTasks.push_back(interval);
	SpecialTaskRunner recurringRunner(recurringDependencies, clock);
	const SpecialRunResult recurringResult = recurringRunner.Run(
		recurring, recurringStop.get_token());
	test.Expect(clock->waitCount == 1
			&& clock->lastDeadline - fixedNow == chrono::minutes(15)
			&& backupRuns == 2
			&& recurringResult.code == OperationCode::Cancelled,
		"Interval tasks should wait one complete interval before their first execution");

	SpecialTaskTrigger scheduled;
	scheduled.type = SpecialTaskTriggerType::Scheduled;
	scheduled.month = 0;
	scheduled.day = 0;
	scheduled.hour = 23;
	scheduled.minute = 59;
	const auto next = SpecialTaskRunner::NextScheduledTime(scheduled, fixedNow);
	test.Expect(next.has_value() && *next > fixedNow
			&& *next - fixedNow <= chrono::hours(24),
		"Wildcard scheduled tasks should select the next matching local time");
}

