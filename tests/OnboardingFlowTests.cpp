#include "AppPaths.h"
#include "AppState.h"
#include "BatchReadinessService.h"
#include "ConfigBatchCreationService.h"
#include "ConfigManager.h"
#include "Globals.h"
#include "WizardSession.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

struct TestContext {
	int failures = 0;

	void Expect(bool condition, const char* message) {
		if (condition) return;
		++failures;
		std::cerr << "FAIL: " << message << '\n';
	}
};

AppPaths BuildTestAppPaths(const std::filesystem::path& root) {
	AppPaths paths;
	paths.configRoot = root / "profile" / "config";
	paths.dataRoot = root / "profile" / "data";
	paths.stateRoot = root / "profile" / "state";
	paths.cacheRoot = root / "profile" / "cache";
	paths.runtimeRoot = root / "profile" / "runtime";
	paths.toolsRoot = root / "profile" / "tools";
	paths.resourcesRoot = root / "resources";
	paths.logsRoot = root / "profile" / "logs";
	paths.profileIdentity = L"onboarding-flow-test";
	paths.mode = AppPathMode::Explicit;
	return paths;
}

InspectedMinecraftInstance CreateJavaInstance(
	const std::filesystem::path& root,
	const std::wstring& suggestedName) {
	const auto savesRoot = root / "saves";
	const auto worldRoot = savesRoot / "World";
	std::filesystem::create_directories(worldRoot);
	std::ofstream(worldRoot / "level.dat", std::ios::binary) << "test-world";

	InspectedMinecraftInstance instance;
	instance.edition = MinecraftEdition::Java;
	instance.instanceRoot = root;
	instance.savesRoot = savesRoot;
	instance.suggestedName = suggestedName;
	instance.worlds.push_back({
		worldRoot,
		std::filesystem::path(L"World"),
		L"World",
		L"Test World"});
	return instance;
}

void TestFirstRunSelectionAndCommit(
	TestContext& test,
	const std::filesystem::path& root) {
	const AppPaths appPaths = BuildTestAppPaths(root);
	std::filesystem::create_directories(appPaths.configRoot);
	std::filesystem::create_directories(appPaths.dataRoot);
	SetCurrentAppPaths(appPaths);

	g_appState.configs.clear();
	g_appState.jobs = {};
	g_appState.currentConfigIndex = 1;
	RestoreNormalConfigIndexAllocator({1});
	g_defaultBackupRootPath.clear();
	g_CoreValidationPending.store(false);
	g_CoreValidationPassed.store(false);

	MinecraftDiscoveryResult discovery;
	discovery.instances.push_back({
		CreateJavaInstance(root / "instances" / "One", L"Minecraft"), false});
	discovery.instances.push_back({
		CreateJavaInstance(root / "instances" / "Two", L"Minecraft"), false});

	WizardSession session;
	const auto generation = BeginWizardDiscovery(session);
	test.Expect(ApplyWizardDiscoveryResult(session, generation, std::move(discovery)),
		"the current discovery generation should be accepted");
	const auto backupRoot = root / "recommended-backups";
	SetWizardDefaultBackupRoot(session, backupRoot);
	for (const auto& candidate : session.discovery.instances) {
		test.Expect(SetWizardInstanceSelected(
			session, BuildWizardInstanceKey(candidate.instance), true),
			"each discovered instance should be selectable");
	}
	const auto& drafts = RebuildWizardDrafts(session, g_appState.configs);
	test.Expect(drafts.size() == 2 && drafts[0].name != drafts[1].name
			&& drafts[0].backupPath != drafts[1].backupPath,
		"selected instances should receive independent names and backup directories");
	test.Expect(!std::filesystem::exists(appPaths.ConfigFile())
			&& !std::filesystem::exists(backupRoot),
		"selection alone must not persist configuration or create backup directories");

	int resolutionCalls = 0;
	BatchReadinessDependencies readinessDependencies;
	readinessDependencies.resolveSevenZip = [&](std::stop_token) {
		++resolutionCalls;
		ExternalToolResolution resolution;
		resolution.available = true;
		resolution.executable = appPaths.toolsRoot / "7za-test.exe";
		resolution.source = ExternalToolSource::Managed;
		return resolution;
	};
	// 使用真实写探针验证临时目录会被清理；仅替换外部工具解析以保持测试离线。
	session.readiness = BatchReadinessService(
		appPaths, std::move(readinessDependencies))
		.CheckBatch(session.drafts, g_appState.configs);
	test.Expect(session.readiness.report.ready && resolutionCalls == 1,
		"the selected batch should resolve 7-Zip once and pass readiness");
	test.Expect(!std::filesystem::exists(appPaths.ConfigFile())
			&& !std::filesystem::exists(backupRoot),
		"readiness probes must leave neither configuration nor empty backup directories");

	ConfigBatchCreationRequest request;
	request.drafts = session.drafts;
	request.factoryContext.resolvedSevenZip = session.readiness.resolvedSevenZip;
	request.defaultBackupRoot = session.defaultBackupRoot;
	ConfigBatchCreationDependencies commitDependencies;
	// 与生产默认一致：返回详细持久化状态，而非布尔值。
	commitDependencies.saveConfigs = [] { return SaveConfigsDetailed(); };
	commitDependencies.onCommitted = [](const std::vector<int>&) {};
	const auto committed = ConfigBatchCreationService(
		std::move(commitDependencies)).Commit(request);
	test.Expect(committed.success && committed.configIndices.size() == 2,
		"the ready batch should be committed atomically");
	test.Expect(std::filesystem::exists(appPaths.ConfigFile())
			&& g_appState.configs.size() == 2
			&& g_appState.currentConfigIndex == committed.configIndices.front(),
		"one successful commit should persist both independent configurations");
	test.Expect(g_CoreValidationPending.load() && !g_CoreValidationPassed.load(),
		"first-run commit should schedule visible core validation");

	const Config& first = g_appState.configs.at(committed.configIndices[0]);
	const Config& second = g_appState.configs.at(committed.configIndices[1]);
	test.Expect(first.keepCount == 20 && second.keepCount == 20
			&& first.configId != second.configId
			&& first.backupPath != second.backupPath,
		"created configurations should preserve recommended defaults and unique identity");

	// 核心验证失败只更新状态；已提交的用户配置必须继续可用并可重新验证。
	g_CoreValidationPending.store(false);
	g_CoreValidationPassed.store(false);
	test.Expect(SaveConfigs(), "a core-validation failure state should remain persistable");
	g_appState.configs.clear();
	LoadConfigs();
	test.Expect(g_appState.configs.size() == 2
			&& !g_CoreValidationPending.load() && !g_CoreValidationPassed.load(),
		"reloading after validation failure must retain every committed configuration");
	for (const auto& [index, config] : g_appState.configs) {
		(void)index;
		test.Expect(config.keepCount == 20 && !config.configId.empty(),
			"round-tripped onboarding configurations should retain their defaults and identity");
	}
}

// 行为级回归：discovery 已有有效 Minecraft candidate 时改走 custom folder。
// UI 可见性由 DrawDiscoveryStage 的条件修改保证（无法在测试中点击 ImGui 按钮）；
// 这里保证“有候选仍可走 custom path”在业务模型上完全成立。
void TestCustomFolderWithDiscoveredCandidate(
	TestContext& test,
	const std::filesystem::path& root) {
	const AppPaths appPaths = BuildTestAppPaths(root / "custom-profile");
	std::filesystem::create_directories(appPaths.configRoot);
	std::filesystem::create_directories(appPaths.dataRoot);
	SetCurrentAppPaths(appPaths);

	g_appState.configs.clear();
	g_appState.jobs = {};
	g_appState.currentConfigIndex = 1;
	RestoreNormalConfigIndexAllocator({1});
	g_defaultBackupRootPath.clear();
	g_CoreValidationPending.store(false);
	g_CoreValidationPassed.store(false);

	MinecraftDiscoveryResult discovery;
	discovery.instances.push_back({
		CreateJavaInstance(root / "custom-instances" / "Discovered", L"Minecraft"),
		false});
	WizardSession session;
	const auto generation = BeginWizardDiscovery(session);
	test.Expect(ApplyWizardDiscoveryResult(session, generation, std::move(discovery)),
		"the custom-path scenario should start with one discovered candidate");
	test.Expect(!session.discovery.instances.empty(),
		"the custom-path scenario requires a discovered Minecraft candidate");
	const auto backupRoot = root / "custom-backups";
	SetWizardDefaultBackupRoot(session, backupRoot);
	// 用户不选择任何 Minecraft candidate，直接改走 custom folder 入口。
	session.selectedInstanceKeys.clear();

	// 与 UI 点击 custom folder 后相同的业务模型：
	// BuildCustomFolderDraft -> 互斥清空 selection -> 仅保留 custom draft。
	const auto customFolder = root / "custom-data" / "ProjectX";
	std::filesystem::create_directories(customFolder);
	const auto customDraft = BuildCustomFolderDraft(customFolder);
	test.Expect(customDraft.has_value(),
		"a regular folder below the drive root should yield a custom draft");
	session.drafts = ResolveUniqueConfigDrafts(
		{*customDraft}, session.defaultBackupRoot, g_appState.configs);
	test.Expect(session.drafts.size() == 1,
		"the custom path should produce exactly one draft");

	BatchReadinessDependencies readinessDependencies;
	readinessDependencies.resolveSevenZip = [&](std::stop_token) {
		ExternalToolResolution resolution;
		resolution.available = true;
		resolution.executable = appPaths.toolsRoot / "7za-test.exe";
		resolution.source = ExternalToolSource::Managed;
		return resolution;
	};
	session.readiness = BatchReadinessService(appPaths, std::move(readinessDependencies))
		.CheckBatch(session.drafts, g_appState.configs);
	test.Expect(session.readiness.report.ready,
		"the custom-folder draft should pass full readiness without level.dat");

	ConfigBatchCreationRequest request;
	request.drafts = session.drafts;
	request.factoryContext.resolvedSevenZip = session.readiness.resolvedSevenZip;
	request.defaultBackupRoot = session.defaultBackupRoot;
	ConfigBatchCreationDependencies commitDependencies;
	commitDependencies.saveConfigs = [] { return SaveConfigsDetailed(); };
	commitDependencies.onCommitted = [](const std::vector<int>&) {};
	const auto committed = ConfigBatchCreationService(
		std::move(commitDependencies)).Commit(request);
	test.Expect(committed.success && committed.configIndices.size() == 1
			&& g_appState.configs.size() == 1,
		"the custom path should create exactly one configuration");
	test.Expect(g_appState.configs.size() == 1,
		"the discovered Minecraft candidate must not be configured implicitly");

	const Config& custom = g_appState.configs.at(committed.configIndices.front());
	test.Expect(custom.name == "ProjectX"
			&& custom.saveRoot == customFolder.parent_path().wstring()
			&& custom.worlds.size() == 1
			&& custom.worlds.front().first == customFolder.filename().wstring(),
		"the custom config should keep the folder's parent as save root with a single world");
	test.Expect(g_defaultBackupRootPath == backupRoot.wstring(),
		"the custom onboarding commit should persist the chosen default backup root");

	// Save -> Load round trip：重启后配置保持正确。
	g_appState.configs.clear();
	LoadConfigs();
	test.Expect(g_appState.configs.size() == 1
			&& g_appState.configs.begin()->second.saveRoot
				== customFolder.parent_path().wstring()
			&& !g_appState.configs.begin()->second.configId.empty(),
		"the custom config should round-trip through Save/Load with identity");
}

} // namespace

int main() {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path())
		/ ("MineBackupOnboardingFlow-" + std::to_string(stamp));
	std::filesystem::create_directories(root);

	TestContext test;
	TestFirstRunSelectionAndCommit(test, root);
	TestCustomFolderWithDiscoveredCandidate(test, root);

	std::error_code error;
	std::filesystem::remove_all(root, error);
	if (test.failures != 0) {
		std::cerr << test.failures << " onboarding flow test(s) failed\n";
		return 1;
	}
	std::cout << "All onboarding flow tests passed\n";
	return 0;
}
