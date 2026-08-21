#pragma once

#include "BackupService.h"
#include "JobModels.h"

#include <functional>
#include <optional>
#include <stop_token>

struct JobPreflightResult {
	OperationCode code = OperationCode::Success;
	std::vector<Diagnostic> diagnostics;
};

struct JobRunnerDependencies {
	std::function<std::optional<BackupRequest>(const JobBackupTarget&)> resolveBackup;
	std::function<JobPreflightResult(const BackupRequest&)> preflightBackup;
	std::function<BackupResult(const BackupRequest&, std::stop_token)> runBackup;
	std::function<ProcessResult(const ProcessSpec&, std::stop_token)> runProcess;
};

class JobRunner {
public:
	explicit JobRunner(JobRunnerDependencies dependencies);

	JobRunResult Run(const Job& job, std::stop_token stopToken = {}) const;

private:
	JobStepResult RunStep(const JobStep& step, std::stop_token stopToken) const;
	JobRunnerDependencies dependencies_;
};
