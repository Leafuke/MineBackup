#include "MinecraftInstanceInspector.h"

#include "FolderRewindFormat.h"
#include "PathIdentity.h"
#include "text_to_text.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

namespace {

using std::filesystem::path;
using std::filesystem::directory_iterator;
using std::filesystem::directory_options;

constexpr std::size_t kMaximumLevelNameBytes = 64u * 1024u;

path NormalizePath(const path& input) {
	return input.empty()
		? path{} : PathIdentity::NormalizeExistingOrProspectivePath(input);
}

std::wstring LowerForSort(std::wstring value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
	return value;
}

std::wstring PathKey(const path& input) {
	return ::PathIdentity::BuildPathIdentityKey(input);
}

std::string ProviderId(const DiscoveryLocation& location) {
	for (const auto& evidence : location.evidence) {
		if (!evidence.providerId.empty()) return evidence.providerId;
	}
	if (location.kind == DiscoveryLocationKind::Manual) return "manual";
	return "minecraft-inspector";
}

void AddDiagnostic(
	MinecraftInspectionResult& result,
	const DiscoveryLocation& location,
	std::string code,
	const path& relatedPath,
	std::error_code error = {}) {
	result.diagnostics.push_back({
		std::move(code), ProviderId(location), relatedPath, error});
}

bool IsDirectory(const path& candidate, std::error_code& error) {
	error.clear();
	const bool directory = std::filesystem::is_directory(candidate, error);
	return directory && !error;
}

bool IsRegularFile(const path& candidate, std::error_code& error) {
	error.clear();
	const bool regular = std::filesystem::is_regular_file(candidate, error);
	return regular && !error;
}

bool ValidateRoot(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& root,
	std::stop_token stopToken) {
	if (stopToken.stop_requested()) {
		AddDiagnostic(result, location, "discovery_cancelled", root);
		return false;
	}
	if (root.empty()) {
		AddDiagnostic(result, location, "location_empty", root);
		return false;
	}

	std::error_code error;
	const bool exists = std::filesystem::exists(root, error);
	if (error) {
		AddDiagnostic(result, location, "location_unreadable", root, error);
		return false;
	}
	if (!exists) {
		AddDiagnostic(result, location, "location_not_found", root);
		return false;
	}
	if (!IsDirectory(root, error)) {
		AddDiagnostic(result, location,
			error ? "location_unreadable" : "location_not_directory", root, error);
		return false;
	}
	return true;
}

std::vector<path> ChildDirectories(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& root,
	std::stop_token stopToken) {
	std::vector<path> children;
	std::error_code iteratorError;
	directory_iterator iterator(
		root, directory_options::skip_permission_denied, iteratorError);
	if (iteratorError) {
		AddDiagnostic(result, location, "directory_unreadable", root, iteratorError);
		return children;
	}
	const directory_iterator end;
	for (; iterator != end; iterator.increment(iteratorError)) {
		if (stopToken.stop_requested()) {
			AddDiagnostic(result, location, "discovery_cancelled", root);
			break;
		}
		const auto current = iterator->path();
		std::error_code statusError;
		if (IsDirectory(current, statusError)) {
			children.push_back(current);
		}
		else if (statusError) {
			// 权限异常只跳过当前项，不能让其他版本或世界丢失。
			AddDiagnostic(result, location, "directory_entry_unreadable", current, statusError);
		}
	}
	if (iteratorError) {
		AddDiagnostic(result, location, "directory_unreadable", root, iteratorError);
	}
	std::sort(children.begin(), children.end(), [](const path& left, const path& right) {
		const auto leftName = LowerForSort(left.filename().wstring());
		const auto rightName = LowerForSort(right.filename().wstring());
		if (leftName != rightName) return leftName < rightName;
		return left.lexically_normal().wstring() < right.lexically_normal().wstring();
	});
	return children;
}

std::vector<DiscoveryEvidence> LocationEvidence(
	const DiscoveryLocation& location,
	const path& fallbackSource) {
	std::vector<DiscoveryEvidence> evidence = location.evidence;
	if (location.kind == DiscoveryLocationKind::Manual
		&& std::none_of(evidence.begin(), evidence.end(), [](const DiscoveryEvidence& item) {
			return item.kind == DiscoveryEvidenceKind::Manual;
		})) {
		evidence.push_back({DiscoveryEvidenceKind::Manual, "manual", fallbackSource});
	}
	return evidence;
}

bool HasLevelDat(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& worldPath,
	std::stop_token stopToken) {
	if (stopToken.stop_requested()) return false;
	std::error_code error;
	const bool valid = IsRegularFile(worldPath / L"level.dat", error);
	if (error) {
		AddDiagnostic(result, location, "world_level_dat_unreadable", worldPath / L"level.dat", error);
		return false;
	}
	return valid;
}

std::wstring ReadBedrockDisplayName(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& worldPath,
	const std::wstring& fallback,
	std::stop_token stopToken) {
	if (stopToken.stop_requested()) return fallback;
	const path levelNamePath = worldPath / L"levelname.txt";
	std::error_code statusError;
	const bool exists = std::filesystem::exists(levelNamePath, statusError);
	if (statusError) {
		AddDiagnostic(result, location, "bedrock_levelname_unreadable", levelNamePath, statusError);
		return fallback;
	}
	if (!exists) return fallback;
	if (!IsRegularFile(levelNamePath, statusError)) {
		if (statusError) {
			AddDiagnostic(result, location, "bedrock_levelname_unreadable", levelNamePath, statusError);
		}
		return fallback;
	}
	const auto size = std::filesystem::file_size(levelNamePath, statusError);
	if (statusError || size > kMaximumLevelNameBytes) {
		AddDiagnostic(result, location, "bedrock_levelname_invalid", levelNamePath, statusError);
		return fallback;
	}

	std::ifstream input(levelNamePath, std::ios::binary);
	if (!input) {
		AddDiagnostic(result, location, "bedrock_levelname_unreadable", levelNamePath);
		return fallback;
	}
	std::string line;
	line.reserve(128);
	std::getline(input, line);
	if (!line.empty() && line.back() == '\r') line.pop_back();
	if (line.size() >= 3
		&& static_cast<unsigned char>(line[0]) == 0xEF
		&& static_cast<unsigned char>(line[1]) == 0xBB
		&& static_cast<unsigned char>(line[2]) == 0xBF) {
		line.erase(0, 3);
	}
	const auto sanitized = SanitizeUtf8(line, kMaximumLevelNameBytes);
	if (sanitized.truncated || sanitized.invalidUtf8Replaced) {
		AddDiagnostic(result, location, "bedrock_levelname_invalid", levelNamePath);
	}
	if (sanitized.value.empty()) return fallback;
	const auto displayName = utf8_to_wstring(sanitized.value);
	return displayName.empty() ? fallback : displayName;
}

bool AddJavaWorlds(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& savesRoot,
	const path& instanceRoot,
	std::wstring suggestedName,
	std::stop_token stopToken,
	InspectedMinecraftInstance& instance) {
	if (!ValidateRoot(location, result, savesRoot, stopToken)) return false;
	const auto children = ChildDirectories(location, result, savesRoot, stopToken);
	instance.edition = MinecraftEdition::Java;
	instance.instanceRoot = NormalizePath(instanceRoot);
	instance.savesRoot = NormalizePath(savesRoot);
	instance.suggestedName = suggestedName.empty() ? L"Minecraft" : std::move(suggestedName);
	instance.evidence = LocationEvidence(location, instance.savesRoot);
	for (const auto& worldPath : children) {
		if (stopToken.stop_requested()) {
			AddDiagnostic(result, location, "discovery_cancelled", savesRoot);
			break;
		}
		if (!HasLevelDat(location, result, worldPath, stopToken)) continue;
		const auto absoluteWorldPath = NormalizePath(worldPath);
		const auto relativePath = absoluteWorldPath.lexically_relative(instance.savesRoot);
		instance.worlds.push_back({
			absoluteWorldPath,
			path(FolderRewindFormat::NormalizeRelativePath(relativePath)),
			worldPath.filename().wstring(),
			worldPath.filename().wstring()});
	}
	return !instance.worlds.empty();
}

bool AddBedrockWorlds(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& worldsRoot,
	const path& instanceRoot,
	std::stop_token stopToken,
	InspectedMinecraftInstance& instance) {
	if (!ValidateRoot(location, result, worldsRoot, stopToken)) return false;
	const auto children = ChildDirectories(location, result, worldsRoot, stopToken);
	instance.edition = MinecraftEdition::Bedrock;
	instance.instanceRoot = NormalizePath(instanceRoot);
	instance.savesRoot = NormalizePath(worldsRoot);
	instance.suggestedName = L"Minecraft Bedrock";
	instance.evidence = LocationEvidence(location, instance.savesRoot);
	for (const auto& worldPath : children) {
		if (stopToken.stop_requested()) {
			AddDiagnostic(result, location, "discovery_cancelled", worldsRoot);
			break;
		}
		if (!HasLevelDat(location, result, worldPath, stopToken)) continue;
		const auto absoluteWorldPath = NormalizePath(worldPath);
		const auto relativePath = absoluteWorldPath.lexically_relative(instance.savesRoot);
		instance.worlds.push_back({
			absoluteWorldPath,
			path(FolderRewindFormat::NormalizeRelativePath(relativePath)),
			worldPath.filename().wstring(),
			ReadBedrockDisplayName(
				location, result, worldPath, worldPath.filename().wstring(), stopToken)});
	}
	return !instance.worlds.empty();
}

void SortWorlds(InspectedMinecraftInstance& instance) {
	std::sort(instance.worlds.begin(), instance.worlds.end(), [](const auto& left, const auto& right) {
		const auto leftName = LowerForSort(left.folderName);
		const auto rightName = LowerForSort(right.folderName);
		if (leftName != rightName) return leftName < rightName;
		return left.relativePath.wstring() < right.relativePath.wstring();
	});
}

void AddJavaSavesRoot(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& savesRoot,
	const path& instanceRoot,
	std::wstring suggestedName,
	std::stop_token stopToken,
	std::set<std::wstring>& seenSavesRoots) {
	if (stopToken.stop_requested()) return;
	const auto normalizedSavesRoot = NormalizePath(savesRoot);
	if (!seenSavesRoots.insert(PathKey(normalizedSavesRoot)).second) return;
	InspectedMinecraftInstance instance;
	if (AddJavaWorlds(location, result, normalizedSavesRoot, instanceRoot,
		std::move(suggestedName), stopToken, instance)) {
		SortWorlds(instance);
		result.instances.push_back(std::move(instance));
	}
}

void InspectJavaRoot(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& root,
	std::stop_token stopToken) {
	if (!ValidateRoot(location, result, root, stopToken)) return;
	std::set<std::wstring> seenSavesRoots;
	AddJavaSavesRoot(location, result, root / L"saves", root, L"Minecraft",
		stopToken, seenSavesRoots);
	if (stopToken.stop_requested()) {
		AddDiagnostic(result, location, "discovery_cancelled", root);
		return;
	}

	const path versionsRoot = root / L"versions";
	std::error_code versionsError;
	const bool versionsExists = std::filesystem::exists(versionsRoot, versionsError);
	if (versionsError) {
		AddDiagnostic(result, location, "java_versions_unreadable", versionsRoot, versionsError);
		return;
	}
	if (!versionsExists) return;
	if (!IsDirectory(versionsRoot, versionsError)) {
		AddDiagnostic(result, location,
			versionsError ? "java_versions_unreadable" : "java_versions_not_directory",
			versionsRoot, versionsError);
		return;
	}
	for (const auto& versionRoot : ChildDirectories(location, result, versionsRoot, stopToken)) {
		if (stopToken.stop_requested()) return;
		AddJavaSavesRoot(location, result, versionRoot / L"saves", versionRoot,
			versionRoot.filename().wstring(), stopToken, seenSavesRoots);
	}
}

void InspectJavaSavesRoot(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& savesRoot,
	std::stop_token stopToken) {
	const auto parent = savesRoot.parent_path();
	std::wstring suggestedName;
	path instanceRoot = parent;
	if (LowerForSort(parent.filename().wstring()) == L".minecraft") {
		suggestedName = L"Minecraft";
	}
	else {
		suggestedName = parent.filename().wstring();
		if (suggestedName.empty()) {
			suggestedName = savesRoot.filename().wstring();
			instanceRoot = savesRoot;
		}
	}
	std::set<std::wstring> unused;
	AddJavaSavesRoot(location, result, savesRoot, instanceRoot,
		std::move(suggestedName), stopToken, unused);
}

void InspectJavaVersionRoot(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& versionRoot,
	std::stop_token stopToken) {
	std::wstring suggestedName = versionRoot.filename().wstring();
	if (suggestedName.empty()) suggestedName = L"Minecraft";
	std::set<std::wstring> unused;
	AddJavaSavesRoot(location, result, versionRoot / L"saves", versionRoot,
		std::move(suggestedName), stopToken, unused);
}

bool LooksLikeBedrockWorldsRoot(const path& root) {
	return LowerForSort(root.filename().wstring()) == L"minecraftworlds";
}

void InspectManual(
	const DiscoveryLocation& location,
	MinecraftInspectionResult& result,
	const path& root,
	std::stop_token stopToken) {
	if (!ValidateRoot(location, result, root, stopToken)) return;
	if (LooksLikeBedrockWorldsRoot(root)) {
		InspectedMinecraftInstance instance;
		if (AddBedrockWorlds(location, result, root, root, stopToken, instance)) {
			SortWorlds(instance);
			result.instances.push_back(std::move(instance));
		}
		return;
	}

	std::error_code statusError;
	const bool hasJavaSaves = std::filesystem::is_directory(root / L"saves", statusError)
		&& !statusError;
	if (statusError) {
		AddDiagnostic(result, location, "manual_structure_unreadable", root / L"saves", statusError);
		statusError.clear();
	}
	if (hasJavaSaves
		&& LowerForSort(root.parent_path().filename().wstring()) == L"versions") {
		InspectJavaVersionRoot(location, result, root, stopToken);
		return;
	}
	if (hasJavaSaves || LowerForSort(root.filename().wstring()) == L".minecraft") {
		InspectJavaRoot(location, result, root, stopToken);
		return;
	}
	const bool hasVersions = std::filesystem::is_directory(root / L"versions", statusError)
		&& !statusError;
	if (statusError) {
		AddDiagnostic(result, location, "manual_structure_unreadable", root / L"versions", statusError);
	}
	if (hasVersions) {
		InspectJavaRoot(location, result, root, stopToken);
		return;
	}

	// 手动选择的 saves/version 目录无法仅凭名字可靠区分，先按世界根目录尝试 Java。
	std::set<std::wstring> unused;
	AddJavaSavesRoot(location, result, root, root.parent_path(),
		root.filename().wstring(), stopToken, unused);
	if (!result.instances.empty()) return;

	// 最后再尝试 Bedrock，避免把普通 Java saves 误标为 Bedrock。
	InspectedMinecraftInstance bedrock;
	if (AddBedrockWorlds(location, result, root, root, stopToken, bedrock)) {
		SortWorlds(bedrock);
		result.instances.push_back(std::move(bedrock));
	}
}

void SortInstances(MinecraftInspectionResult& result) {
	std::sort(result.instances.begin(), result.instances.end(), [](const auto& left, const auto& right) {
		const bool leftDefault = left.suggestedName == L"Minecraft";
		const bool rightDefault = right.suggestedName == L"Minecraft";
		if (leftDefault != rightDefault) return leftDefault;
		const auto leftName = LowerForSort(left.suggestedName);
		const auto rightName = LowerForSort(right.suggestedName);
		if (leftName != rightName) return leftName < rightName;
		return PathKey(left.savesRoot) < PathKey(right.savesRoot);
	});
}

} // namespace

std::vector<InspectedMinecraftInstance> MinecraftInstanceInspector::Inspect(
	const DiscoveryLocation& location,
	std::stop_token stopToken) const {
	return InspectDetailed(location, stopToken).instances;
}

MinecraftInspectionResult MinecraftInstanceInspector::InspectDetailed(
	const DiscoveryLocation& location,
	std::stop_token stopToken) const {
	MinecraftInspectionResult result;
	if (stopToken.stop_requested()) {
		AddDiagnostic(result, location, "discovery_cancelled", location.path);
		return result;
	}

	switch (location.kind) {
	case DiscoveryLocationKind::MinecraftRoot:
		InspectJavaRoot(location, result, NormalizePath(location.path), stopToken);
		break;
	case DiscoveryLocationKind::SavesRoot:
		InspectJavaSavesRoot(location, result, NormalizePath(location.path), stopToken);
		break;
	case DiscoveryLocationKind::VersionRoot:
		InspectJavaVersionRoot(location, result, NormalizePath(location.path), stopToken);
		break;
	case DiscoveryLocationKind::BedrockWorldsRoot: {
		InspectedMinecraftInstance instance;
		const auto worldsRoot = NormalizePath(location.path);
		if (AddBedrockWorlds(location, result, worldsRoot, worldsRoot,
			stopToken, instance)) {
			SortWorlds(instance);
			result.instances.push_back(std::move(instance));
		}
		break;
	}
	case DiscoveryLocationKind::Manual:
		InspectManual(location, result, NormalizePath(location.path), stopToken);
		break;
	}
	SortInstances(result);
	return result;
}
