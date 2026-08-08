#pragma once

#include "BackupService.h"
#include "OperationResult.h"
#include "ProcessRunner.h"
#include "SpecialTaskModels.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>

struct SpecialTaskPreflightResult {
	OperationCode code = OperationCode::Success;
	std::vector<Diagnostic> diagnostics;
};

struct SpecialTaskRunnerDependencies {
	std::function<std::optional<BackupRequest>(const SpecialTaskTarget&)> resolveBackup;
	std::function<SpecialTaskPreflightResult(const BackupRequest&)> preflightBackup;
	std::function<BackupResult(const BackupRequest&, std::stop_token)> runBackup;
	std::function<ProcessResult(const ShellTaskSpec&, std::stop_token)> runCommand;
};

class ISpecialTaskClock {
public:
	using TimePoint = std::chrono::system_clock::time_point;
	virtual ~ISpecialTaskClock() = default;
	virtual TimePoint Now() const = 0;
	virtual bool WaitUntil(TimePoint deadline, std::stop_token stopToken) = 0;
};

class SystemSpecialTaskClock final : public ISpecialTaskClock {
public:
	TimePoint Now() const override;
	bool WaitUntil(TimePoint deadline, std::stop_token stopToken) override;
};

class SpecialTaskRunner {
public:
	SpecialTaskRunner(
		SpecialTaskRunnerDependencies dependencies,
		std::shared_ptr<ISpecialTaskClock> clock = {});

	SpecialRunResult Run(
		const SpecialConfig& config,
		std::stop_token stopToken = {}) const;

	static std::optional<ISpecialTaskClock::TimePoint> NextScheduledTime(
		const SpecialTaskTrigger& trigger,
		ISpecialTaskClock::TimePoint now);

private:
	SpecialTaskRunnerDependencies dependencies_;
	std::shared_ptr<ISpecialTaskClock> clock_;
};

