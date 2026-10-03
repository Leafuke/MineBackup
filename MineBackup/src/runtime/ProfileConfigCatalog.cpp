#include "ProfileConfigCatalog.h"
#include "ConfigIniCodec.h"
#include "ProfileTransaction.h"

#include "JobDocument.h"
#include "LegacyIniConfigCodec.h"
#include "WorldIdentity.h"
#include "text_to_text.h"

#include <fstream>
#include <limits>
#include <set>

using namespace std;

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
    if (!ProfileTransaction::Inspect(configFile, result.diagnostics)) {
        result.status = ProfileCatalogStatus::Invalid;
        return result;
    }

	ifstream input(configFile, ios::binary);
	if (!input.is_open()) {
		result.status = ProfileCatalogStatus::Missing;
		result.diagnostics.push_back({"profile.config.missing", DiagnosticSeverity::Error, wstring_to_utf8(configFile.wstring())});
		return result;
	}
    auto decoded = ConfigIniCodec::Parse(string(istreambuf_iterator<char>(input), {}));
    result.catalog.configs = std::move(decoded.configs);
    result.diagnostics = std::move(decoded.diagnostics);
    bool invalid = !decoded.valid;
    bool identityMigration = false;
    auto AddDiagnostic = [](auto& value, string event, DiagnosticSeverity severity, string detail) {
        value.diagnostics.push_back({std::move(event), severity, std::move(detail)});
    };

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
