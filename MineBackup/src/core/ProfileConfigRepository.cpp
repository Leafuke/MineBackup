#include "ProfileConfigRepository.h"
#include "ConfigIniCodec.h"
#include <stdexcept>

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
    if (!input.is_open()) throw runtime_error("Cannot read existing configuration document");
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
		if (!ConfigIniCodec::ManagedConfigKeys().contains(key)) result.push_back(line);
	}
	return result;
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
	const auto loaded = ProfileConfigCatalogLoader::Load(configFile_);
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

ProfileConfigDocumentResult ProfileConfigRepository::Prepare(
	const map<int, Config>& configs,
	const vector<wstring>& restorePreserve,
	bool pruneMissingConfigs, const string& desktopGeneral) const {
	ProfileConfigDocumentResult result;
    for (const auto& [index, config] : configs) {
        if (!BackupPolicy::IsValid(config.backupMode, config.maxSmartBackupsPerFull)) {
            result.diagnostics.push_back({"config.backup_policy.invalid", DiagnosticSeverity::Error, config.name});
            return result;
        }
    }

	vector<IniSection> sections;
	error_code existsError;
	if (filesystem::exists(configFile_, existsError) && !existsError) {
		sections = SplitSections(ReadLines(configFile_));
	}
	else {
        if (existsError) throw filesystem::filesystem_error("Cannot inspect configuration", configFile_, existsError);
		sections.push_back({});
	}

	auto general = find_if(sections.begin(), sections.end(), [](const IniSection& section) {
		return section.name == L"General";
	});
	if (general == sections.end()) {
		general = sections.insert(sections.begin() + min<size_t>(1, sections.size()),
			IniSection{L"General", {L"[General]"}});
	}
    if (!desktopGeneral.empty()) {
        istringstream input(desktopGeneral);
        vector<wstring> replacement;
        set<wstring> keys;
        for (string line; getline(input, line);) {
            auto wide = utf8_to_wstring(line);
            const auto separator = wide.find(L'=');
            if (separator != wstring::npos) keys.insert(wide.substr(0, separator));
            replacement.push_back(std::move(wide));
        }
        for (size_t i = 1; i < general->lines.size(); ++i) {
            const auto& line = general->lines[i];
            const auto separator = line.find(L'=');
            if (separator != wstring::npos && !keys.contains(line.substr(0, separator))) replacement.push_back(line);
        }
        general->lines = std::move(replacement);
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
			ConfigIniCodec::SerializeConfig(index, replacement->second, UnknownConfigLines(section))});
		emitted.insert(id);
	}
	for (const auto& [id, config] : desired) {
		if (emitted.contains(id)) continue;
		const auto requested = find_if(configs.begin(), configs.end(), [&](const auto& pair) { return pair.second.configId == id; });
        const bool occupied = any_of(existing.begin(), existing.end(), [&](const auto& pair) { return pair.second.first == requested->first; });
        const int index = !occupied && requested->first > 0 ? requested->first : ++maximumIndex;
        maximumIndex = max(maximumIndex, index);
		output.push_back({L"Config" + to_wstring(index),
			ConfigIniCodec::SerializeConfig(index, config, {})});
	}

    result.content = JoinUtf8(output);
    result.success = true;
    return result;
}

ProfileConfigWriteResult ProfileConfigRepository::Save(const map<int, Config>& configs,
    const vector<wstring>& restorePreserve, bool pruneMissingConfigs) const {
    ProfileConfigWriteResult result;
    try {
        const auto prepared = Prepare(configs, restorePreserve, pruneMissingConfigs);
        result.diagnostics = prepared.diagnostics;
        if (!prepared.success) return result;
        const auto write = AtomicFileWriter::WriteText(configFile_, prepared.content);
        result.success = write.WasReplaced();
        result.backupPath = write.backupPath;
        if (!write.IsDurable()) result.diagnostics.push_back({"config.write.failed",
            write.WasReplaced() ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error, wstring_to_utf8(write.error)});
    } catch (const exception& error) {
        result.diagnostics.push_back({"config.write.failed", DiagnosticSeverity::Error, error.what()});
    }
    return result;
}
