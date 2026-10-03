#pragma once

#include "JobModels.h"

#include <filesystem>
#include <map>

namespace JobStorage {

enum class LoadStatus {
	Missing,
	Loaded,
	Invalid,
	UnsupportedSchema,
	IoError
};

struct LoadResult {
	LoadStatus status = LoadStatus::Missing;
	JobDocument document;
	std::vector<Diagnostic> diagnostics;

	bool IsLoaded() const noexcept { return status == LoadStatus::Loaded; }
};

bool IsCanonicalUuid(const std::wstring& value) noexcept;
bool TryNormalizeWorldPath(const std::wstring& value, std::wstring& normalized);
bool ValidateReferences(
	const JobDocument& document,
	const std::map<int, Config>& configs,
	std::vector<Diagnostic>& diagnostics);

std::string Serialize(const JobDocument& document);
LoadResult Parse(const std::string& content);
LoadResult Load(const std::filesystem::path& path);
bool Save(
	const std::filesystem::path& path,
	const JobDocument& document,
	std::wstring& error);

const Job* Find(const JobDocument& document, const std::wstring& jobId) noexcept;

} // namespace JobStorage
