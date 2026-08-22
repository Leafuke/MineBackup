#include "AppState.h"
#include "BackupManager.h"
#include "ConfigBatchCreationService.h"
#include "ConfigFactory.h"
#include "ConfigManager.h"
#include "CoreValidation.h"
#include "Globals.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
	ConfigSaveState saveState,
	int& refreshCalls) {
	ConfigBatchCreationDependencies dependencies;
	dependencies.buildConfig = BuildRecommendedConfig;
	dependencies.saveConfigs = [&saveCalls, saveState] {
		++saveCalls;
		return ConfigSaveResult{saveState, L""};
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
	buildFailure.saveConfigs = [&]() -> ConfigSaveResult {
		++saveCalls;
		return {ConfigSaveState::CommittedDurably, L""};
	};
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
		Dependencies(saveCalls, ConfigSaveState::NotCommitted, refreshCalls)).Commit(request);
	test.Expect(!failedSave.success && saveCalls == 1 && refreshCalls == 0,
		"a persistence failure should save once and skip refresh");
	test.Expect(g_appState.configs.size() == 1 && g_appState.currentConfigIndex == 7
			&& SnapshotNormalConfigIndexAllocator().nextIndex == 10
			&& g_defaultBackupRootPath == (root / "old-root").wstring()
			&& !g_CoreValidationPending.load() && g_CoreValidationPassed.load(),
		"save failure should restore configs, selection, allocator, settings, and validation flags");

	// 注入抛出异常的 saveConfigs：无法确定 commit point，按 NotCommitted 回滚。
	saveCalls = 0;
	refreshCalls = 0;
	ConfigBatchCreationDependencies throwingSave;
	throwingSave.buildConfig = BuildRecommendedConfig;
	throwingSave.saveConfigs = [&]() -> ConfigSaveResult {
		++saveCalls;
		throw std::runtime_error("injected persistence failure");
	};
	throwingSave.onCommitted = [&refreshCalls](const std::vector<int>&) { ++refreshCalls; };
	const auto thrownSave = ConfigBatchCreationService(throwingSave).Commit(request);
	test.Expect(!thrownSave.success && thrownSave.errorCode == "minecraft.config_batch.commit_failed"
			&& saveCalls == 1 && refreshCalls == 0,
		"an exceptional save dependency should be treated as not committed");
	test.Expect(g_appState.configs.size() == 1 && g_appState.currentConfigIndex == 7
			&& SnapshotNormalConfigIndexAllocator().nextIndex == 10,
		"an exceptional save dependency should restore the memory state");

	saveCalls = 0;
	refreshCalls = 0;
	const auto succeeded = ConfigBatchCreationService(
		Dependencies(saveCalls, ConfigSaveState::CommittedDurably, refreshCalls)).Commit(request);
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

// CommittedNotDurable：config.ini 已替换、目录同步未确认。
// 内存状态必须向磁盘提交状态收敛，任何回滚都会造成 disk-new/memory-old 撕裂。
void TestCommittedNotDurableKeepsMemoryState(
	TestContext& test,
	const std::filesystem::path& root) {
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

	int saveCalls = 0;
	int refreshCalls = 0;
	ConfigBatchCreationRequest request;
	request.defaultBackupRoot = root / "durable-root";
	request.drafts = ResolveUniqueConfigDrafts(
		{Draft("Durable", root)}, request.defaultBackupRoot, g_appState.configs);
	const auto result = ConfigBatchCreationService(
		Dependencies(saveCalls, ConfigSaveState::CommittedNotDurable, refreshCalls))
		.Commit(request);

	test.Expect(result.success && result.warningCode == "minecraft.config_batch.commit_not_durable"
			&& result.configIndices.size() == 1 && result.errorCode.empty(),
		"a committed-but-not-durable save must succeed with a non-blocking warning");
	test.Expect(saveCalls == 1 && refreshCalls == 1,
		"a committed-but-not-durable save should still run onCommitted exactly once");
	test.Expect(g_appState.configs.size() == 2 && g_appState.currentConfigIndex == 10
			&& SnapshotNormalConfigIndexAllocator().nextIndex == 11
			&& g_appState.configs.at(10).name == "Durable",
		"a committed-but-not-durable save must keep new configs, selection, and allocator");
	test.Expect(g_defaultBackupRootPath == request.defaultBackupRoot.wstring()
			&& g_CoreValidationPending.load() && !g_CoreValidationPassed.load(),
		"a committed-but-not-durable save must keep backup root and validation flags");
}

void TestSettingsCommitPreservesValidationState(
	TestContext& test,
	const std::filesystem::path& root) {
	GlobalSnapshot restore;
	g_appState.configs.clear();
	g_appState.currentConfigIndex = 1;
	RestoreNormalConfigIndexAllocator({1});
	g_CoreValidationPending.store(false);
	g_CoreValidationPassed.store(true);

	ConfigBatchCreationRequest request;
	request.defaultBackupRoot = root / "settings-root";
	request.drafts = ResolveUniqueConfigDrafts(
		{Draft("Settings", root)}, request.defaultBackupRoot, {});
	request.markCoreValidationPending = false;
	int saveCalls = 0;
	int refreshCalls = 0;
	const auto result = ConfigBatchCreationService(
		Dependencies(saveCalls, ConfigSaveState::CommittedDurably, refreshCalls)).Commit(request);
		test.Expect(result.success && saveCalls == 1 && refreshCalls == 1
			&& !g_CoreValidationPending.load() && g_CoreValidationPassed.load(),
			"Settings batch creation should save once without scheduling onboarding validation");
}

void TestCoreValidationContracts(TestContext& test, const std::filesystem::path& root) {
	test.Expect(ResolveDesktopConfigIndex(-1, 3) == 3,
		"legacy config index -1 should fall back to the current config");
	test.Expect(ResolveDesktopConfigIndex(0, 3) == 0
			&& ResolveDesktopConfigIndex(2, 3) == 2
			&& ResolveDesktopConfigIndex(-424242, 3) == -424242
			&& ResolveDesktopConfigIndex(std::numeric_limits<int>::min(), 3)
				== std::numeric_limits<int>::min(),
		"explicit config identities, including negative synthetic indices, must be preserved");

	HistoryEntry stale;
	stale.configId = L"real-config";
	stale.timestamp_str = L"2026-08-21T00:00:00";
	stale.worldPath = (root / "MineBackup_CoreValidation" / "run-1" / "worlds" / "__CoreValidationSmart").wstring();
	stale.worldName = L"__CoreValidationSmart";
	stale.backupFile = L"[Full][2026-08-21_00-00-00]__CoreValidationSmart [CoreValidation_Base].7z";
	stale.backupType = L"Full";
	stale.comment = L"CoreValidation_Base";
	test.Expect(IsLegacyCoreValidationPollution(stale),
		"the strict classifier should recognize a stale core-validation history entry");

	HistoryEntry limit = stale;
	limit.worldName = L"__CoreValidationLimit";
	limit.worldPath = (root / "MineBackup_CoreValidation" / "run-2" / "worlds" / "__CoreValidationLimit").wstring();
	limit.backupFile = L"[Smart][2026-08-21_00-00-00]__CoreValidationLimit [CoreValidation_Limit_1].7z";
	limit.backupType = L"Smart";
	limit.comment = L"CoreValidation_Limit_1";
	test.Expect(IsLegacyCoreValidationPollution(limit),
		"the strict classifier should recognize both synthetic validation worlds");

	HistoryEntry sameNameReal = stale;
	sameNameReal.worldPath = L"D:/Minecraft/saves/__CoreValidationSmart";
	test.Expect(!IsLegacyCoreValidationPollution(sameNameReal),
		"a real world with a synthetic-looking name must not be classified as pollution");
	HistoryEntry pathSimilar = stale;
	pathSimilar.worldPath = L"D:/Backups/My_MineBackup_CoreValidation_notes/worlds/__CoreValidationSmart";
	test.Expect(!IsLegacyCoreValidationPollution(pathSimilar),
		"a path with only a textual sandbox-name match must not be classified as pollution");
	HistoryEntry commentMismatch = stale;
	commentMismatch.comment = L"UserCreated";
	test.Expect(!IsLegacyCoreValidationPollution(commentMismatch),
		"a normal comment must protect a matching world from cleanup");

	std::vector<HistoryEntry> entries{stale, sameNameReal};
	test.Expect(RemoveLegacyCoreValidationPollution(entries) == 1 && entries.size() == 1,
		"legacy pollution cleanup should remove only the high-confidence entry");
	test.Expect(RemoveLegacyCoreValidationPollution(entries) == 0 && entries.front().worldPath == sameNameReal.worldPath,
		"legacy pollution cleanup should be idempotent and preserve normal history");

	CoreValidationHistorySnapshot before;
	before[L"config-a"] = {stale};
	CoreValidationHistorySnapshot unchanged = before;
	std::size_t changedConfigs = 0;
	test.Expect(AreCoreValidationHistorySnapshotsEqual(before, unchanged, &changedConfigs)
			&& changedConfigs == 0,
		"history snapshots with identical semantic fields should compare equal");
	unchanged[L"config-a"][0].comment = L"different";
	test.Expect(!AreCoreValidationHistorySnapshotsEqual(before, unchanged, &changedConfigs)
			&& changedConfigs == 1,
		"history isolation comparison must detect content changes with unchanged counts");
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
	TestCommittedNotDurableKeepsMemoryState(test, root);
	TestSettingsCommitPreservesValidationState(test, root);
	TestCoreValidationContracts(test, root);
	std::error_code error;
	std::filesystem::remove_all(root, error);
	if (test.failures != 0) {
		std::cerr << test.failures << " config batch test(s) failed\n";
		return 1;
	}
	std::cout << "All config batch tests passed\n";
	return 0;
}
