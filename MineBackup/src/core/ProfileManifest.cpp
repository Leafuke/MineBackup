#include "ProfileManifest.h"

#include "AtomicFileWriter.h"
#include "FolderRewindFormat.h"
#include "HistoryRepository.h"
#include "JobDocument.h"
#include "ProfileConfigRepository.h"
#include "WorldIdentity.h"
#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>

using namespace std;

namespace ProfileManifest {
namespace {

using nlohmann::json;

constexpr wchar_t TransactionName[] = L".profile-apply-transaction.json";
constexpr wchar_t ConfigRollbackName[] = L".profile-apply-config.rollback";
constexpr wchar_t JobsRollbackName[] = L".profile-apply-jobs.rollback";

Diagnostic Error(string eventId, string detail) {
	return {std::move(eventId), DiagnosticSeverity::Error, std::move(detail)};
}

Diagnostic Warning(string eventId, string detail) {
	return {std::move(eventId), DiagnosticSeverity::Warning, std::move(detail)};
}

bool HasOnly(const json& object, initializer_list<const char*> allowed) {
	set<string> keys;
	for (const char* key : allowed) keys.emplace(key);
	for (auto item = object.begin(); item != object.end(); ++item) {
		if (!keys.contains(item.key())) return false;
	}
	return true;
}

bool ReadString(const json& object, const char* key, string& value, bool required = true) {
	const auto found = object.find(key);
	if (found == object.end()) return !required;
	if (!found->is_string()) return false;
	value = found->get<string>();
	return true;
}

bool ReadUtf8(const json& object, const char* key, wstring& value, bool required = true) {
	string text;
	if (!ReadString(object, key, text, required)) return false;
	if (!required && object.find(key) == object.end()) return true;
	try {
		value = utf8_to_wstring(text);
		return true;
	}
	catch (...) {
		return false;
	}
}

bool ReadBoolean(const json& object, const char* key, bool& value, bool required = true) {
	const auto found = object.find(key);
	if (found == object.end()) return !required;
	if (!found->is_boolean()) return false;
	value = found->get<bool>();
	return true;
}

bool ReadInteger(
	const json& object,
	const char* key,
	long long minimum,
	long long maximum,
	int& value,
	bool required = true) {
	const auto found = object.find(key);
	if (found == object.end()) return !required;
	if (!found->is_number_integer()) return false;
	try {
		const long long parsed = found->get<long long>();
		if (parsed < minimum || parsed > maximum) return false;
		value = static_cast<int>(parsed);
		return true;
	}
	catch (...) {
		return false;
	}
}

filesystem::path ResolvePath(const filesystem::path& directory, const wstring& text) {
	if (text.empty()) return {};
	filesystem::path value(text);
	if (value.is_relative()) value = directory / value;
	error_code error;
	const auto absolute = filesystem::absolute(value, error);
	return (error ? value : absolute).lexically_normal();
}

bool ParseStringArray(
	const json& object,
	const char* key,
	vector<wstring>& values) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_array()) return false;
	for (const auto& item : *found) {
		if (!item.is_string()) return false;
		try {
			values.push_back(utf8_to_wstring(item.get<string>()));
		}
		catch (...) {
			return false;
		}
	}
	return true;
}

bool ParseConfig(
	const json& value,
	const filesystem::path& directory,
	Config& config,
	vector<Diagnostic>& diagnostics) {
	if (!value.is_object()
		|| !HasOnly(value, {"configId", "name", "saveRoot", "backupRoot", "worlds",
			"backup", "archive", "retention", "restore", "exclude", "cloud"})
		|| !ReadUtf8(value, "configId", config.configId)
		|| !JobStorage::IsCanonicalUuid(config.configId)
		|| !ReadString(value, "name", config.name)
		|| config.name.empty()) {
		diagnostics.push_back(Error("manifest.config.invalid_identity",
			"Config requires a canonical UUID, non-empty name and known fields."));
		return false;
	}
	wstring saveRoot;
	wstring backupRoot;
	if (!ReadUtf8(value, "saveRoot", saveRoot) || saveRoot.empty()
		|| !ReadUtf8(value, "backupRoot", backupRoot) || backupRoot.empty()) {
		diagnostics.push_back(Error("manifest.config.invalid_path",
			wstring_to_utf8(config.configId)));
		return false;
	}
	config.saveRoot = ResolvePath(directory, saveRoot).wstring();
	config.backupPath = ResolvePath(directory, backupRoot).wstring();
	const auto worlds = value.find("worlds");
	if (worlds == value.end() || !worlds->is_array() || worlds->empty()) {
		diagnostics.push_back(Error("manifest.config.invalid_worlds",
			wstring_to_utf8(config.configId)));
		return false;
	}
	for (const auto& worldValue : *worlds) {
		wstring path;
		wstring normalized;
		wstring description;
		if (!worldValue.is_object()
			|| !HasOnly(worldValue, {"path", "description"})
			|| !ReadUtf8(worldValue, "path", path)
			|| !JobStorage::TryNormalizeWorldPath(path, normalized)
			|| !ReadUtf8(worldValue, "description", description)) {
			diagnostics.push_back(Error("manifest.config.invalid_world",
				wstring_to_utf8(config.configId)));
			return false;
		}
		if (any_of(config.worlds.begin(), config.worlds.end(), [&](const auto& world) {
			return world.first == normalized;
		})) {
			diagnostics.push_back(Error("manifest.config.duplicate_world",
				wstring_to_utf8(config.configId + L":" + normalized)));
			return false;
		}
		config.worlds.emplace_back(std::move(normalized), std::move(description));
	}

	if (const auto backup = value.find("backup"); backup != value.end()) {
		string mode;
		if (!backup->is_object()
			|| !HasOnly(*backup, {"mode", "skipIfUnchanged", "maxSmartBackupsPerFull"})
			|| !ReadString(*backup, "mode", mode)
			|| !ReadBoolean(*backup, "skipIfUnchanged", config.skipIfUnchanged)
			|| !ReadInteger(*backup, "maxSmartBackupsPerFull", 0, 100000,
				config.maxSmartBackupsPerFull)) {
			diagnostics.push_back(Error("manifest.config.invalid_backup",
				wstring_to_utf8(config.configId)));
			return false;
		}
		if (mode == "full") config.backupMode = 1;
		else if (mode == "smart") config.backupMode = 2;
		else if (mode == "overwrite") config.backupMode = 3;
		else {
			diagnostics.push_back(Error("manifest.config.invalid_backup_mode", mode));
			return false;
		}
	}

	if (const auto archive = value.find("archive"); archive != value.end()) {
		wstring tool;
		if (!archive->is_object()
			|| !HasOnly(*archive, {"tool", "format", "method", "level", "threads", "lowPriority"})
			|| !ReadUtf8(*archive, "tool", tool)
			|| !ReadUtf8(*archive, "format", config.zipFormat)
			|| config.zipFormat.empty()
			|| !ReadUtf8(*archive, "method", config.zipMethod)
			|| config.zipMethod.empty()
			|| !ReadInteger(*archive, "level", 0, 22, config.zipLevel)
			|| !ReadInteger(*archive, "threads", 0, 1024, config.cpuThreads)
			|| !ReadBoolean(*archive, "lowPriority", config.useLowPriority)) {
			diagnostics.push_back(Error("manifest.config.invalid_archive",
				wstring_to_utf8(config.configId)));
			return false;
		}
		config.zipPath = ResolvePath(directory, tool).wstring();
	}

	if (const auto retention = value.find("retention"); retention != value.end()) {
		if (!retention->is_object() || !HasOnly(*retention, {"keepCount"})
			|| !ReadInteger(*retention, "keepCount", 0, 100000, config.keepCount)) {
			diagnostics.push_back(Error("manifest.config.invalid_retention",
				wstring_to_utf8(config.configId)));
			return false;
		}
	}
	if (const auto restore = value.find("restore"); restore != value.end()) {
		if (!restore->is_object() || !HasOnly(*restore, {"backupBefore"})
			|| !ReadBoolean(*restore, "backupBefore", config.backupBefore)) {
			diagnostics.push_back(Error("manifest.config.invalid_restore",
				wstring_to_utf8(config.configId)));
			return false;
		}
	}
	if (value.contains("exclude") && !ParseStringArray(value, "exclude", config.blacklist)) {
		diagnostics.push_back(Error("manifest.config.invalid_exclude",
			wstring_to_utf8(config.configId)));
		return false;
	}
	if (const auto cloud = value.find("cloud"); cloud != value.end()) {
		wstring rclone;
		string mode;
		wstring workingDirectory;
		if (!cloud->is_object()
			|| !HasOnly(*cloud, {"enabled", "rclone", "remote", "mode",
				"workingDirectory", "timeoutSeconds", "retryCount", "syncHistoryAfterUpload"})
			|| !ReadBoolean(*cloud, "enabled", config.cloudSyncEnabled)
			|| !ReadUtf8(*cloud, "rclone", rclone)
			|| !ReadUtf8(*cloud, "remote", config.rcloneRemotePath)
			|| !ReadString(*cloud, "mode", mode)
			|| !ReadUtf8(*cloud, "workingDirectory", workingDirectory)
			|| !ReadInteger(*cloud, "timeoutSeconds", 1, 86400,
				config.cloudTimeoutSeconds)
			|| !ReadInteger(*cloud, "retryCount", 0, 100, config.cloudRetryCount)
			|| !ReadBoolean(*cloud, "syncHistoryAfterUpload",
				config.cloudSyncHistoryAfterUpload)) {
			diagnostics.push_back(Error("manifest.config.invalid_cloud",
				wstring_to_utf8(config.configId)));
			return false;
		}
		if (mode == "history-only") config.cloudSyncMode = 0;
		else if (mode == "history-and-backups") config.cloudSyncMode = 1;
		else {
			diagnostics.push_back(Error("manifest.config.invalid_cloud_mode", mode));
			return false;
		}
		config.rclonePath = ResolvePath(directory, rclone).wstring();
		config.cloudWorkingDirectory = ResolvePath(directory, workingDirectory).wstring();
	}
	return true;
}

json ConfigJson(const Config& config) {
	const char* backupMode = config.backupMode == 2 ? "smart"
		: config.backupMode == 3 ? "overwrite" : "full";
	json worlds = json::array();
	for (const auto& [path, description] : config.worlds) {
		worlds.push_back({{"path", wstring_to_utf8(path)},
			{"description", wstring_to_utf8(description)}});
	}
	json exclude = json::array();
	for (const auto& item : config.blacklist) exclude.push_back(wstring_to_utf8(item));
	return {
		{"configId", wstring_to_utf8(config.configId)},
		{"name", config.name},
		{"saveRoot", wstring_to_utf8(config.saveRoot)},
		{"backupRoot", wstring_to_utf8(config.backupPath)},
		{"worlds", std::move(worlds)},
		{"backup", {
			{"mode", backupMode},
			{"skipIfUnchanged", config.skipIfUnchanged},
			{"maxSmartBackupsPerFull", config.maxSmartBackupsPerFull}}},
		{"archive", {
			{"tool", wstring_to_utf8(config.zipPath)},
			{"format", wstring_to_utf8(config.zipFormat)},
			{"method", wstring_to_utf8(config.zipMethod)},
			{"level", config.zipLevel},
			{"threads", config.cpuThreads},
			{"lowPriority", config.useLowPriority}}},
		{"retention", {{"keepCount", config.keepCount}}},
		{"restore", {{"backupBefore", config.backupBefore}}},
		{"exclude", std::move(exclude)},
		{"cloud", {
			{"enabled", config.cloudSyncEnabled},
			{"rclone", wstring_to_utf8(config.rclonePath)},
			{"remote", wstring_to_utf8(config.rcloneRemotePath)},
			{"mode", config.cloudSyncMode == 1 ? "history-and-backups" : "history-only"},
			{"workingDirectory", wstring_to_utf8(config.cloudWorkingDirectory)},
			{"timeoutSeconds", config.cloudTimeoutSeconds},
			{"retryCount", config.cloudRetryCount},
			{"syncHistoryAfterUpload", config.cloudSyncHistoryAfterUpload}}}
	};
}

bool OwnedConfigEqual(const Config& left, const Config& right) {
	return ConfigJson(left) == ConfigJson(right);
}

Config MergeOwnedConfig(const Config& incoming, const Config* existing) {
	Config merged = existing ? *existing : Config{};
	merged.configId = incoming.configId;
	merged.name = incoming.name;
	merged.saveRoot = incoming.saveRoot;
	merged.backupPath = incoming.backupPath;
	merged.worlds = incoming.worlds;
	merged.backupMode = incoming.backupMode;
	merged.skipIfUnchanged = incoming.skipIfUnchanged;
	merged.maxSmartBackupsPerFull = incoming.maxSmartBackupsPerFull;
	merged.zipPath = incoming.zipPath;
	merged.zipFormat = incoming.zipFormat;
	merged.zipMethod = incoming.zipMethod;
	merged.zipLevel = incoming.zipLevel;
	merged.cpuThreads = incoming.cpuThreads;
	merged.useLowPriority = incoming.useLowPriority;
	merged.keepCount = incoming.keepCount;
	merged.backupBefore = incoming.backupBefore;
	merged.blacklist = incoming.blacklist;
	merged.cloudSyncEnabled = incoming.cloudSyncEnabled;
	merged.rclonePath = incoming.rclonePath;
	merged.rcloneRemotePath = incoming.rcloneRemotePath;
	merged.cloudSyncMode = incoming.cloudSyncMode;
	merged.cloudWorkingDirectory = incoming.cloudWorkingDirectory;
	merged.cloudTimeoutSeconds = incoming.cloudTimeoutSeconds;
	merged.cloudRetryCount = incoming.cloudRetryCount;
	merged.cloudSyncHistoryAfterUpload = incoming.cloudSyncHistoryAfterUpload;
	merged.pendingLocalBinding = false;
	merged.legacyConfigIdGenerated = false;
	return merged;
}

bool JobsEqual(const Job& left, const Job& right) {
	JobDocument leftDocument;
	leftDocument.jobs.push_back(left);
	JobDocument rightDocument;
	rightDocument.jobs.push_back(right);
	return JobStorage::Serialize(leftDocument) == JobStorage::Serialize(rightDocument);
}

string ReadFile(const filesystem::path& path) {
	ifstream input(path, ios::binary);
	return input.is_open()
		? string((istreambuf_iterator<char>(input)), istreambuf_iterator<char>())
		: string{};
}

void RemoveTransactionFiles(const AppPaths& paths) {
	error_code ignored;
	filesystem::remove(paths.configRoot / TransactionName, ignored);
	filesystem::remove(paths.configRoot / ConfigRollbackName, ignored);
	filesystem::remove(paths.configRoot / JobsRollbackName, ignored);
}

bool RestoreSnapshot(
	const filesystem::path& target,
	const filesystem::path& snapshot,
	bool originallyExisted,
	vector<Diagnostic>& diagnostics) {
	if (!originallyExisted) {
		error_code error;
		filesystem::remove(target, error);
		if (error) {
			diagnostics.push_back(Error("profile.transaction.rollback_failed",
				wstring_to_utf8(target.wstring())));
			return false;
		}
		return true;
	}
	const auto write = AtomicFileWriter::WriteText(target, ReadFile(snapshot),
		{false, true});
	if (!write.success) {
		diagnostics.push_back(Error("profile.transaction.rollback_failed",
			wstring_to_utf8(write.error)));
	}
	return write.success;
}

bool RecoverTransaction(const AppPaths& paths, vector<Diagnostic>& diagnostics) {
	const filesystem::path journalPath = paths.configRoot / TransactionName;
	error_code existsError;
	if (!filesystem::exists(journalPath, existsError) || existsError) return !existsError;
	const json journal = json::parse(ReadFile(journalPath), nullptr, false);
	if (journal.is_discarded() || !journal.is_object()) {
		diagnostics.push_back(Error("profile.transaction.invalid_journal",
			wstring_to_utf8(journalPath.wstring())));
		return false;
	}
	if (journal.value("phase", string{}) == "committed") {
		RemoveTransactionFiles(paths);
		diagnostics.push_back(Warning("profile.transaction.cleanup_recovered", {}));
		return true;
	}
	const bool configExisted = journal.value("configExisted", false);
	const bool jobsExisted = journal.value("jobsExisted", false);
	const bool configRestored = RestoreSnapshot(paths.ConfigFile(),
		paths.configRoot / ConfigRollbackName, configExisted, diagnostics);
	const bool jobsRestored = RestoreSnapshot(paths.JobsFile(),
		paths.configRoot / JobsRollbackName, jobsExisted, diagnostics);
	if (configRestored && jobsRestored) {
		RemoveTransactionFiles(paths);
		diagnostics.push_back(Warning("profile.transaction.rollback_recovered", {}));
		return true;
	}
	return false;
}

size_t HistoryCount(
	HistoryRepository& history,
	const wstring& configId) {
	return history.EntriesForConfig(configId)->size();
}

} // namespace

ServerProfileManifest CreateTemplate() {
	ServerProfileManifest manifest;
	manifest.restorePreserve = {
		L"session.lock", L"xaeromap.txt", L"soul_archive.json", L"voxy",
		L"DistantHorizons.sqlite", L"DistantHorizons.sqlite-shm",
		L"DistantHorizons.sqlite-wal"};
	Config config;
	config.configId = FolderRewindFormat::GenerateGuidString();
	config.name = "Minecraft Server";
	config.saveRoot = L"server";
	config.backupPath = L"backups";
	config.worlds = {{L"world", L"Primary world"}};
	config.backupMode = 2;
	config.blacklist = {L"session.lock"};
	manifest.configs.push_back(config);
	Job job;
	job.jobId = FolderRewindFormat::GenerateGuidString();
	job.name = "Backup all worlds";
	JobStage stage;
	stage.stageId = FolderRewindFormat::GenerateGuidString();
	stage.name = "Backup";
	JobStep step;
	step.stepId = FolderRewindFormat::GenerateGuidString();
	step.name = "Backup primary world";
	step.type = JobStepType::Backup;
	step.backup.configId = config.configId;
	step.backup.worldPath = config.worlds.front().first;
	stage.steps.push_back(std::move(step));
	job.stages.push_back(std::move(stage));
	manifest.jobs.jobs.push_back(std::move(job));
	return manifest;
}

ProfileManifestLoadResult Parse(
	const string& content,
	const filesystem::path& manifestDirectory) {
	ProfileManifestLoadResult result;
	const json root = json::parse(content, nullptr, false);
	if (root.is_discarded() || !root.is_object()
		|| !HasOnly(root, {"schemaVersion", "profile", "configs", "jobs"})) {
		result.status = ProfileManifestStatus::Invalid;
		result.diagnostics.push_back(Error("manifest.schema.invalid_json",
			"Manifest must be a strict JSON object."));
		return result;
	}
	int version = 0;
	if (!ReadInteger(root, "schemaVersion", 0, (numeric_limits<int>::max)(), version)) {
		result.status = ProfileManifestStatus::Invalid;
		result.diagnostics.push_back(Error("manifest.schema.invalid_version", {}));
		return result;
	}
	if (version > ServerProfileManifest::SchemaVersion) {
		result.status = ProfileManifestStatus::UnsupportedSchema;
		result.diagnostics.push_back(Error("manifest.schema.unsupported", to_string(version)));
		return result;
	}
	if (version != ServerProfileManifest::SchemaVersion) {
		result.status = ProfileManifestStatus::Invalid;
		result.diagnostics.push_back(Error("manifest.schema.invalid_version", to_string(version)));
		return result;
	}
	result.manifest.schemaVersion = version;
	const auto profile = root.find("profile");
	if (profile == root.end() || !profile->is_object()
		|| !HasOnly(*profile, {"restorePreserve"})
		|| !ParseStringArray(*profile, "restorePreserve", result.manifest.restorePreserve)) {
		result.status = ProfileManifestStatus::Invalid;
		result.diagnostics.push_back(Error("manifest.schema.invalid_profile", {}));
		return result;
	}
	const auto configs = root.find("configs");
	if (configs == root.end() || !configs->is_array()) {
		result.status = ProfileManifestStatus::Invalid;
		result.diagnostics.push_back(Error("manifest.schema.invalid_configs", {}));
		return result;
	}
	set<wstring> configIds;
	for (const auto& value : *configs) {
		Config config;
		if (!ParseConfig(value, manifestDirectory, config, result.diagnostics)) continue;
		if (!configIds.insert(config.configId).second) {
			result.diagnostics.push_back(Error("manifest.config.duplicate_id",
				wstring_to_utf8(config.configId)));
			continue;
		}
		result.manifest.configs.push_back(std::move(config));
	}
	{
		map<int, Config> parsedConfigs;
		for (size_t index = 0; index < result.manifest.configs.size(); ++index) {
			parsedConfigs.emplace(static_cast<int>(index + 1), result.manifest.configs[index]);
		}
		for (const auto& conflict : WorldIdentity::FindStorageConflicts(parsedConfigs)) {
			result.diagnostics.push_back(Error("manifest.config.storage_collision",
				wstring_to_utf8(conflict.backupRoot + L":"
					+ conflict.storageFolderName + L" ("
					+ conflict.leftConfigId + L":" + conflict.leftWorldPath + L", "
					+ conflict.rightConfigId + L":" + conflict.rightWorldPath + L")")));
		}
	}
	const auto jobs = root.find("jobs");
	if (jobs == root.end() || !jobs->is_array()) {
		result.diagnostics.push_back(Error("manifest.schema.invalid_jobs", {}));
	}
	else {
		const auto loadedJobs = JobStorage::Parse(json{
			{"schemaVersion", JobDocument::SchemaVersion}, {"jobs", *jobs}}.dump());
		result.diagnostics.insert(result.diagnostics.end(),
			loadedJobs.diagnostics.begin(), loadedJobs.diagnostics.end());
		if (loadedJobs.IsLoaded()) {
			result.manifest.jobs = loadedJobs.document;
			for (auto& job : result.manifest.jobs.jobs) {
				for (auto& stage : job.stages) {
					for (auto& step : stage.steps) {
						if (step.type != JobStepType::Process) continue;
						step.process.executable = ResolvePath(
							manifestDirectory, step.process.executable.wstring());
						if (!step.process.workingDirectory.empty()) {
							step.process.workingDirectory = ResolvePath(
								manifestDirectory, step.process.workingDirectory.wstring());
						}
					}
				}
			}
		}
	}
	if (!JobStorage::ValidateReferences(
			result.manifest.jobs,
			[&] {
				map<int, Config> map;
				for (size_t index = 0; index < result.manifest.configs.size(); ++index) {
					map.emplace(static_cast<int>(index + 1), result.manifest.configs[index]);
				}
				return map;
			}(),
			result.diagnostics)) {
		// Diagnostics already identify every dangling target.
	}
	result.status = any_of(result.diagnostics.begin(), result.diagnostics.end(),
		[](const Diagnostic& item) { return item.severity == DiagnosticSeverity::Error; })
		? ProfileManifestStatus::Invalid : ProfileManifestStatus::Loaded;
	return result;
}

ProfileManifestLoadResult Load(const filesystem::path& path) {
	ProfileManifestLoadResult result;
	error_code error;
	if (!filesystem::exists(path, error) || error) {
		result.status = ProfileManifestStatus::Missing;
		result.diagnostics.push_back(Error("manifest.file.missing",
			wstring_to_utf8(path.wstring())));
		return result;
	}
	ifstream input(path, ios::binary);
	if (!input.is_open()) {
		result.status = ProfileManifestStatus::IoError;
		result.diagnostics.push_back(Error("manifest.file.read_failed",
			wstring_to_utf8(path.wstring())));
		return result;
	}
	const string content((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
	const auto absolute = filesystem::absolute(path, error);
	return Parse(content, (error ? path : absolute).parent_path());
}

string Serialize(const ServerProfileManifest& manifest) {
	json root{{"schemaVersion", manifest.schemaVersion},
		{"profile", {{"restorePreserve", json::array()}}},
		{"configs", json::array()}, {"jobs", json::array()}};
	for (const auto& item : manifest.restorePreserve) {
		root["profile"]["restorePreserve"].push_back(wstring_to_utf8(item));
	}
	for (const auto& config : manifest.configs) root["configs"].push_back(ConfigJson(config));
	const json jobs = json::parse(JobStorage::Serialize(manifest.jobs));
	root["jobs"] = jobs.at("jobs");
	return root.dump(2);
}

ProfileApplyPlan Plan(
	const AppPaths& paths,
	const ServerProfileManifest& manifest,
	bool prune) {
	ProfileApplyPlan plan;
	if (!RecoverTransaction(paths, plan.diagnostics)) return plan;
	ProfileConfigRepository repository(paths.ConfigFile());
	const auto current = repository.Load();
	if (!current.IsUsable()) {
		plan.diagnostics.insert(plan.diagnostics.end(),
			current.diagnostics.begin(), current.diagnostics.end());
		return plan;
	}
	auto currentJobs = JobStorage::Load(paths.JobsFile());
	if (currentJobs.status == JobStorage::LoadStatus::Missing) {
		currentJobs.status = JobStorage::LoadStatus::Loaded;
		currentJobs.document = {};
	}
	if (!currentJobs.IsLoaded()) {
		plan.diagnostics.insert(plan.diagnostics.end(),
			currentJobs.diagnostics.begin(), currentJobs.diagnostics.end());
		return plan;
	}

	plan.configs = prune ? map<int, Config>{} : current.configs;
	int maximumIndex = 0;
	map<wstring, int> currentIndices;
	for (const auto& [index, config] : current.configs) {
		maximumIndex = max(maximumIndex, index);
		currentIndices[config.configId] = index;
	}
	set<wstring> manifestConfigIds;
	for (const auto& incoming : manifest.configs) {
		manifestConfigIds.insert(incoming.configId);
		const auto existingIndex = currentIndices.find(incoming.configId);
		if (existingIndex == currentIndices.end()) {
			plan.configs[++maximumIndex] = MergeOwnedConfig(incoming, nullptr);
			plan.diff.push_back({"config", incoming.configId, ProfileDiffAction::Add});
		}
		else {
			const Config& existing = current.configs.at(existingIndex->second);
			const Config merged = MergeOwnedConfig(incoming, &existing);
			plan.configs[existingIndex->second] = merged;
			if (!OwnedConfigEqual(existing, merged)) {
				plan.diff.push_back({"config", incoming.configId, ProfileDiffAction::Update});
			}
		}
	}

	HistoryRepository history;
	if (filesystem::exists(paths.HistoryFile())) {
		history.Load(paths.HistoryFile(), current.configs);
	}
	if (prune) {
		for (const auto& [index, config] : current.configs) {
			(void)index;
			if (!manifestConfigIds.contains(config.configId)) {
				plan.diff.push_back({"config", config.configId,
					ProfileDiffAction::Remove, HistoryCount(history, config.configId)});
			}
		}
	}

	plan.jobs = prune ? JobDocument{} : currentJobs.document;
	set<wstring> manifestJobIds;
	for (const auto& incoming : manifest.jobs.jobs) {
		manifestJobIds.insert(incoming.jobId);
		auto existing = find_if(plan.jobs.jobs.begin(), plan.jobs.jobs.end(),
			[&](const Job& job) { return job.jobId == incoming.jobId; });
		if (existing == plan.jobs.jobs.end()) {
			plan.jobs.jobs.push_back(incoming);
			plan.diff.push_back({"job", incoming.jobId, ProfileDiffAction::Add});
		}
		else if (!JobsEqual(*existing, incoming)) {
			*existing = incoming;
			plan.diff.push_back({"job", incoming.jobId, ProfileDiffAction::Update});
		}
	}
	if (prune) {
		for (const auto& currentJob : currentJobs.document.jobs) {
			if (!manifestJobIds.contains(currentJob.jobId)) {
				plan.diff.push_back({"job", currentJob.jobId, ProfileDiffAction::Remove});
			}
		}
	}
	for (const auto& conflict : WorldIdentity::FindStorageConflicts(plan.configs)) {
		plan.diagnostics.push_back(Error("manifest.config.storage_collision",
			wstring_to_utf8(conflict.backupRoot + L":"
				+ conflict.storageFolderName + L" ("
				+ conflict.leftConfigId + L":" + conflict.leftWorldPath + L", "
				+ conflict.rightConfigId + L":" + conflict.rightWorldPath + L")")));
	}
	if (any_of(plan.diagnostics.begin(), plan.diagnostics.end(),
		[](const Diagnostic& item) {
			return item.eventId == "manifest.config.storage_collision"
				&& item.severity == DiagnosticSeverity::Error;
		})) return plan;
	plan.restorePreserve = manifest.restorePreserve;
	if (current.restorePreserve != manifest.restorePreserve) {
		plan.diff.push_back({"profile", L"restorePreserve", ProfileDiffAction::Update});
	}
	if (!JobStorage::ValidateReferences(plan.jobs, plan.configs, plan.diagnostics)) return plan;
	plan.code = OperationCode::Success;
	return plan;
}

ProfileApplyResult Apply(const AppPaths& paths, const ProfileApplyPlan& plan) {
	ProfileApplyResult result;
	result.diff = plan.diff;
	result.diagnostics = plan.diagnostics;
	if (!IsSuccessful(plan.code)) {
		result.code = plan.code;
		return result;
	}
	vector<Diagnostic> recoveryDiagnostics;
	if (!RecoverTransaction(paths, recoveryDiagnostics)) {
		result.code = OperationCode::InvalidProfile;
		result.diagnostics.insert(result.diagnostics.end(),
			recoveryDiagnostics.begin(), recoveryDiagnostics.end());
		return result;
	}
	const bool configExisted = filesystem::exists(paths.ConfigFile());
	const bool jobsExisted = filesystem::exists(paths.JobsFile());
	if (configExisted) {
		const auto snapshot = AtomicFileWriter::WriteText(
			paths.configRoot / ConfigRollbackName, ReadFile(paths.ConfigFile()), {false, true});
		if (!snapshot.success) {
			result.code = OperationCode::InvalidProfile;
			result.diagnostics.push_back(Error("profile.transaction.snapshot_failed",
				wstring_to_utf8(snapshot.error)));
			return result;
		}
	}
	if (jobsExisted) {
		const auto snapshot = AtomicFileWriter::WriteText(
			paths.configRoot / JobsRollbackName, ReadFile(paths.JobsFile()), {false, true});
		if (!snapshot.success) {
			result.code = OperationCode::InvalidProfile;
			result.diagnostics.push_back(Error("profile.transaction.snapshot_failed",
				wstring_to_utf8(snapshot.error)));
			RemoveTransactionFiles(paths);
			return result;
		}
	}
	json journal{{"schemaVersion", 1}, {"phase", "prepared"},
		{"configExisted", configExisted}, {"jobsExisted", jobsExisted}};
	const auto prepared = AtomicFileWriter::WriteText(
		paths.configRoot / TransactionName, journal.dump(), {false, true});
	if (!prepared.success) {
		result.code = OperationCode::InvalidProfile;
		result.diagnostics.push_back(Error("profile.transaction.prepare_failed",
			wstring_to_utf8(prepared.error)));
		RemoveTransactionFiles(paths);
		return result;
	}

	ProfileConfigRepository repository(paths.ConfigFile());
	const auto configWrite = repository.Save(plan.configs, plan.restorePreserve, true);
	wstring jobsError;
	const bool jobsWrite = configWrite.success
		&& JobStorage::Save(paths.JobsFile(), plan.jobs, jobsError);
	if (!configWrite.success || !jobsWrite) {
		result.diagnostics.insert(result.diagnostics.end(),
			configWrite.diagnostics.begin(), configWrite.diagnostics.end());
		if (!jobsWrite && !jobsError.empty()) {
			result.diagnostics.push_back(Error("job.document.write_failed",
				wstring_to_utf8(jobsError)));
		}
		vector<Diagnostic> rollbackDiagnostics;
		RecoverTransaction(paths, rollbackDiagnostics);
		result.diagnostics.insert(result.diagnostics.end(),
			rollbackDiagnostics.begin(), rollbackDiagnostics.end());
		result.code = OperationCode::InvalidProfile;
		return result;
	}
	journal["phase"] = "committed";
	const auto committed = AtomicFileWriter::WriteText(
		paths.configRoot / TransactionName, journal.dump(), {false, true});
	if (!committed.success) {
		result.code = OperationCode::InvalidProfile;
		result.diagnostics.push_back(Error("profile.transaction.commit_marker_failed",
			wstring_to_utf8(committed.error)));
		return result;
	}
	RemoveTransactionFiles(paths);
	result.code = OperationCode::Success;
	return result;
}

ProfileManifestLoadResult Export(const AppPaths& paths) {
	ProfileManifestLoadResult result;
	if (!RecoverTransaction(paths, result.diagnostics)) {
		result.status = ProfileManifestStatus::Invalid;
		return result;
	}
	const auto config = ProfileConfigRepository(paths.ConfigFile()).Load();
	if (!config.IsUsable() || config.status == ProfileCatalogStatus::Missing) {
		result.status = ProfileManifestStatus::Invalid;
		result.diagnostics.insert(result.diagnostics.end(),
			config.diagnostics.begin(), config.diagnostics.end());
		return result;
	}
	const auto jobs = JobStorage::Load(paths.JobsFile());
	if (jobs.status != JobStorage::LoadStatus::Missing && !jobs.IsLoaded()) {
		result.status = ProfileManifestStatus::Invalid;
		result.diagnostics.insert(result.diagnostics.end(),
			jobs.diagnostics.begin(), jobs.diagnostics.end());
		return result;
	}
	result.manifest.restorePreserve = config.restorePreserve;
	for (const auto& [index, value] : config.configs) {
		(void)index;
		result.manifest.configs.push_back(value);
	}
	if (jobs.IsLoaded()) result.manifest.jobs = jobs.document;
	result.status = ProfileManifestStatus::Loaded;
	return result;
}

const char* ToString(ProfileDiffAction action) noexcept {
	switch (action) {
	case ProfileDiffAction::Add: return "add";
	case ProfileDiffAction::Update: return "update";
	case ProfileDiffAction::Remove: return "remove";
	}
	return "update";
}

} // namespace ProfileManifest
