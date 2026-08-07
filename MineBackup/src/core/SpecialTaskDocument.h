#pragma once

#include "DataModels.h"
#include "SpecialTaskModels.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace SpecialTaskStorage {

enum class DiagnosticSeverity {
	Warning,
	Fatal
};

struct Diagnostic {
	DiagnosticSeverity severity = DiagnosticSeverity::Warning;
	std::string eventId;
	std::wstring specialConfigId;
	std::wstring taskId;
	std::string detail;
};

enum class LoadStatus {
	Missing,
	Loaded,
	Invalid,
	UnsupportedSchema,
	IoError
};

struct LoadResult {
	LoadStatus status = LoadStatus::Missing;
	SpecialTaskDocument document;
	std::vector<Diagnostic> diagnostics;

	bool IsLoaded() const { return status == LoadStatus::Loaded; }
};

struct MigrationResult {
	bool success = false;
	SpecialTaskDocument document;
	std::vector<Diagnostic> diagnostics;
};

bool TryNormalizeWorldPath(const std::wstring& value, std::wstring& normalized);
bool IsStableIdentifier(const std::wstring& value);

std::string Serialize(const SpecialTaskDocument& document);
LoadResult Parse(const std::string& content);
LoadResult Load(const std::filesystem::path& path);
bool Save(
	const std::filesystem::path& path,
	const SpecialTaskDocument& document,
	std::wstring& error);

MigrationResult MigrateLegacy(
	const std::map<int, Config>& configs,
	const std::map<int, SpecialConfig>& specialConfigs);

SpecialTaskDocument BuildDocument(
	const std::map<int, SpecialConfig>& specialConfigs);

bool ApplyAndValidate(
	const SpecialTaskDocument& document,
	const std::map<int, Config>& configs,
	std::map<int, SpecialConfig>& specialConfigs,
	std::vector<Diagnostic>& diagnostics);

bool HasFatalDiagnostics(const std::vector<Diagnostic>& diagnostics);

} // namespace SpecialTaskStorage
