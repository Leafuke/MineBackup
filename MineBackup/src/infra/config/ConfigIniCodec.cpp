#include "CompressionPolicy.h"
#include "ConfigIniCodec.h"
#include "JobDocument.h"
#include "LegacyIniConfigCodec.h"
#include "text_to_text.h"
#include <limits>
#include <sstream>
using namespace std;
namespace {
wstring Trim(wstring value) {
    const auto begin = value.find_first_not_of(L" \t\r");
    if (begin == wstring::npos) return {};
    return value.substr(begin, value.find_last_not_of(L" \t\r") - begin + 1);
}
void AddDiagnostic(
	ConfigIniCodec::DecodeResult& result,
	string eventId,
	DiagnosticSeverity severity,
	string detail) {
	result.diagnostics.push_back({
		std::move(eventId), severity, std::move(detail)});
}

bool ReadInteger(
	ConfigIniCodec::DecodeResult& result,
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
	ConfigIniCodec::DecodeResult& result,
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


}
namespace ConfigIniCodec {
DecodeResult Parse(const string& content) {
    DecodeResult result;
    istringstream input(content);
	vector<wstring> lines;
	for (string line; getline(input, line);) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		lines.push_back(utf8_to_wstring(line));
	}
	wstring section;
	Config* config = nullptr;
	bool invalid = false;
	for (size_t index = 0; index < lines.size(); ++index) {
		const wstring line = Trim(lines[index]);
		if (line.empty() || line.front() == L'#') continue;
		if (line.front() == L'[' && line.back() == L']') {
			section = Trim(line.substr(1, line.size() - 2));
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
				config = &result.configs[sectionIndex];
			}
			continue;
		}
		const auto separator = line.find(L'=');
		if (separator == wstring::npos) continue;
		const wstring key = Trim(line.substr(0, separator));
		const wstring value = Trim(line.substr(separator + 1));
		const size_t lineNumber = index + 1;
		if (config) {
			if (key == L"ConfigName") config->name = wstring_to_utf8(value);
			else if (key == L"ConfigId") config->configId = value;
			else if (key == L"PendingLocalBinding") invalid |= !ReadBoolean(result, value, config->pendingLocalBinding, lineNumber, key);
			else if (key == L"SavePath") config->saveRoot = value;
			else if (key == L"WorldData") {
				bool terminated = false;
				while (++index < lines.size()) {
					if (Trim(lines[index]) == L"*") { terminated = true; break; }
					const wstring name = lines[index];
					if (++index >= lines.size() || Trim(lines[index]) == L"*") break;
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
			else if (key == L"SmartBackup") invalid |= !ReadInteger(result, value, BackupPolicy::MinimumMode, BackupPolicy::MaximumMode, config->backupMode, lineNumber, key);
			else if (key == L"RestoreBeforeBackup") invalid |= !ReadBoolean(result, value, config->backupBefore, lineNumber, key);
			else if (key == L"CpuThreads") invalid |= !ReadInteger(result, value, 0, 1024, config->cpuThreads, lineNumber, key);
			else if (key == L"UseLowPriority") invalid |= !ReadBoolean(result, value, config->useLowPriority, lineNumber, key);
			else if (key == L"SkipIfUnchanged") invalid |= !ReadBoolean(result, value, config->skipIfUnchanged, lineNumber, key);
			else if (key == L"MaxSmartBackups") invalid |= !ReadInteger(result, value, BackupPolicy::MinimumSmartCount, BackupPolicy::MaximumSmartCount, config->maxSmartBackupsPerFull, lineNumber, key);
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

    for (auto& [index, config] : result.configs) config.zipLevel = CompressionPolicy::NormalizeLevel(config.zipMethod, config.zipLevel);
    result.valid = !invalid;
    return result;
}
const set<wstring>& ManagedConfigKeys() {
	static const set<wstring> keys{
		L"ConfigName", L"ConfigId", L"PendingLocalBinding", L"SavePath",
		L"WorldData", L"BackupPath", L"ZipProgram", L"ZipFormat",
		L"ZipLevel", L"ZipMethod", L"CpuThreads", L"UseLowPriority",
		L"KeepCount", L"SmartBackup", L"RestoreBeforeBackup",
		L"SkipIfUnchanged", L"MaxSmartBackups", L"BackupOnStart",
		L"CloudSyncEnabled", L"RclonePath", L"RcloneRemotePath",
		L"CloudSyncMode", L"CloudWorkingDirectory", L"CloudTimeoutSeconds",
		L"CloudRetryCount", L"CloudSyncHistoryAfterUpload",
		L"CloudAutoDownloadBeforeRestore", L"CloudLastRunUtc",
		L"CloudLastExitCode", L"CloudLastErrorMessage", L"SnapshotPath",
		L"OtherPath", L"EnableWEIntegration", L"WESnapshotPath",
		L"BlacklistItem"};
	return keys;
}

vector<wstring> SerializeConfig(
	int index,
	const Config& config,
	const vector<wstring>& unknownLines) {
	vector<wstring> lines;
	auto add = [&](const wstring& key, const wstring& value) {
		lines.push_back(key + L"=" + value);
	};
	lines.push_back(L"[Config" + to_wstring(index) + L"]");
	add(L"ConfigName", utf8_to_wstring(config.name));
	add(L"ConfigId", config.configId);
	add(L"PendingLocalBinding", config.pendingLocalBinding ? L"1" : L"0");
	add(L"SavePath", config.saveRoot);
	lines.push_back(L"# One line for name, one line for description, terminated by '*'");
	lines.push_back(L"WorldData=");
	for (const auto& [path, description] : config.worlds) {
		lines.push_back(path);
		lines.push_back(description);
	}
	lines.push_back(L"*");
	add(L"BackupPath", config.backupPath);
	add(L"ZipProgram", config.zipPath);
	add(L"ZipFormat", config.zipFormat);
	add(L"ZipLevel", to_wstring(config.zipLevel));
	add(L"ZipMethod", config.zipMethod);
	add(L"CpuThreads", to_wstring(config.cpuThreads));
	add(L"UseLowPriority", config.useLowPriority ? L"1" : L"0");
	add(L"KeepCount", to_wstring(config.keepCount));
	add(L"SmartBackup", to_wstring(config.backupMode));
	add(L"RestoreBeforeBackup", config.backupBefore ? L"1" : L"0");
	add(L"SkipIfUnchanged", config.skipIfUnchanged ? L"1" : L"0");
	add(L"MaxSmartBackups", to_wstring(config.maxSmartBackupsPerFull));
	add(L"BackupOnStart", config.backupOnGameStart ? L"1" : L"0");
	add(L"CloudSyncEnabled", config.cloudSyncEnabled ? L"1" : L"0");
	add(L"RclonePath", config.rclonePath);
	add(L"RcloneRemotePath", config.rcloneRemotePath);
	add(L"CloudSyncMode", to_wstring(config.cloudSyncMode));
	add(L"CloudWorkingDirectory", config.cloudWorkingDirectory);
	add(L"CloudTimeoutSeconds", to_wstring(config.cloudTimeoutSeconds));
	add(L"CloudRetryCount", to_wstring(config.cloudRetryCount));
	add(L"CloudSyncHistoryAfterUpload", config.cloudSyncHistoryAfterUpload ? L"1" : L"0");
	add(L"CloudAutoDownloadBeforeRestore", config.cloudAutoDownloadBeforeRestore ? L"1" : L"0");
	add(L"CloudLastRunUtc", config.cloudLastRunUtc);
	add(L"CloudLastExitCode", to_wstring(config.cloudLastExitCode));
	add(L"CloudLastErrorMessage", config.cloudLastErrorMessage);
	add(L"SnapshotPath", config.snapshotPath);
	add(L"OtherPath", config.othersPath);
	add(L"EnableWEIntegration", config.enableWEIntegration ? L"1" : L"0");
	add(L"WESnapshotPath", config.weSnapshotPath);
	for (const auto& item : config.blacklist) add(L"BlacklistItem", item);
	lines.insert(lines.end(), unknownLines.begin(), unknownLines.end());
	lines.emplace_back();
	return lines;
}

}
