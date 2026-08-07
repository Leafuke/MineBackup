#pragma once

#include <string>
#include <vector>

enum class SpecialTaskType {
	Backup,
	Command,
	Script
};

enum class SpecialTaskExecutionMode {
	Sequential,
	Parallel
};

enum class SpecialTaskTriggerType {
	Once,
	Interval,
	Scheduled
};

struct SpecialTaskTrigger {
	SpecialTaskTriggerType type = SpecialTaskTriggerType::Once;
	int intervalMinutes = 15;
	int month = 0;
	int day = 0;
	int hour = 0;
	int minute = 0;
};

struct SpecialTaskTarget {
	std::wstring configId;
	std::wstring worldPath;
};

struct SpecialTask {
	std::wstring taskId;
	std::string name;
	SpecialTaskType type = SpecialTaskType::Backup;
	SpecialTaskExecutionMode executionMode = SpecialTaskExecutionMode::Sequential;
	bool enabled = true;
	SpecialTaskTrigger trigger;
	SpecialTaskTarget target;
	std::wstring command;
	std::wstring workingDirectory;
};

struct SpecialTaskConfigDocument {
	std::wstring specialConfigId;
	std::vector<SpecialTask> tasks;
};

struct SpecialTaskDocument {
	static constexpr int SchemaVersion = 1;
	int schemaVersion = SchemaVersion;
	std::vector<SpecialTaskConfigDocument> specialConfigs;
};
