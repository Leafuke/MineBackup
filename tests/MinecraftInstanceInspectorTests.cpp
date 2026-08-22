#include "MinecraftInstanceInspector.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
	if (condition) return;
	++failures;
	std::cerr << "[FAIL] " << message << '\n';
}

std::filesystem::path TemporaryRoot() {
	return std::filesystem::temp_directory_path()
		/ ("MineBackupMinecraftInspector-"
			+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void Touch(const std::filesystem::path& path) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << "level";
}

DiscoveryLocation Location(
	const std::filesystem::path& path,
	DiscoveryLocationKind kind) {
	return {path, kind, {{DiscoveryEvidenceKind::Manual, "test", path}}};
}

void TestJavaRoot(const std::filesystem::path& root) {
	const auto minecraft = root / ".minecraft";
	Touch(minecraft / "saves" / "Zulu" / "level.dat");
	Touch(minecraft / "saves" / "alpha" / "level.dat");
	std::filesystem::create_directories(minecraft / "saves" / "not-a-world");
	Touch(minecraft / "versions" / "Fabric" / "saves" / L"隔离" / "level.dat");
	Touch(minecraft / "versions" / "Empty" / "saves" / "missing.dat");
	std::error_code linkError;
	std::filesystem::create_directories(minecraft / "versions" / "Duplicate");
	std::filesystem::create_directory_symlink(
		minecraft / "saves", minecraft / "versions" / "Duplicate" / "saves", linkError);

	MinecraftInstanceInspector inspector;
	const auto result = inspector.InspectDetailed(
		Location(minecraft, DiscoveryLocationKind::MinecraftRoot));
	Expect(result.instances.size() == 2, "Java root should produce default and isolated instances");
	Expect(result.instances[0].suggestedName == L"Minecraft"
		&& result.instances[0].worlds.size() == 2,
		"default Java instance should contain only level.dat worlds");
	Expect(result.instances[0].worlds[0].folderName == L"alpha"
		&& result.instances[0].worlds[1].folderName == L"Zulu",
		"Java worlds should use stable case-insensitive folder ordering");
	Expect(result.instances[0].worlds[0].relativePath == std::filesystem::path(L"alpha")
		&& result.instances[0].worlds[0].absolutePath.filename() == L"alpha",
		"Java world paths should include a relative saves-root path and absolute path");
	Expect(result.instances[1].suggestedName == L"Fabric"
		&& result.instances[1].worlds.size() == 1
		&& result.instances[1].worlds[0].folderName == L"隔离",
		"version-isolated saves should use the version folder as the instance name");
	if (!linkError) {
		Expect(std::none_of(result.instances.begin(), result.instances.end(), [](const auto& instance) {
			return instance.suggestedName == L"Duplicate";
		}), "canonical saves-root identity should suppress a linked duplicate instance");
	}
}

void TestBedrock(const std::filesystem::path& root) {
	const auto worlds = root / "minecraftWorlds";
	Touch(worlds / "world-b" / "level.dat");
	std::filesystem::create_directories(worlds / "world-b" / "db");
	std::ofstream(worlds / "world-b" / "levelname.txt", std::ios::binary)
		<< "Bedrock B\r\nignored";
	Touch(worlds / "world-a" / "level.dat");
	std::ofstream(worlds / "world-a" / "levelname.txt", std::ios::binary)
		<< "世界 A\n";
	Touch(worlds / "world-c" / "level.dat");
	std::ofstream(worlds / "world-c" / "levelname.txt", std::ios::binary)
		<< std::string(64 * 1024 + 1, 'x');
	std::filesystem::create_directories(worlds / "not-a-world");

	MinecraftInstanceInspector inspector;
	const auto result = inspector.InspectDetailed(
		Location(worlds, DiscoveryLocationKind::BedrockWorldsRoot));
	Expect(result.instances.size() == 1 && result.instances[0].edition == MinecraftEdition::Bedrock,
		"Bedrock worlds root should produce one Bedrock instance");
	Expect(result.instances[0].worlds.size() == 3
		&& result.instances[0].worlds[0].displayName == L"世界 A"
		&& result.instances[0].worlds[1].displayName == L"Bedrock B"
		&& result.instances[0].worlds[2].displayName == L"world-c",
		"Bedrock levelname.txt should be a display-only name with stable ordering");
	Expect(result.instances[0].worlds[1].absolutePath.filename() == L"world-b",
		"Bedrock db should remain auxiliary and not alter level.dat world identity");
	Expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
		[](const auto& diagnostic) {
			return diagnostic.code == "bedrock_levelname_invalid";
		}), "oversized Bedrock display names should fall back with a diagnostic");
}

void TestManualAndCancellation(const std::filesystem::path& root) {
	const auto manual = root / "manual-saves";
	Touch(manual / "World" / "level.dat");
	MinecraftInstanceInspector inspector;
	const auto manualResult = inspector.InspectDetailed(
		Location(manual, DiscoveryLocationKind::Manual));
	Expect(manualResult.instances.size() == 1
		&& manualResult.instances[0].edition == MinecraftEdition::Java,
		"manual saves location should use the common Java inspector");
	Expect(std::any_of(manualResult.instances[0].evidence.begin(),
		manualResult.instances[0].evidence.end(), [](const auto& evidence) {
			return evidence.kind == DiscoveryEvidenceKind::Manual;
		}), "manual location should retain manual evidence");

	std::stop_source stop;
	stop.request_stop();
	const auto cancelled = inspector.InspectDetailed(
		Location(manual, DiscoveryLocationKind::Manual), stop.get_token());
	Expect(cancelled.instances.empty() && !cancelled.diagnostics.empty()
		&& cancelled.diagnostics[0].code == "discovery_cancelled",
		"cancelled inspection should stop without producing candidates");
}

void TestErrorDegradation(const std::filesystem::path& root) {
	MinecraftInstanceInspector inspector;
	const auto result = inspector.InspectDetailed(
		Location(root / "does-not-exist", DiscoveryLocationKind::MinecraftRoot));
	Expect(result.instances.empty() && !result.diagnostics.empty()
		&& result.diagnostics[0].code == "location_not_found",
		"missing locations should degrade to diagnostics instead of throwing");
}

} // namespace

int main() {
	const auto root = TemporaryRoot();
	std::error_code error;
	std::filesystem::create_directories(root);
	TestJavaRoot(root);
	TestBedrock(root);
	TestManualAndCancellation(root);
	TestErrorDegradation(root);
	std::filesystem::remove_all(root, error);
	if (failures == 0) {
		std::cout << "[PASS] Minecraft instance inspector tests\n";
		return 0;
	}
	return 1;
}
