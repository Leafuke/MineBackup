#pragma once

#include "MinecraftTypes.h"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

enum class DiscoveryEvidenceKind {
	KnownLocation,
	LauncherProcess,
	LauncherSettings,
	WorkspaceProbe,
	Manual
};

enum class DiscoveryLocationKind {
	MinecraftRoot,
	SavesRoot,
	VersionRoot,
	BedrockWorldsRoot,
	// Manual 位置由用户选择，Inspector 会根据目录结构进行有限推断。
	Manual
};

struct DiscoveryEvidence {
	DiscoveryEvidenceKind kind = DiscoveryEvidenceKind::KnownLocation;
	std::string providerId;
	std::filesystem::path source;
};

struct DiscoveryLocation {
	std::filesystem::path path;
	DiscoveryLocationKind kind = DiscoveryLocationKind::Manual;
	std::vector<DiscoveryEvidence> evidence;
};

struct DiscoveryDiagnostic {
	// code 是稳定的机器标识；具体文案由上层 i18n 决定。
	std::string code;
	std::string providerId;
	std::filesystem::path relatedPath;
	std::error_code filesystemError;
};

// Provider 只返回待检查的位置；局部失败经回调汇入扫描诊断。
struct MinecraftDiscoveryContext {
	std::function<void(DiscoveryDiagnostic)> reportDiagnostic;
};

class IMinecraftDiscoveryProvider {
public:
	virtual ~IMinecraftDiscoveryProvider() = default;
	virtual std::string Id() const = 0;
	virtual std::vector<DiscoveryLocation> DiscoverLocations(
		const MinecraftDiscoveryContext& context,
		std::stop_token stopToken) = 0;
};

struct DiscoveredMinecraftWorld {
	std::filesystem::path absolutePath;
	std::filesystem::path relativePath;
	std::wstring folderName;
	std::wstring displayName;
};

struct InspectedMinecraftInstance {
	MinecraftEdition edition = MinecraftEdition::Unknown;
	std::filesystem::path instanceRoot;
	std::filesystem::path savesRoot;
	std::wstring suggestedName;
	std::vector<DiscoveredMinecraftWorld> worlds;
	std::vector<DiscoveryEvidence> evidence;
};

struct DiscoveredMinecraftInstance {
	InspectedMinecraftInstance instance;
	bool alreadyConfigured = false;
};

struct MinecraftDiscoveryResult {
	std::vector<DiscoveredMinecraftInstance> instances;
	std::vector<DiscoveryDiagnostic> diagnostics;
};

struct MinecraftInspectionResult {
	std::vector<InspectedMinecraftInstance> instances;
	std::vector<DiscoveryDiagnostic> diagnostics;
};
