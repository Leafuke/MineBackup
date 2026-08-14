#include "ProfileConfigCatalog.h"

#include "JobDocument.h"
#include "LegacyIniConfigCodec.h"
#include "WorldIdentity.h"
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

} // namespace

const Config* ProfileConfigCatalog::FindConfig(const wstring& configId) const {
	for (const auto& [index, config] : configs) {
		(void)index;
		if (config.configId == configId) return &config;
	}
	return nullptr;
}

ProfileCatalogLoadResult ProfileConfigCatalogLoader::Load(
	const filesystem::path& configFile) {
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
	bool invalid = false;
	bool identityMigration = false;
	for (size_t index = 0; index < lines.size(); ++index) {
		const wstring& line = lines[index];
		if (line.empty() || line.front() == L'#') continue;
		if (line.front() == L'[' && line.back() == L']') {
			section = line.substr(1, line.size() - 2);
			config = nullptr;
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
					if (!JobStorage::TryNormalizeWorldPath(name, normalized)) {
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
	for (const auto& conflict : WorldIdentity::FindStorageConflicts(result.catalog.configs)) {
		invalid = true;
		AddDiagnostic(result, "config.storage.collision", DiagnosticSeverity::Error,
			wstring_to_utf8(conflict.backupRoot + L":" + conflict.storageFolderName
				+ L" (" + conflict.leftConfigId + L":" + conflict.leftWorldPath
				+ L", " + conflict.rightConfigId + L":" + conflict.rightWorldPath + L")"));
	}
	if (invalid) result.status = ProfileCatalogStatus::Invalid;
	else if (identityMigration) result.status = ProfileCatalogStatus::MigrationRequired;
	else result.status = ProfileCatalogStatus::Loaded;
	return result;
}
