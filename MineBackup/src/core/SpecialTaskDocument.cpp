#include "SpecialTaskDocument.h"

#include "AtomicFileWriter.h"
#include "FolderRewindFormat.h"
#include "text_to_text.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include "json.hpp"
#include <set>
#include <sstream>
#include <unordered_set>

using namespace std;

namespace SpecialTaskStorage {
namespace {

using nlohmann::json;

Diagnostic Fatal(
	string eventId,
	string detail,
	wstring specialConfigId = {},
	wstring taskId = {}) {
	return {DiagnosticSeverity::Fatal, std::move(eventId),
		std::move(specialConfigId), std::move(taskId), std::move(detail)};
}

Diagnostic Warning(
	string eventId,
	string detail,
	wstring specialConfigId = {},
	wstring taskId = {}) {
	return {DiagnosticSeverity::Warning, std::move(eventId),
		std::move(specialConfigId), std::move(taskId), std::move(detail)};
}

wstring Lower(wstring value) {
	transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
		return static_cast<wchar_t>(towlower(character));
	});
	return value;
}

const char* ToString(SpecialTaskType value) {
	switch (value) {
	case SpecialTaskType::Backup: return "backup";
	case SpecialTaskType::Command: return "command";
	case SpecialTaskType::Script: return "script";
	}
	return "backup";
}

const char* ToString(SpecialTaskExecutionMode value) {
	return value == SpecialTaskExecutionMode::Parallel ? "parallel" : "sequential";
}

const char* ToString(SpecialTaskTriggerType value) {
	switch (value) {
	case SpecialTaskTriggerType::Once: return "once";
	case SpecialTaskTriggerType::Interval: return "interval";
	case SpecialTaskTriggerType::Scheduled: return "scheduled";
	}
	return "once";
}

bool ParseType(const string& value, SpecialTaskType& result) {
	if (value == "backup") result = SpecialTaskType::Backup;
	else if (value == "command") result = SpecialTaskType::Command;
	else if (value == "script") result = SpecialTaskType::Script;
	else return false;
	return true;
}

bool ParseExecutionMode(const string& value, SpecialTaskExecutionMode& result) {
	if (value == "sequential") result = SpecialTaskExecutionMode::Sequential;
	else if (value == "parallel") result = SpecialTaskExecutionMode::Parallel;
	else return false;
	return true;
}

bool ParseTriggerType(const string& value, SpecialTaskTriggerType& result) {
	if (value == "once") result = SpecialTaskTriggerType::Once;
	else if (value == "interval") result = SpecialTaskTriggerType::Interval;
	else if (value == "scheduled") result = SpecialTaskTriggerType::Scheduled;
	else return false;
	return true;
}

bool ReadUtf8String(const json& object, const char* key, wstring& output) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_string()) return false;
	try {
		output = utf8_to_wstring(found->get<string>());
		return true;
	}
	catch (...) {
		return false;
	}
}

bool ReadString(const json& object, const char* key, string& output) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_string()) return false;
	output = found->get<string>();
	return true;
}

bool ReadInt(const json& object, const char* key, int minimum, int maximum, int& output) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_number_integer()) return false;
	try {
		const auto value = found->get<long long>();
		if (value < minimum || value > maximum) return false;
		output = static_cast<int>(value);
		return true;
	}
	catch (...) {
		return false;
	}
}

json SerializeTask(const SpecialTask& task) {
	json item;
	item["taskId"] = wstring_to_utf8(task.taskId);
	item["name"] = task.name;
	item["type"] = ToString(task.type);
	item["executionMode"] = ToString(task.executionMode);
	item["enabled"] = task.enabled;

	json trigger;
	trigger["type"] = ToString(task.trigger.type);
	if (task.trigger.type == SpecialTaskTriggerType::Interval) {
		trigger["intervalMinutes"] = task.trigger.intervalMinutes;
	}
	else if (task.trigger.type == SpecialTaskTriggerType::Scheduled) {
		trigger["month"] = task.trigger.month;
		trigger["day"] = task.trigger.day;
		trigger["hour"] = task.trigger.hour;
		trigger["minute"] = task.trigger.minute;
	}
	item["trigger"] = std::move(trigger);

	if (task.type == SpecialTaskType::Backup) {
		item["target"] = {
			{"configId", wstring_to_utf8(task.target.configId)},
			{"worldPath", wstring_to_utf8(task.target.worldPath)}};
	}
	else {
		item["command"] = wstring_to_utf8(task.command);
		item["workingDirectory"] = wstring_to_utf8(task.workingDirectory);
	}
	return item;
}

bool ParseTask(
	const json& item,
	const wstring& specialConfigId,
	SpecialTask& task,
	vector<Diagnostic>& diagnostics) {
	if (!item.is_object()) {
		diagnostics.push_back(Fatal("tasks.schema.invalid_task", "task must be an object", specialConfigId));
		return false;
	}
	string type;
	string executionMode;
	string triggerType;
	if (!ReadUtf8String(item, "taskId", task.taskId)
		|| !IsStableIdentifier(task.taskId)
		|| !ReadString(item, "name", task.name)
		|| !ReadString(item, "type", type)
		|| !ParseType(type, task.type)
		|| !ReadString(item, "executionMode", executionMode)
		|| !ParseExecutionMode(executionMode, task.executionMode)) {
		diagnostics.push_back(Fatal(
			"tasks.schema.invalid_task", "task identity or enum field is invalid",
			specialConfigId, task.taskId));
		return false;
	}
	const auto enabled = item.find("enabled");
	if (enabled == item.end() || !enabled->is_boolean()) {
		diagnostics.push_back(Fatal(
			"tasks.schema.invalid_task", "enabled must be a boolean",
			specialConfigId, task.taskId));
		return false;
	}
	task.enabled = enabled->get<bool>();

	const auto trigger = item.find("trigger");
	if (trigger == item.end() || !trigger->is_object()
		|| !ReadString(*trigger, "type", triggerType)
		|| !ParseTriggerType(triggerType, task.trigger.type)) {
		diagnostics.push_back(Fatal(
			"tasks.schema.invalid_trigger", "trigger is missing or invalid",
			specialConfigId, task.taskId));
		return false;
	}
	if (task.trigger.type == SpecialTaskTriggerType::Interval
		&& !ReadInt(*trigger, "intervalMinutes", 1, 525600, task.trigger.intervalMinutes)) {
		diagnostics.push_back(Fatal(
			"tasks.schema.invalid_trigger", "intervalMinutes is outside the supported range",
			specialConfigId, task.taskId));
		return false;
	}
	if (task.trigger.type == SpecialTaskTriggerType::Scheduled
		&& (!ReadInt(*trigger, "month", 0, 12, task.trigger.month)
			|| !ReadInt(*trigger, "day", 0, 31, task.trigger.day)
			|| !ReadInt(*trigger, "hour", 0, 23, task.trigger.hour)
			|| !ReadInt(*trigger, "minute", 0, 59, task.trigger.minute))) {
		diagnostics.push_back(Fatal(
			"tasks.schema.invalid_trigger", "scheduled trigger field is outside the supported range",
			specialConfigId, task.taskId));
		return false;
	}

	if (task.type == SpecialTaskType::Backup) {
		const auto target = item.find("target");
		wstring worldPath;
		if (target == item.end() || !target->is_object()
			|| !ReadUtf8String(*target, "configId", task.target.configId)
			|| task.target.configId.empty()
			|| !ReadUtf8String(*target, "worldPath", worldPath)
			|| !TryNormalizeWorldPath(worldPath, task.target.worldPath)) {
			diagnostics.push_back(Fatal(
				"tasks.schema.invalid_target", "backup target must use a stable ConfigId and safe relative worldPath",
				specialConfigId, task.taskId));
			return false;
		}
	}
	else {
		if (!ReadUtf8String(item, "command", task.command)
			|| !ReadUtf8String(item, "workingDirectory", task.workingDirectory)) {
			diagnostics.push_back(Fatal(
				"tasks.schema.invalid_command", "command fields must be strings",
				specialConfigId, task.taskId));
			return false;
		}
		if (task.type == SpecialTaskType::Command
			&& task.trigger.type != SpecialTaskTriggerType::Once) {
			diagnostics.push_back(Fatal(
				"tasks.schema.invalid_command_trigger", "command tasks only support the once trigger",
				specialConfigId, task.taskId));
			return false;
		}
	}
	return true;
}

bool FindLegacyTarget(
	const map<int, Config>& configs,
	int configIndex,
	int worldIndex,
	SpecialTaskTarget& target) {
	const auto config = configs.find(configIndex);
	if (config == configs.end() || config->second.configId.empty()
		|| worldIndex < 0
		|| worldIndex >= static_cast<int>(config->second.worlds.size())) {
		return false;
	}
	wstring normalized;
	if (!TryNormalizeWorldPath(config->second.worlds[worldIndex].first, normalized)) return false;
	target.configId = config->second.configId;
	target.worldPath = std::move(normalized);
	return true;
}

SpecialTaskTrigger LegacyTrigger(
	int trigger,
	int interval,
	int month,
	int day,
	int hour,
	int minute) {
	SpecialTaskTrigger result;
	result.type = trigger == 1 ? SpecialTaskTriggerType::Interval
		: trigger == 2 ? SpecialTaskTriggerType::Scheduled
		: SpecialTaskTriggerType::Once;
	result.intervalMinutes = interval;
	result.month = month;
	result.day = day;
	result.hour = hour;
	result.minute = minute;
	return result;
}

} // namespace

bool TryNormalizeWorldPath(const wstring& value, wstring& normalized) {
	if (value.empty()) return false;
	wstring separators = value;
	replace(separators.begin(), separators.end(), L'\\', L'/');
	filesystem::path path(separators);
	if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
	for (const auto& component : path) {
		if (component == L"..") return false;
	}
	path = path.lexically_normal();
	if (path.empty() || path == L".") return false;
	normalized = path.generic_wstring();
	return !normalized.empty() && normalized.front() != L'/';
}

bool IsStableIdentifier(const wstring& value) {
	if (value.size() != 36) return false;
	for (size_t index = 0; index < value.size(); ++index) {
		if (index == 8 || index == 13 || index == 18 || index == 23) {
			if (value[index] != L'-') return false;
		}
		else if (!iswxdigit(value[index])) {
			return false;
		}
	}
	return true;
}

string Serialize(const SpecialTaskDocument& document) {
	json root;
	root["schemaVersion"] = document.schemaVersion;
	root["specialConfigs"] = json::array();
	for (const auto& special : document.specialConfigs) {
		json item;
		item["specialConfigId"] = wstring_to_utf8(special.specialConfigId);
		item["tasks"] = json::array();
		for (const auto& task : special.tasks) item["tasks"].push_back(SerializeTask(task));
		root["specialConfigs"].push_back(std::move(item));
	}
	return root.dump(2);
}

LoadResult Parse(const string& content) {
	LoadResult result;
	const json root = json::parse(content, nullptr, false);
	if (root.is_discarded() || !root.is_object()) {
		result.status = LoadStatus::Invalid;
		result.diagnostics.push_back(Fatal("tasks.schema.invalid_json", "special task JSON cannot be parsed"));
		return result;
	}
	const auto version = root.find("schemaVersion");
	if (version == root.end() || !version->is_number_integer()) {
		result.status = LoadStatus::Invalid;
		result.diagnostics.push_back(Fatal("tasks.schema.missing_version", "schemaVersion must be an integer"));
		return result;
	}
	try {
		const auto schemaVersion = version->get<long long>();
		if (schemaVersion < 0 || schemaVersion > (numeric_limits<int>::max)()) {
			result.status = LoadStatus::UnsupportedSchema;
			result.diagnostics.push_back(Fatal("tasks.schema.unsupported", "special task schema is outside the supported range"));
			return result;
		}
		result.document.schemaVersion = static_cast<int>(schemaVersion);
	}
	catch (...) {
		result.status = LoadStatus::UnsupportedSchema;
		result.diagnostics.push_back(Fatal("tasks.schema.unsupported", "special task schema is outside the supported range"));
		return result;
	}
	if (result.document.schemaVersion > SpecialTaskDocument::SchemaVersion) {
		result.status = LoadStatus::UnsupportedSchema;
		result.diagnostics.push_back(Fatal("tasks.schema.unsupported", "special task schema is newer than this application"));
		return result;
	}
	if (result.document.schemaVersion != SpecialTaskDocument::SchemaVersion) {
		result.status = LoadStatus::Invalid;
		result.diagnostics.push_back(Fatal("tasks.schema.invalid_version", "special task schema version is not supported"));
		return result;
	}
	const auto configs = root.find("specialConfigs");
	if (configs == root.end() || !configs->is_array()) {
		result.status = LoadStatus::Invalid;
		result.diagnostics.push_back(Fatal("tasks.schema.invalid_configs", "specialConfigs must be an array"));
		return result;
	}

	set<wstring> specialIds;
	set<wstring> taskIds;
	for (const auto& item : *configs) {
		SpecialTaskConfigDocument special;
		if (!item.is_object()
			|| !ReadUtf8String(item, "specialConfigId", special.specialConfigId)
			|| special.specialConfigId.empty()) {
			result.diagnostics.push_back(Fatal("tasks.schema.invalid_special_config", "specialConfigId is missing"));
			continue;
		}
		if (!specialIds.insert(Lower(special.specialConfigId)).second) {
			result.diagnostics.push_back(Fatal(
				"tasks.schema.duplicate_special_config", "specialConfigId appears more than once",
				special.specialConfigId));
			continue;
		}
		const auto tasks = item.find("tasks");
		if (tasks == item.end() || !tasks->is_array()) {
			result.diagnostics.push_back(Fatal(
				"tasks.schema.invalid_tasks", "tasks must be an array", special.specialConfigId));
			continue;
		}
		for (const auto& taskItem : *tasks) {
			SpecialTask task;
			if (!ParseTask(taskItem, special.specialConfigId, task, result.diagnostics)) continue;
			if (!taskIds.insert(Lower(task.taskId)).second) {
				result.diagnostics.push_back(Fatal(
					"tasks.schema.duplicate_task_id", "taskId appears more than once",
					special.specialConfigId, task.taskId));
				continue;
			}
			special.tasks.push_back(std::move(task));
		}
		result.document.specialConfigs.push_back(std::move(special));
	}
	result.status = HasFatalDiagnostics(result.diagnostics) ? LoadStatus::Invalid : LoadStatus::Loaded;
	return result;
}

LoadResult Load(const filesystem::path& path) {
	LoadResult result;
	error_code existsError;
	if (!filesystem::exists(path, existsError)) {
		result.status = existsError ? LoadStatus::IoError : LoadStatus::Missing;
		if (existsError) result.diagnostics.push_back(Fatal("tasks.io.stat_failed", existsError.message()));
		return result;
	}
	ifstream input(path, ios::binary);
	if (!input) {
		result.status = LoadStatus::IoError;
		result.diagnostics.push_back(Fatal("tasks.io.read_failed", "special task document cannot be opened"));
		return result;
	}
	ostringstream content;
	content << input.rdbuf();
	result = Parse(content.str());
	return result;
}

bool Save(const filesystem::path& path, const SpecialTaskDocument& document, wstring& error) {
	try {
		const string content = Serialize(document);
		const auto validation = Parse(content);
		if (!validation.IsLoaded()) {
			error = L"special task document failed structural validation";
			return false;
		}
		const auto write = AtomicFileWriter::WriteText(path, content);
		error = write.error;
		return write.success;
	}
	catch (...) {
		error = L"special task document serialization failed";
		return false;
	}
}

MigrationResult MigrateLegacy(
	const map<int, Config>& configs,
	const map<int, SpecialConfig>& specialConfigs) {
	MigrationResult result;
	set<wstring> specialIds;
	for (const auto& [specialIndex, source] : specialConfigs) {
		(void)specialIndex;
		SpecialTaskConfigDocument targetConfig;
		targetConfig.specialConfigId = source.specialConfigId;
		if (targetConfig.specialConfigId.empty()
			|| !specialIds.insert(Lower(targetConfig.specialConfigId)).second) {
			result.diagnostics.push_back(Fatal(
				"tasks.migration.invalid_special_config_id",
				"special configuration has no unique stable identity",
				targetConfig.specialConfigId));
			continue;
		}

		for (size_t index = 0; index < source.commands.size(); ++index) {
			SpecialTask task;
			task.taskId = FolderRewindFormat::GenerateGuidString();
			task.name = "legacy-command-" + to_string(index + 1);
			task.type = SpecialTaskType::Command;
			task.command = source.commands[index];
			targetConfig.tasks.push_back(std::move(task));
		}

		if (!source.unifiedTasks.empty()) {
			vector<UnifiedTaskV2> ordered = source.unifiedTasks;
			stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
				return left.id < right.id;
			});
			set<int> oldIds;
			for (const auto& legacy : ordered) {
				if (!oldIds.insert(legacy.id).second) {
					result.diagnostics.push_back(Fatal(
						"tasks.migration.duplicate_legacy_id",
						"legacy UnifiedTask IDs must be unique",
						targetConfig.specialConfigId));
					continue;
				}
				SpecialTask task;
				task.taskId = FolderRewindFormat::GenerateGuidString();
				task.name = legacy.name;
				task.type = legacy.type == TaskTypeV2::Command ? SpecialTaskType::Command
					: legacy.type == TaskTypeV2::Script ? SpecialTaskType::Script
					: SpecialTaskType::Backup;
				task.executionMode = legacy.executionMode == TaskExecMode::Parallel
					? SpecialTaskExecutionMode::Parallel : SpecialTaskExecutionMode::Sequential;
				task.enabled = legacy.enabled;
				task.trigger = LegacyTrigger(
					static_cast<int>(legacy.triggerMode), legacy.intervalMinutes,
					legacy.schedMonth, legacy.schedDay, legacy.schedHour, legacy.schedMinute);
				if (task.type == SpecialTaskType::Backup) {
					if (!FindLegacyTarget(configs, legacy.configIndex, legacy.worldIndex, task.target)) {
						result.diagnostics.push_back(Fatal(
							"tasks.migration.invalid_target",
							"legacy UnifiedTask target index is missing or out of range",
							targetConfig.specialConfigId, task.taskId));
						continue;
					}
				}
				else {
					task.command = legacy.command;
					task.workingDirectory = legacy.workingDirectory;
					if (task.type == SpecialTaskType::Command
						&& task.trigger.type != SpecialTaskTriggerType::Once) {
						task.trigger.type = SpecialTaskTriggerType::Once;
						result.diagnostics.push_back(Warning(
							"tasks.migration.command_trigger_normalized",
							"legacy command trigger was normalized to once",
							targetConfig.specialConfigId, task.taskId));
					}
				}
				targetConfig.tasks.push_back(std::move(task));
			}
		}
		else {
			for (size_t index = 0; index < source.tasks.size(); ++index) {
				const auto& legacy = source.tasks[index];
				SpecialTask task;
				task.taskId = FolderRewindFormat::GenerateGuidString();
				task.name = "legacy-backup-" + to_string(index + 1);
				task.type = SpecialTaskType::Backup;
				task.trigger = LegacyTrigger(
					legacy.backupType, legacy.intervalMinutes,
					legacy.schedMonth, legacy.schedDay, legacy.schedHour, legacy.schedMinute);
				if (!FindLegacyTarget(configs, legacy.configIndex, legacy.worldIndex, task.target)) {
					result.diagnostics.push_back(Fatal(
						"tasks.migration.invalid_target",
						"legacy AutoBackupTask target index is missing or out of range",
						targetConfig.specialConfigId, task.taskId));
					continue;
				}
				targetConfig.tasks.push_back(std::move(task));
			}
		}
		result.document.specialConfigs.push_back(std::move(targetConfig));
	}
	result.success = !HasFatalDiagnostics(result.diagnostics);
	return result;
}

SpecialTaskDocument BuildDocument(const map<int, SpecialConfig>& specialConfigs) {
	SpecialTaskDocument document;
	for (const auto& [index, special] : specialConfigs) {
		(void)index;
		document.specialConfigs.push_back({special.specialConfigId, special.specialTasks});
	}
	return document;
}

bool ApplyAndValidate(
	const SpecialTaskDocument& document,
	const map<int, Config>& configs,
	map<int, SpecialConfig>& specialConfigs,
	vector<Diagnostic>& diagnostics) {
	map<wstring, const Config*> configsById;
	for (const auto& [index, config] : configs) {
		(void)index;
		if (config.configId.empty()
			|| !configsById.emplace(Lower(config.configId), &config).second) {
			diagnostics.push_back(Fatal(
				"tasks.profile.invalid_config_id", "normal ConfigId is missing or duplicated"));
		}
	}
	map<wstring, SpecialConfig*> specialById;
	for (auto& [index, special] : specialConfigs) {
		(void)index;
		if (special.specialConfigId.empty()
			|| !specialById.emplace(Lower(special.specialConfigId), &special).second) {
			diagnostics.push_back(Fatal(
				"tasks.profile.invalid_special_config_id", "SpecialConfigId is missing or duplicated"));
		}
	}

	map<wstring, vector<SpecialTask>> stagedTasks;
	for (const auto& source : document.specialConfigs) {
		const auto special = specialById.find(Lower(source.specialConfigId));
		if (special == specialById.end()) {
			diagnostics.push_back(Fatal(
				"tasks.profile.special_config_not_found",
				"special task document refers to an unknown SpecialConfigId",
				source.specialConfigId));
			continue;
		}
		vector<SpecialTask> resolvedTasks = source.tasks;
		for (auto& task : resolvedTasks) {
			if (task.type != SpecialTaskType::Backup) continue;
			const auto config = configsById.find(Lower(task.target.configId));
			if (config == configsById.end()) {
				diagnostics.push_back(Fatal(
					"tasks.profile.config_not_found",
					"backup task refers to an unknown ConfigId",
					source.specialConfigId, task.taskId));
				continue;
			}
			const bool worldExists = any_of(
				config->second->worlds.begin(), config->second->worlds.end(),
				[&](const auto& world) {
					wstring normalized;
					return TryNormalizeWorldPath(world.first, normalized)
						&& normalized == task.target.worldPath;
				});
			if (!worldExists) {
				diagnostics.push_back(Fatal(
					"tasks.profile.world_not_found",
					"backup task worldPath is not present in its configuration",
					source.specialConfigId, task.taskId));
			}
			else {
				task.target.configId = config->second->configId;
			}
		}
		stagedTasks.emplace(Lower(source.specialConfigId), std::move(resolvedTasks));
	}
	if (HasFatalDiagnostics(diagnostics)) return false;
	for (auto& [identity, special] : specialById) {
		const auto staged = stagedTasks.find(identity);
		special->specialTasks = staged != stagedTasks.end()
			? staged->second : vector<SpecialTask>{};
	}
	return true;
}

bool HasFatalDiagnostics(const vector<Diagnostic>& diagnostics) {
	return any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
		return diagnostic.severity == DiagnosticSeverity::Fatal;
	});
}

} // namespace SpecialTaskStorage
