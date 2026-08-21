#pragma once

#include "OperationResult.h"
#include "ProcessRunner.h"

#include <filesystem>
#include <string>
#include <vector>

enum class JobStepType {
	Backup,
	Process
};

struct JobBackupTarget {
	std::wstring configId;
	std::wstring worldPath;
	std::wstring comment;
};

struct JobStep {
	std::wstring stepId;
	std::string name;
	JobStepType type = JobStepType::Backup;
	JobBackupTarget backup;
	ProcessSpec process;
};

struct JobStage {
	std::wstring stageId;
	std::string name;
	std::vector<JobStep> steps;
};

struct Job {
	std::wstring jobId;
	std::string name;
	std::vector<JobStage> stages;
};

struct JobDocument {
	static constexpr int SchemaVersion = 1;
	int schemaVersion = SchemaVersion;
	std::vector<Job> jobs;
};

struct JobStepResult {
	std::wstring stepId;
	OperationCode code = OperationCode::JobFailed;
	std::vector<Diagnostic> diagnostics;
};

struct JobStageResult {
	std::wstring stageId;
	OperationCode code = OperationCode::JobFailed;
	bool skipped = false;
	std::vector<JobStepResult> steps;
};

struct JobRunResult {
	std::wstring jobId;
	OperationCode code = OperationCode::JobFailed;
	std::vector<JobStageResult> stages;
	std::vector<Diagnostic> diagnostics;
};
