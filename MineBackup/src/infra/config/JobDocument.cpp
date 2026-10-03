#include "JobDocument.h"

#include "AtomicFileWriter.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <set>

using namespace std;

namespace JobStorage {
namespace {

using nlohmann::json;

Diagnostic Error(string eventId, string detail) {
	return {std::move(eventId), DiagnosticSeverity::Error, std::move(detail)};
}

bool ReadUtf8(const json& object, const char* key, wstring& value) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_string()) return false;
	try {
		value = utf8_to_wstring(found->get<string>());
		return true;
	}
	catch (...) {
		return false;
	}
}

bool ReadString(const json& object, const char* key, string& value) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_string()) return false;
	value = found->get<string>();
	return true;
}

bool ReadInteger(
	const json& object,
	const char* key,
	long long minimum,
	long long maximum,
	long long& value,
	bool required = true) {
	const auto found = object.find(key);
	if (found == object.end()) return !required;
	if (!found->is_number_integer()) return false;
	try {
		value = found->get<long long>();
		return value >= minimum && value <= maximum;
	}
	catch (...) {
		return false;
	}
}

bool HasOnly(const json& object, initializer_list<const char*> allowed) {
	set<string> keys;
	for (const char* key : allowed) keys.emplace(key);
	for (auto item = object.begin(); item != object.end(); ++item) {
		if (!keys.contains(item.key())) return false;
	}
	return true;
}

json SerializeStep(const JobStep& step) {
	json value{
		{"stepId", wstring_to_utf8(step.stepId)},
		{"name", step.name},
		{"type", step.type == JobStepType::Backup ? "backup" : "process"}};
	if (step.type == JobStepType::Backup) {
		value["target"] = {
			{"configId", wstring_to_utf8(step.backup.configId)},
			{"worldPath", wstring_to_utf8(step.backup.worldPath)}};
		if (!step.backup.comment.empty()) {
			value["comment"] = wstring_to_utf8(step.backup.comment);
		}
	}
	else {
		value["executable"] = wstring_to_utf8(step.process.executable.wstring());
		value["arguments"] = json::array();
		for (const auto& argument : step.process.arguments) {
			value["arguments"].push_back(wstring_to_utf8(argument));
		}
		value["workingDirectory"] = wstring_to_utf8(step.process.workingDirectory.wstring());
		value["timeoutSeconds"] = step.process.timeout.count() / 1000;
		value["maximumCapturedBytes"] = step.process.maximumCapturedBytes;
		value["lowPriority"] = step.process.useLowPriority;
	}
	return value;
}

bool ParseStep(const json& value, JobStep& step, vector<Diagnostic>& diagnostics) {
	string type;
	if (!value.is_object()
		|| !ReadUtf8(value, "stepId", step.stepId)
		|| !IsCanonicalUuid(step.stepId)
		|| !ReadString(value, "name", step.name)
		|| step.name.empty()
		|| !ReadString(value, "type", type)) {
		diagnostics.push_back(Error("job.schema.invalid_step",
			"Step identity, name or type is invalid."));
		return false;
	}
	if (type == "backup") {
		step.type = JobStepType::Backup;
		if (!HasOnly(value, {"stepId", "name", "type", "target", "comment"})) {
			diagnostics.push_back(Error("job.schema.unknown_step_field",
				wstring_to_utf8(step.stepId)));
			return false;
		}
		const auto target = value.find("target");
		wstring world;
		if (target == value.end() || !target->is_object()
			|| !HasOnly(*target, {"configId", "worldPath"})
			|| !ReadUtf8(*target, "configId", step.backup.configId)
			|| !IsCanonicalUuid(step.backup.configId)
			|| !ReadUtf8(*target, "worldPath", world)
			|| !TryNormalizeWorldPath(world, step.backup.worldPath)) {
			diagnostics.push_back(Error("job.schema.invalid_backup_target",
				wstring_to_utf8(step.stepId)));
			return false;
		}
		const auto comment = value.find("comment");
		if (comment != value.end() && !ReadUtf8(value, "comment", step.backup.comment)) {
			diagnostics.push_back(Error("job.schema.invalid_comment",
				wstring_to_utf8(step.stepId)));
			return false;
		}
		return true;
	}
	if (type != "process"
		|| !HasOnly(value, {"stepId", "name", "type", "executable", "arguments",
			"workingDirectory", "timeoutSeconds", "maximumCapturedBytes", "lowPriority"})) {
		diagnostics.push_back(Error("job.schema.invalid_step_type",
			wstring_to_utf8(step.stepId)));
		return false;
	}
	step.type = JobStepType::Process;
	wstring executable;
	wstring workingDirectory;
	if (!ReadUtf8(value, "executable", executable) || executable.empty()) {
		diagnostics.push_back(Error("job.schema.invalid_executable",
			wstring_to_utf8(step.stepId)));
		return false;
	}
	const auto arguments = value.find("arguments");
	if (arguments == value.end() || !arguments->is_array()) {
		diagnostics.push_back(Error("job.schema.invalid_arguments",
			wstring_to_utf8(step.stepId)));
		return false;
	}
	for (const auto& argument : *arguments) {
		if (!argument.is_string()) {
			diagnostics.push_back(Error("job.schema.invalid_arguments",
				wstring_to_utf8(step.stepId)));
			return false;
		}
		step.process.arguments.push_back(utf8_to_wstring(argument.get<string>()));
	}
	const auto directory = value.find("workingDirectory");
	if (directory != value.end()
		&& !ReadUtf8(value, "workingDirectory", workingDirectory)) {
		diagnostics.push_back(Error("job.schema.invalid_working_directory",
			wstring_to_utf8(step.stepId)));
		return false;
	}
	long long timeout = 0;
	long long maximumOutput = 4ll * 1024ll * 1024ll;
	if (!ReadInteger(value, "timeoutSeconds", 0, 86400, timeout, false)
		|| !ReadInteger(value, "maximumCapturedBytes", 1024,
			64ll * 1024ll * 1024ll, maximumOutput, false)) {
		diagnostics.push_back(Error("job.schema.invalid_process_limit",
			wstring_to_utf8(step.stepId)));
		return false;
	}
	bool lowPriority = false;
	const auto priority = value.find("lowPriority");
	if (priority != value.end()) {
		if (!priority->is_boolean()) {
			diagnostics.push_back(Error("job.schema.invalid_process_priority",
				wstring_to_utf8(step.stepId)));
			return false;
		}
		lowPriority = priority->get<bool>();
	}
	step.process.executable = executable;
	step.process.workingDirectory = workingDirectory;
	step.process.timeout = chrono::seconds(timeout);
	step.process.maximumCapturedBytes = static_cast<size_t>(maximumOutput);
	step.process.useLowPriority = lowPriority;
	return true;
}

} // namespace

bool IsCanonicalUuid(const wstring& value) noexcept {
	if (value.size() != 36) return false;
	for (size_t index = 0; index < value.size(); ++index) {
		if (index == 8 || index == 13 || index == 18 || index == 23) {
			if (value[index] != L'-') return false;
			continue;
		}
		const wchar_t character = value[index];
		if (!((character >= L'0' && character <= L'9')
			|| (character >= L'a' && character <= L'f'))) return false;
	}
	return true;
}

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

string Serialize(const JobDocument& document) {
	json root{{"schemaVersion", document.schemaVersion}, {"jobs", json::array()}};
	for (const auto& job : document.jobs) {
		json value{{"jobId", wstring_to_utf8(job.jobId)}, {"name", job.name},
			{"stages", json::array()}};
		for (const auto& stage : job.stages) {
			json stageValue{{"stageId", wstring_to_utf8(stage.stageId)},
				{"name", stage.name}, {"steps", json::array()}};
			for (const auto& step : stage.steps) {
				stageValue["steps"].push_back(SerializeStep(step));
			}
			value["stages"].push_back(std::move(stageValue));
		}
		root["jobs"].push_back(std::move(value));
	}
	return root.dump(2);
}

LoadResult Parse(const string& content) {
	LoadResult result;
	const json root = json::parse(content, nullptr, false);
	if (root.is_discarded() || !root.is_object()
		|| !HasOnly(root, {"schemaVersion", "jobs"})) {
		result.status = LoadStatus::Invalid;
		result.diagnostics.push_back(Error("job.schema.invalid_json",
			"jobs.json must be a strict JSON object."));
		return result;
	}
	long long schemaVersion = 0;
	if (!ReadInteger(root, "schemaVersion", 0,
			(numeric_limits<int>::max)(), schemaVersion)) {
		result.status = LoadStatus::Invalid;
		result.diagnostics.push_back(Error("job.schema.invalid_version",
			"schemaVersion must be an integer."));
		return result;
	}
	if (schemaVersion > JobDocument::SchemaVersion) {
		result.status = LoadStatus::UnsupportedSchema;
		result.diagnostics.push_back(Error("job.schema.unsupported",
			"jobs.json is newer than this application."));
		return result;
	}
	if (schemaVersion != JobDocument::SchemaVersion) {
		result.status = LoadStatus::Invalid;
		result.diagnostics.push_back(Error("job.schema.invalid_version",
			"Only jobs schema v1 is supported."));
		return result;
	}
	result.document.schemaVersion = static_cast<int>(schemaVersion);
	const auto jobs = root.find("jobs");
	if (jobs == root.end() || !jobs->is_array()) {
		result.status = LoadStatus::Invalid;
		result.diagnostics.push_back(Error("job.schema.invalid_jobs", "jobs must be an array."));
		return result;
	}
	set<wstring> allIds;
	for (const auto& jobValue : *jobs) {
		Job job;
		if (!jobValue.is_object()
			|| !HasOnly(jobValue, {"jobId", "name", "stages"})
			|| !ReadUtf8(jobValue, "jobId", job.jobId)
			|| !IsCanonicalUuid(job.jobId)
			|| !ReadString(jobValue, "name", job.name)
			|| job.name.empty()
			|| !allIds.insert(job.jobId).second) {
			result.diagnostics.push_back(Error("job.schema.invalid_job",
				"Job identity, name or fields are invalid."));
			continue;
		}
		const auto stages = jobValue.find("stages");
		if (stages == jobValue.end() || !stages->is_array() || stages->empty()) {
			result.diagnostics.push_back(Error("job.schema.invalid_stages",
				wstring_to_utf8(job.jobId)));
			continue;
		}
		for (const auto& stageValue : *stages) {
			JobStage stage;
			if (!stageValue.is_object()
				|| !HasOnly(stageValue, {"stageId", "name", "steps"})
				|| !ReadUtf8(stageValue, "stageId", stage.stageId)
				|| !IsCanonicalUuid(stage.stageId)
				|| !ReadString(stageValue, "name", stage.name)
				|| stage.name.empty()
				|| !allIds.insert(stage.stageId).second) {
				result.diagnostics.push_back(Error("job.schema.invalid_stage",
					wstring_to_utf8(job.jobId)));
				continue;
			}
			const auto steps = stageValue.find("steps");
			if (steps == stageValue.end() || !steps->is_array() || steps->empty()) {
				result.diagnostics.push_back(Error("job.schema.invalid_steps",
					wstring_to_utf8(stage.stageId)));
				continue;
			}
			for (const auto& stepValue : *steps) {
				JobStep step;
				if (!ParseStep(stepValue, step, result.diagnostics)) continue;
				if (!allIds.insert(step.stepId).second) {
					result.diagnostics.push_back(Error("job.schema.duplicate_id",
						wstring_to_utf8(step.stepId)));
					continue;
				}
				stage.steps.push_back(std::move(step));
			}
			if (!stage.steps.empty()) job.stages.push_back(std::move(stage));
		}
		if (!job.stages.empty()) result.document.jobs.push_back(std::move(job));
	}
	result.status = result.diagnostics.empty() ? LoadStatus::Loaded : LoadStatus::Invalid;
	return result;
}

LoadResult Load(const filesystem::path& path) {
	LoadResult result;
	error_code error;
	if (!filesystem::exists(path, error) || error) {
		result.status = LoadStatus::Missing;
		return result;
	}
	ifstream input(path, ios::binary);
	if (!input.is_open()) {
		result.status = LoadStatus::IoError;
		result.diagnostics.push_back(Error("job.document.read_failed",
			wstring_to_utf8(path.wstring())));
		return result;
	}
	const string content((istreambuf_iterator<char>(input)), {});
	return Parse(content);
}

bool Save(const filesystem::path& path, const JobDocument& document, wstring& error) {
	const auto write = AtomicFileWriter::WriteText(path, Serialize(document) + "\n");
	error = write.error;
	return write.success;
}

bool ValidateReferences(
	const JobDocument& document,
	const map<int, Config>& configs,
	vector<Diagnostic>& diagnostics) {
	bool valid = true;
	for (const auto& job : document.jobs) {
		for (const auto& stage : job.stages) {
			for (const auto& step : stage.steps) {
				if (step.type != JobStepType::Backup) continue;
				const auto config = find_if(configs.begin(), configs.end(), [&](const auto& item) {
					return item.second.configId == step.backup.configId;
				});
				if (config == configs.end()
					|| none_of(config->second.worlds.begin(), config->second.worlds.end(),
						[&](const auto& world) { return world.first == step.backup.worldPath; })) {
					valid = false;
					diagnostics.push_back(Error("job.target.not_found",
						wstring_to_utf8(job.jobId + L":" + step.stepId)));
				}
			}
		}
	}
	return valid;
}

const Job* Find(const JobDocument& document, const wstring& jobId) noexcept {
	const auto found = find_if(document.jobs.begin(), document.jobs.end(),
		[&](const Job& job) { return job.jobId == jobId; });
	return found == document.jobs.end() ? nullptr : &*found;
}

} // namespace JobStorage
