#include "ProfileConfigRepository.h"

#include "AtomicFileWriter.h"
#include "LegacyIniConfigCodec.h"
#include "text_to_text.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

using namespace std;

namespace {

struct IniSection {
	wstring name;
	vector<wstring> lines;
};

vector<wstring> ReadLines(const filesystem::path& path) {
	ifstream input(path, ios::binary);
	vector<wstring> lines;
	for (string line; getline(input, line);) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		lines.push_back(utf8_to_wstring(line));
	}
	return lines;
}

vector<IniSection> SplitSections(const vector<wstring>& lines) {
	vector<IniSection> sections(1);
	for (const auto& line : lines) {
		if (line.size() >= 2 && line.front() == L'[' && line.back() == L']') {
			sections.push_back({line.substr(1, line.size() - 2), {line}});
		}
		else {
			sections.back().lines.push_back(line);
		}
	}
	return sections;
}

bool ConfigSectionIndex(const wstring& section, int& index) {
	return section.rfind(L"Config", 0) == 0
		&& LegacyIniConfigCodec::TryParseInt(
			section.substr(6), 1, (numeric_limits<int>::max)(), index);
}

wstring FieldValue(const IniSection& section, const wstring& key) {
	const wstring prefix = key + L"=";
	for (const auto& line : section.lines) {
		if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
	}
	return {};
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

vector<wstring> UnknownConfigLines(const IniSection& section) {
	vector<wstring> result;
	bool inWorldData = false;
	for (size_t index = 1; index < section.lines.size(); ++index) {
		const wstring& line = section.lines[index];
		if (inWorldData) {
			if (line == L"*") inWorldData = false;
			continue;
		}
		const auto separator = line.find(L'=');
		if (separator == wstring::npos) {
			if (!line.empty() && line.front() != L'#') result.push_back(line);
			continue;
		}
		const wstring key = line.substr(0, separator);
		if (key == L"WorldData") inWorldData = true;
		if (!ManagedConfigKeys().contains(key)) result.push_back(line);
	}
	return result;
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

void ReplaceRestorePreserve(
	IniSection& general,
	const vector<wstring>& restorePreserve) {
	if (general.lines.empty()) general.lines.push_back(L"[General]");
	erase_if(general.lines, [](const wstring& line) {
		return line.rfind(L"RestoreWhitelistItem=", 0) == 0;
	});
	while (!general.lines.empty() && general.lines.back().empty()) {
		general.lines.pop_back();
	}
	for (const auto& item : restorePreserve) {
		general.lines.push_back(L"RestoreWhitelistItem=" + item);
	}
	general.lines.emplace_back();
}

string JoinUtf8(const vector<IniSection>& sections) {
	wostringstream output;
	for (const auto& section : sections) {
		for (const auto& line : section.lines) output << line << L'\n';
	}
	return wstring_to_utf8(output.str());
}

} // namespace

ProfileConfigRepository::ProfileConfigRepository(filesystem::path configFile)
	: configFile_(std::move(configFile)) {}

ProfileConfigSnapshot ProfileConfigRepository::Load() const {
	ProfileConfigSnapshot snapshot;
	error_code error;
	if (!filesystem::exists(configFile_, error) || error) {
		snapshot.status = ProfileCatalogStatus::Missing;
		return snapshot;
	}
	const auto loaded = ProfileConfigCatalogLoader::Load(
		configFile_, configFile_.parent_path() / L".minebackup-ignore-special-tasks.json");
	snapshot.status = loaded.status;
	snapshot.configs = loaded.catalog.configs;
	snapshot.diagnostics = loaded.diagnostics;
	for (const auto& section : SplitSections(ReadLines(configFile_))) {
		if (section.name != L"General") continue;
		for (const auto& line : section.lines) {
			const wstring prefix = L"RestoreWhitelistItem=";
			if (line.rfind(prefix, 0) == 0) {
				snapshot.restorePreserve.push_back(line.substr(prefix.size()));
			}
		}
	}
	return snapshot;
}

ProfileConfigWriteResult ProfileConfigRepository::Save(
	const map<int, Config>& configs,
	const vector<wstring>& restorePreserve,
	bool pruneMissingConfigs) const {
	ProfileConfigWriteResult result;
	vector<IniSection> sections;
	error_code existsError;
	if (filesystem::exists(configFile_, existsError) && !existsError) {
		sections = SplitSections(ReadLines(configFile_));
	}
	else {
		sections.push_back({});
	}

	auto general = find_if(sections.begin(), sections.end(), [](const IniSection& section) {
		return section.name == L"General";
	});
	if (general == sections.end()) {
		general = sections.insert(sections.begin() + min<size_t>(1, sections.size()),
			IniSection{L"General", {L"[General]"}});
	}
	ReplaceRestorePreserve(*general, restorePreserve);

	map<wstring, pair<int, vector<wstring>>> existing;
	int maximumIndex = 0;
	for (const auto& section : sections) {
		int index = 0;
		if (!ConfigSectionIndex(section.name, index)) continue;
		maximumIndex = max(maximumIndex, index);
		const wstring id = FieldValue(section, L"ConfigId");
		if (!id.empty()) existing[id] = {index, UnknownConfigLines(section)};
	}

	map<wstring, Config> desired;
	for (const auto& [unused, config] : configs) {
		(void)unused;
		if (config.configId.empty()) {
			result.diagnostics.push_back({"config.identity.required",
				DiagnosticSeverity::Error, config.name});
			return result;
		}
		if (!desired.emplace(config.configId, config).second) {
			result.diagnostics.push_back({"config.identity.duplicate",
				DiagnosticSeverity::Error, wstring_to_utf8(config.configId)});
			return result;
		}
	}

	vector<IniSection> output;
	set<wstring> emitted;
	for (auto& section : sections) {
		int index = 0;
		if (!ConfigSectionIndex(section.name, index)) {
			output.push_back(std::move(section));
			continue;
		}
		const wstring id = FieldValue(section, L"ConfigId");
		const auto replacement = desired.find(id);
		if (replacement == desired.end()) {
			if (!pruneMissingConfigs) output.push_back(std::move(section));
			continue;
		}
		output.push_back({L"Config" + to_wstring(index),
			SerializeConfig(index, replacement->second, UnknownConfigLines(section))});
		emitted.insert(id);
	}
	for (const auto& [id, config] : desired) {
		if (emitted.contains(id)) continue;
		const int index = ++maximumIndex;
		output.push_back({L"Config" + to_wstring(index),
			SerializeConfig(index, config, {})});
	}

	const auto write = AtomicFileWriter::WriteText(configFile_, JoinUtf8(output));
	result.success = write.success;
	result.backupPath = write.backupPath;
	if (!write.success) {
		result.diagnostics.push_back({"config.write.failed", DiagnosticSeverity::Error,
			wstring_to_utf8(write.error)});
	}
	return result;
}
