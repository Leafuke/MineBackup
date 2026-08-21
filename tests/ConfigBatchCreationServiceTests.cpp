#include "AppState.h"
#include "ConfigBatchCreationService.h"
#include "ConfigFactory.h"
#include "ConfigManager.h"
#include "Globals.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

struct TestContext {
	int failures = 0;

	void Expect(bool condition, const char* message) {
		if (condition) return;
		++failures;
		std::cerr << "FAIL: " << message << '\n';
	}
};

struct GlobalSnapshot {
	std::map<int, Config> configs = g_appState.configs;
	int currentConfigIndex = g_appState.currentConfigIndex;
	NormalConfigIndexAllocatorState allocator = SnapshotNormalConfigIndexAllocator();
	std::wstring backupRoot = g_defaultBackupRootPath;
	bool validationPending = g_CoreValidationPending.load();
	bool validationPassed = g_CoreValidationPassed.load();
	std::vector<std::wstring> restoreWhitelistSnapshot = restoreWhitelist;

	~GlobalSnapshot() {
		g_appState.configs = std::move(configs);
		g_appState.currentConfigIndex = currentConfigIndex;
		RestoreNormalConfigIndexAllocator(allocator);
		g_defaultBackupRootPath = std::move(backupRoot);
		g_CoreValidationPending.store(validationPending);
		g_CoreValidationPassed.store(validationPassed);
		restoreWhitelist = std::move(restoreWhitelistSnapshot);
	}
};

ConfigDraft Draft(std::string name, const std::filesystem::path& root) {
	ConfigDraft draft;
	draft.name = std::move(name);
	draft.edition = MinecraftEdition::Java;
	draft.saveRoot = root / "saves";
	draft.worlds = {{L"World", L"World"}};
	return draft;
}

void TestCompatibilityCreationEntry(TestContext& test) {
	GlobalSnapshot restore;
	g_appState.configs.clear();
	RestoreNormalConfigIndexAllocator({2});
	restoreWhitelist = {L"custom-only"};
	const int index = CreateNewNormalConfig("Manual");
	const Config& config = g_appState.configs.at(index);
	test.Expect(index == 2 && config.name == "Manual" && config.keepCount == 20
			&& !config.configId.empty(),
		"the legacy create entry should reuse factory defaults and assign identity at commit");
	test.Expect(restoreWhitelist == std::vector<std::wstring>({L"custom-only"}),
		"the compatibility create entry should not rewrite global restore settings");
}

void TestFactoryDefaults(TestContext& test, const std::filesystem::path& root) {
	GlobalSnapshot restore;
	restoreWhitelist = {L"custom-only"};
	ConfigDraft draft = Draft("Java", root);
	draft.backupPath = root / "backups" / "Java";
	ConfigFactoryContext context{root / "tools" / "7za.exe"};
	const Config config = BuildRecommendedConfig(draft, context);
	test.Expect(config.configId.empty(),
		"factory should leave ConfigId assignment to the commit boundary");
	test.Expect(config.keepCount == 20 && config.zipLevel == 5
			&& config.backupMode == 1 && config.skipIfUnchanged,
		"factory should apply the recommended backup defaults");
	test.Expect(config.zipFormat == L"7z" && config.zipMethod == L"LZMA2"
			&& config.zipPath == context.resolvedSevenZip.wstring(),
		"factory should use the batch-resolved 7-Zip executable");
	test.Expect(!config.blacklist.empty() && !config.pendingLocalBinding,
		"factory should apply only per-config safety defaults");
	test.Expect(Config{}.keepCount == 0,
		"the structural Config default must remain compatible with old files");
	test.Expect(restoreWhitelist == std::vector<std::wstring>({L"custom-only"}),
		"the pure config factory must not mutate the global restore whitelist");
}

void TestNaming(TestContext& test, const std::filesystem::path& root) {
	const auto backupRoot = root / "naming";
	std::filesystem::create_directories(backupRoot / "Create (2)");
	Config existing;
	existing.name = "Create";
	existing.backupPath = (backupRoot / "Create").wstring();
	const auto resolved = ResolveUniqueConfigDrafts(
		{Draft("Create", root), Draft("Create", root), Draft("", root)},
		backupRoot,
		{{2, existing}});
	test.Expect(resolved.size() == 3 && resolved[0].name == "Create (3)"
			&& resolved[1].name == "Create (4)" && resolved[2].name == "Minecraft",
		"naming should account for existing configs, the batch, and filesystem entries");
	test.Expect(resolved[0].backupPath == backupRoot / "Create (3)"
			&& resolved[1].backupPath == backupRoot / "Create (4)",
		"backup paths should follow the final unique config names");

	const auto reserved = ResolveUniqueConfigDrafts(
		{Draft("CON", root)}, backupRoot / "reserved", {});
	test.Expect(reserved[0].backupPath.filename() != L"CON",
		"reserved device names should be sanitized for backup directories");
}

ConfigBatchCreationDependencies Dependencies(
	int& saveCalls,
	bool saveResult,
	int& refreshCalls) {
	ConfigBatchCreationDependencies dependencies;
	dependencies.buildConfig = BuildRecommendedConfig;
	dependencies.saveConfigs = [&saveCalls, saveResult] {
		++saveCalls;
		return saveResult;
	};
	dependencies.onCommitted = [&refreshCalls](const std::vector<int>&) { ++refreshCalls; };
	return dependencies;
}

void TestBuildAndSaveRollback(TestContext& test, const std::filesystem::path& root) {
	GlobalSnapshot restore;
	Config original;
	original.name = "Existing";
	original.configId = L"11111111-1111-4111-8111-111111111111";
	g_appState.configs = {{7, original}};
	g_appState.currentConfigIndex = 7;
	RestoreNormalConfigIndexAllocator({10});
	g_defaultBackupRootPath = (root / "old-root").wstring();
	g_CoreValidationPending.store(false);
	g_CoreValidationPassed.store(true);

	int buildCalls = 0;
	int saveCalls = 0;
	int refreshCalls = 0;
	ConfigBatchCreationDependencies buildFailure;
	buildFailure.buildConfig = [&](const ConfigDraft& draft, const ConfigFactoryContext& context) {
		if (++buildCalls == 2) throw std::runtime_error("injected build failure");
		return BuildRecommendedConfig(draft, context);
	};
	buildFailure.saveConfigs = [&] { ++saveCalls; return true; };
	buildFailure.onCommitted = [&](const std::vector<int>&) { ++refreshCalls; };
	ConfigBatchCreationRequest request;
	request.defaultBackupRoot = root / "new-root";
	request.drafts = ResolveUniqueConfigDrafts(
		{Draft("One", root), Draft("Two", root), Draft("Three", root)},
		request.defaultBackupRoot, g_appState.configs);
	const auto failedBuild = ConfigBatchCreationService(buildFailure).Commit(request);
	test.Expect(!failedBuild.success && saveCalls == 0 && g_appState.configs.size() == 1
			&& SnapshotNormalConfigIndexAllocator().nextIndex == 10,
		"a later factory failure should not mutate global state or save");

	saveCalls = 0;
	refreshCalls = 0;
	const auto failedSave = ConfigBatchCreationService(
		Dependencies(saveCalls, false, refreshCalls)).Commit(request);
	test.Expect(!failedSave.success && saveCalls == 1 && refreshCalls == 0,
		"a persistence failure should save once and skip refresh");
	test.Expect(g_appState.configs.size() == 1 && g_appState.currentConfigIndex == 7
			&& SnapshotNormalConfigIndexAllocator().nextIndex == 10
			&& g_defaultBackupRootPath == (root / "old-root").wstring()
			&& !g_CoreValidationPending.load() && g_CoreValidationPassed.load(),
		"save failure should restore configs, selection, allocator, settings, and validation flags");

	saveCalls = 0;
	refreshCalls = 0;
	const auto succeeded = ConfigBatchCreationService(
		Dependencies(saveCalls, true, refreshCalls)).Commit(request);
	test.Expect(succeeded.success && succeeded.configIndices == std::vector<int>({10, 11, 12})
			&& saveCalls == 1 && refreshCalls == 1,
		"a valid three-config batch should save and refresh exactly once");
	test.Expect(g_appState.currentConfigIndex == 10 && g_appState.configs.size() == 4
			&& g_appState.configs.at(10).name == request.drafts[0].name
			&& g_appState.configs.at(10).backupPath == request.drafts[0].backupPath.wstring(),
		"successful commit should select the first deterministic config and preserve its path");
	test.Expect(g_defaultBackupRootPath == request.defaultBackupRoot.wstring()
			&& g_CoreValidationPending.load() && !g_CoreValidationPassed.load(),
		"successful onboarding commit should persist the default root and pending validation state");
}

} // namespace

int main() {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path())
		/ ("MineBackupConfigBatch-" + std::to_string(stamp));
	std::filesystem::create_directories(root);
	TestContext test;
	TestFactoryDefaults(test, root);
	TestCompatibilityCreationEntry(test);
	TestNaming(test, root);
	TestBuildAndSaveRollback(test, root);
	std::error_code error;
	std::filesystem::remove_all(root, error);
	if (test.failures != 0) {
		std::cerr << test.failures << " config batch test(s) failed\n";
		return 1;
	}
	std::cout << "All config batch tests passed\n";
	return 0;
}
