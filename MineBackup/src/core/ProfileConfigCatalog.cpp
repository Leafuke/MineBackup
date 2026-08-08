#include "ProfileConfigCatalog.h"

#include "LegacyIniConfigCodec.h"
#include "SpecialTaskDocument.h"
#include "text_to_text.h"

#include <fstream>
#include <limits>
#include <set>

using namespace std;

namespace {

void AddDiagnostic(
	ProfileCatalogLoadResult& result,
	string eventId,
	DiagnosticSeverity severity,
	string detail) {
	result.diagnostics.push_back({
		std::move(eventId), severity, std::move(detail)});
}

bool ReadInteger(
	ProfileCatalogLoadResult& result,
	const wstring& value,
	int minimum,
	int maximum,
	int& target,
	size_t line,
	const wstring& key) {
	int parsed = 0;
	if (LegacyIniConfigCodec::TryParseInt(value, minimum, maximum, parsed)) {
		target = parsed;
		return true;
	}
	AddDiagnostic(result, "config.parse.invalid_operational_value",
		DiagnosticSeverity::Error,
		"line=" + to_string(line) + " key=" + wstring_to_utf8(key));
	return false;
}

bool ReadBoolean(
	ProfileCatalogLoadResult& result,
	const wstring& value,
	bool& target,
	size_t line,
	const wstring& key) {
	if (value == L"0" || value == L"1") {
		target = value == L"1";
		return true;
	}
	AddDiagnostic(result, "config.parse.invalid_operational_value",
		DiagnosticSeverity::Error,
		"line=" + to_string(line) + " key=" + wstring_to_utf8(key));
	return false;
}

bool ParseAutomatedTask(
	const wstring& value,
	AutomatedTask& task) {
	const auto tokens = LegacyIniConfigCodec::Split(value, L',');
	if (tokens.size() != 8) return false;
	int values[8]{};
	const pair<int, int> ranges[8] = {
		{-1, (numeric_limits<int>::max)()}, {-1, (numeric_limits<int>::max)()},
		{0, 2}, {1, 525600}, {0, 12}, {0, 31}, {0, 23}, {0, 59}};
	for (size_t index = 0; index < 8; ++index) {
		if (!LegacyIniConfigCodec::TryParseInt(
				tokens[index], ranges[index].first, ranges[index].second, values[index])) {
			return false;
		}
	}
	task.configIndex = values[0];
	task.worldIndex = values[1];
	task.backupType = values[2];
	task.intervalMinutes = values[3];
	task.schedMonth = values[4];
	task.schedDay = values[5];
	task.schedHour = values[6];
	task.schedMinute = values[7];
	return true;
}

bool ParseUnifiedTask(const wstring& value, UnifiedTaskV2& task) {
	const auto tokens = LegacyIniConfigCodec::Split(value, L',');
	if (tokens.size() != 15) return false;
	int values[12]{};
	const size_t numericIndices[12] = {0, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14};
	const pair<int, int> ranges[12] = {
		{0, (numeric_limits<int>::max)()}, {0, 2}, {0, 1}, {0, 2},
		{0, 1}, {-1, (numeric_limits<int>::max)()}, {-1, (numeric_limits<int>::max)()},
		{1, 525600}, {0, 12}, {0, 31}, {0, 23}, {0, 59}};
	for (size_t index = 0; index < 12; ++index) {
		if (!LegacyIniConfigCodec::TryParseInt(
				tokens[numericIndices[index]], ranges[index].first,
				ranges[index].second, values[index])) return false;
	}
	task.id = values[0];
	task.name = wstring_to_utf8(tokens[1]);
	task.type = static_cast<TaskTypeV2>(values[1]);
	task.executionMode = static_cast<TaskExecMode>(values[2]);
	task.triggerMode = static_cast<TaskTrigger>(values[3]);
	task.enabled = values[4] != 0;
	task.configIndex = values[5];
	task.worldIndex = values[6];
	task.command = tokens[8];
	task.workingDirectory = tokens[9];
	task.intervalMinutes = values[7];
	task.schedMonth = values[8];
	task.schedDay = values[9];
	task.schedHour = values[10];
	task.schedMinute = values[11];
	return true;
}

} // namespace

const Config* ProfileConfigCatalog::FindConfig(const wstring& configId) const {
	for (const auto& [index, config] : configs) {
		(void)index;
		if (config.configId == configId) return &config;
	}
	return nullptr;
}

const SpecialConfig* ProfileConfigCatalog::FindSpecialConfig(
	const wstring& specialConfigId) const {
	for (const auto& [index, config] : specialConfigs) {
		(void)index;
		if (config.specialConfigId == specialConfigId) return &config;
	}
	return nullptr;
}

ProfileCatalogLoadResult ProfileConfigCatalogLoader::Load(
	const filesystem::path& configFile,
	const filesystem::path& specialTasksFile) {
	ProfileCatalogLoadResult result;
	ifstream input(configFile, ios::binary);
	if (!input.is_open()) {
		result.status = ProfileCatalogStatus::Missing;
		AddDiagnostic(result, "profile.config.missing", DiagnosticSeverity::Error,
			wstring_to_utf8(configFile.wstring()));
		return result;
	}
	vector<wstring> lines;
	for (string line; getline(input, line);) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		lines.push_back(utf8_to_wstring(line));
	}
	wstring section;
	Config* config = nullptr;
	SpecialConfig* special = nullptr;
	bool invalid = false;
	bool identityMigration = false;
	bool hasLegacyTasks = false;
	for (size_t index = 0; index < lines.size(); ++index) {
		const wstring& line = lines[index];
		if (line.empty() || line.front() == L'#') continue;
		if (line.front() == L'[' && line.back() == L']') {
			section = line.substr(1, line.size() - 2);
			config = nullptr;
			special = nullptr;
			int sectionIndex = 0;
			if (section.rfind(L"Config", 0) == 0) {
				if (!LegacyIniConfigCodec::TryParseInt(
						section.substr(6), 1, (numeric_limits<int>::max)(), sectionIndex)) {
					invalid = true;
					AddDiagnostic(result, "config.section.invalid", DiagnosticSeverity::Error,
						"line=" + to_string(index + 1));
					continue;
				}
				config = &result.catalog.configs[sectionIndex];
			}
			else if (section.rfind(L"SpCfg", 0) == 0) {
				if (!LegacyIniConfigCodec::TryParseInt(
						section.substr(5), 1, (numeric_limits<int>::max)(), sectionIndex)) {
					invalid = true;
					AddDiagnostic(result, "config.section.invalid", DiagnosticSeverity::Error,
						"line=" + to_string(index + 1));
					continue;
				}
				special = &result.catalog.specialConfigs[sectionIndex];
			}
			continue;
		}
		const auto separator = line.find(L'=');
		if (separator == wstring::npos) continue;
		const wstring key = line.substr(0, separator);
		const wstring value = line.substr(separator + 1);
		const size_t lineNumber = index + 1;
		if (config) {
			if (key == L"ConfigName") config->name = wstring_to_utf8(value);
			else if (key == L"ConfigId") config->configId = value;
			else if (key == L"PendingLocalBinding") invalid |= !ReadBoolean(result, value, config->pendingLocalBinding, lineNumber, key);
			else if (key == L"SavePath") config->saveRoot = value;
			else if (key == L"WorldData") {
				bool terminated = false;
				while (++index < lines.size()) {
					if (lines[index] == L"*") { terminated = true; break; }
					const wstring name = lines[index];
					if (++index >= lines.size() || lines[index] == L"*") break;
					wstring normalized;
					if (!SpecialTaskStorage::TryNormalizeWorldPath(name, normalized)) {
						invalid = true;
						AddDiagnostic(result, "config.world.invalid", DiagnosticSeverity::Error,
							wstring_to_utf8(name));
					}
					else config->worlds.push_back({normalized, lines[index]});
				}
				if (!terminated) {
					invalid = true;
					AddDiagnostic(result, "config.world_data.truncated", DiagnosticSeverity::Error,
						"line=" + to_string(lineNumber));
				}
			}
			else if (key == L"BackupPath") config->backupPath = value;
			else if (key == L"ZipProgram") config->zipPath = value;
			else if (key == L"ZipFormat") config->zipFormat = value;
			else if (key == L"ZipLevel") invalid |= !ReadInteger(result, value, 0, 22, config->zipLevel, lineNumber, key);
			else if (key == L"ZipMethod") config->zipMethod = value;
			else if (key == L"KeepCount") invalid |= !ReadInteger(result, value, 0, 100000, config->keepCount, lineNumber, key);
			else if (key == L"SmartBackup") invalid |= !ReadInteger(result, value, 0, 3, config->backupMode, lineNumber, key);
			else if (key == L"RestoreBeforeBackup") invalid |= !ReadBoolean(result, value, config->backupBefore, lineNumber, key);
			else if (key == L"CpuThreads") invalid |= !ReadInteger(result, value, 0, 1024, config->cpuThreads, lineNumber, key);
			else if (key == L"UseLowPriority") invalid |= !ReadBoolean(result, value, config->useLowPriority, lineNumber, key);
			else if (key == L"SkipIfUnchanged") invalid |= !ReadBoolean(result, value, config->skipIfUnchanged, lineNumber, key);
			else if (key == L"MaxSmartBackups") invalid |= !ReadInteger(result, value, 0, 100000, config->maxSmartBackupsPerFull, lineNumber, key);
			else if (key == L"BackupOnStart") invalid |= !ReadBoolean(result, value, config->backupOnGameStart, lineNumber, key);
			else if (key == L"BlacklistItem") config->blacklist.push_back(value);
			else if (key == L"CloudSyncEnabled") invalid |= !ReadBoolean(result, value, config->cloudSyncEnabled, lineNumber, key);
			else if (key == L"RclonePath") config->rclonePath = value;
			else if (key == L"RcloneRemotePath") config->rcloneRemotePath = value;
			else if (key == L"CloudSyncMode") invalid |= !ReadInteger(result, value, 0, 1, config->cloudSyncMode, lineNumber, key);
			else if (key == L"CloudWorkingDirectory") config->cloudWorkingDirectory = value;
			else if (key == L"CloudTimeoutSeconds") invalid |= !ReadInteger(result, value, 1, 86400, config->cloudTimeoutSeconds, lineNumber, key);
			else if (key == L"CloudRetryCount") invalid |= !ReadInteger(result, value, 0, 100, config->cloudRetryCount, lineNumber, key);
			else if (key == L"CloudSyncHistoryAfterUpload") invalid |= !ReadBoolean(result, value, config->cloudSyncHistoryAfterUpload, lineNumber, key);
			else if (key == L"CloudAutoDownloadBeforeRestore") invalid |= !ReadBoolean(result, value, config->cloudAutoDownloadBeforeRestore, lineNumber, key);
			else if (key == L"CloudLastRunUtc") config->cloudLastRunUtc = value;
			else if (key == L"CloudLastExitCode") invalid |= !ReadInteger(result, value, (numeric_limits<int>::min)(), (numeric_limits<int>::max)(), config->cloudLastExitCode, lineNumber, key);
			else if (key == L"CloudLastErrorMessage") config->cloudLastErrorMessage = value;
			else if (key == L"SnapshotPath") config->snapshotPath = value;
			else if (key == L"OtherPath") config->othersPath = value;
			else if (key == L"EnableWEIntegration") invalid |= !ReadBoolean(result, value, config->enableWEIntegration, lineNumber, key);
			else if (key == L"WESnapshotPath") config->weSnapshotPath = value;
		}
		else if (special) {
			if (key == L"Name") special->name = wstring_to_utf8(value);
			else if (key == L"SpecialConfigId") special->specialConfigId = value;
			else if (key == L"AutoExecute") invalid |= !ReadBoolean(result, value, special->autoExecute, lineNumber, key);
			else if (key == L"ExitAfter") invalid |= !ReadBoolean(result, value, special->exitAfterExecution, lineNumber, key);
			else if (key == L"HideWindow") invalid |= !ReadBoolean(result, value, special->hideWindow, lineNumber, key);
			else if (key == L"RunOnStartup") invalid |= !ReadBoolean(result, value, special->runOnStartup, lineNumber, key);
			else if (key == L"ZipLevel") invalid |= !ReadInteger(result, value, 0, 22, special->zipLevel, lineNumber, key);
			else if (key == L"KeepCount") invalid |= !ReadInteger(result, value, 0, 100000, special->keepCount, lineNumber, key);
			else if (key == L"CpuThreads") invalid |= !ReadInteger(result, value, 0, 1024, special->cpuThreads, lineNumber, key);
			else if (key == L"UseLowPriority") invalid |= !ReadBoolean(result, value, special->useLowPriority, lineNumber, key);
			else if (key == L"BackupOnStart") invalid |= !ReadBoolean(result, value, special->backupOnGameStart, lineNumber, key);
			else if (key == L"BlacklistItem") special->blacklist.push_back(value);
			else if (key == L"Command") { special->commands.push_back(value); hasLegacyTasks = true; }
			else if (key == L"AutoBackupTask") {
				AutomatedTask task;
				if (!ParseAutomatedTask(value, task)) {
					invalid = true;
					AddDiagnostic(result, "special.legacy_task.invalid",
						DiagnosticSeverity::Error,
						"line=" + to_string(lineNumber) + " key=AutoBackupTask");
				}
				else special->tasks.push_back(task);
				hasLegacyTasks = true;
			}
			else if (key == L"UnifiedTask") {
				UnifiedTaskV2 task;
				if (!ParseUnifiedTask(value, task)) {
					invalid = true;
					AddDiagnostic(result, "special.legacy_task.invalid",
						DiagnosticSeverity::Error,
						"line=" + to_string(lineNumber) + " key=UnifiedTask");
				}
				else special->unifiedTasks.push_back(std::move(task));
				hasLegacyTasks = true;
			}
		}
	}

	set<wstring> configIds;
	for (auto& [index, value] : result.catalog.configs) {
		(void)index;
		if (value.configId.empty()) {
			value.legacyConfigIdGenerated = true;
			identityMigration = true;
			AddDiagnostic(result, "config.identity.migration_required",
				DiagnosticSeverity::Error, value.name);
		}
		else if (!configIds.insert(value.configId).second) {
			invalid = true;
			AddDiagnostic(result, "config.identity.duplicate",
				DiagnosticSeverity::Error, wstring_to_utf8(value.configId));
		}
	}
	set<wstring> specialIds;
	for (auto& [index, value] : result.catalog.specialConfigs) {
		(void)index;
		if (value.specialConfigId.empty()) {
			value.legacySpecialConfigIdGenerated = true;
			identityMigration = true;
			AddDiagnostic(result, "special.identity.migration_required",
				DiagnosticSeverity::Error, value.name);
		}
		else if (!specialIds.insert(value.specialConfigId).second) {
			invalid = true;
			AddDiagnostic(result, "special.identity.duplicate",
				DiagnosticSeverity::Error, wstring_to_utf8(value.specialConfigId));
		}
	}

	error_code existsError;
	if (filesystem::exists(specialTasksFile, existsError) && !existsError) {
		const auto loaded = SpecialTaskStorage::Load(specialTasksFile);
		if (!loaded.IsLoaded()) {
			invalid = true;
			AddDiagnostic(result,
				loaded.status == SpecialTaskStorage::LoadStatus::UnsupportedSchema
					? "special.schema.unsupported" : "special.document.invalid",
				DiagnosticSeverity::Error, wstring_to_utf8(specialTasksFile.wstring()));
		}
		else {
			vector<SpecialTaskStorage::Diagnostic> diagnostics;
			if (!SpecialTaskStorage::ApplyAndValidate(
					loaded.document,
					result.catalog.configs,
					result.catalog.specialConfigs,
					diagnostics)) invalid = true;
			for (const auto& diagnostic : diagnostics) {
				AddDiagnostic(result, diagnostic.eventId,
					diagnostic.severity == SpecialTaskStorage::DiagnosticSeverity::Fatal
						? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
					diagnostic.detail);
			}
		}
	}
	else if (hasLegacyTasks) {
		result.catalog.specialTaskMigrationPending = true;
		AddDiagnostic(result, "special.migration.pending", DiagnosticSeverity::Warning,
			"special-tasks.json has not been created yet.");
	}

	if (invalid) result.status = ProfileCatalogStatus::Invalid;
	else if (identityMigration) result.status = ProfileCatalogStatus::MigrationRequired;
	else result.status = ProfileCatalogStatus::Loaded;
	return result;
}
