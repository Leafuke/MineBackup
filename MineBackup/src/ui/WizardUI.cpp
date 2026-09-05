#include "MainUI.h"

#include "AppPaths.h"
#include "AppState.h"
#include "BatchReadinessService.h"
#include "ConfigBatchCreationService.h"
#include "CoreValidation.h"
#include "DesktopServices.h"
#include "Globals.h"
#include "KnownUserFolders.h"
#include "MinecraftSetupUI.h"
#include "MinecraftInstanceDiscoveryService.h"
#include "TaskCoordinator.h"
#include "UIHelpers.h"
#include "ThemePalette.h"
#include "WizardSession.h"
#include "i18n.h"
#include "imgui-all.h"
#include "text_to_text.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace {

constexpr size_t kWizardPathCapacity = 4096;

enum class ReadinessPurpose {
	Preview,
	Final
};

struct DiscoveryCompletion {
	uint64_t generation = 0;
	MinecraftDiscoveryResult result;
};

struct ReadinessCompletion {
	uint64_t generation = 0;
	ReadinessPurpose purpose = ReadinessPurpose::Preview;
	BatchReadinessResult result;
};

struct WizardMailbox {
	std::mutex mutex;
	optional<DiscoveryCompletion> discovery;
	optional<ReadinessCompletion> readiness;
};

struct WizardRuntime {
	WizardSession session;
	shared_ptr<WizardMailbox> mailbox = make_shared<WizardMailbox>();
	vector<DiscoveryLocation> manualLocations;
	optional<ConfigDraft> advancedCustomDraft;
	array<char, kWizardPathCapacity> backupRootBuffer{};
	uint64_t readinessGeneration = 0;
	wstring scanTaskName;
	wstring readinessTaskName;
	string errorKey;
	bool initialized = false;
	bool windowOpen = true;
	bool scanning = false;
	bool readinessRunning = false;
	bool finalizing = false;
	bool coreValidationStarted = false;
	bool coreValidationStartFailed = false;
	int languageIndex = 0;
};

WizardRuntime& Runtime() {
	static WizardRuntime runtime;
	return runtime;
}

map<int, Config> ConfigSnapshot() {
	lock_guard<mutex> lock(g_appState.configsMutex);
	return g_appState.configs;
}

void SetPathBuffer(array<char, kWizardPathCapacity>& buffer, const filesystem::path& path) {
	const string value = wstring_to_utf8(path.wstring());
	const size_t count = (min)(value.size(), buffer.size() - 1);
	memcpy(buffer.data(), value.data(), count);
	buffer[count] = '\0';
}

const char* EvidenceLabel(const InspectedMinecraftInstance& instance) {
	for (const auto& evidence : instance.evidence) {
		if (evidence.providerId == "pcl2") {
			return L("WIZARD_SOURCE_PCL2");
		}
		if (evidence.providerId == "hmcl") {
			return L("WIZARD_SOURCE_HMCL");
		}
		if (evidence.providerId == "prism-launcher") {
			return L("WIZARD_SOURCE_PRISM");
		}
		if (evidence.providerId == "modrinth") {
			return L("WIZARD_SOURCE_MODRINTH");
		}
		if (evidence.providerId == "netease-minecraft") {
			return L("WIZARD_SOURCE_NETEASE");
		}
	}
	for (const auto& evidence : instance.evidence) {
		if (evidence.kind == DiscoveryEvidenceKind::ExistingConfig) {
			return L("WIZARD_SOURCE_EXISTING_CONFIG");
		}
	}
	for (const auto& evidence : instance.evidence) {
		if (evidence.kind == DiscoveryEvidenceKind::Manual) {
			return L("WIZARD_SOURCE_MANUAL");
		}
	}
	for (const auto& evidence : instance.evidence) {
		if (evidence.kind == DiscoveryEvidenceKind::LauncherProcess
			|| evidence.kind == DiscoveryEvidenceKind::LauncherSettings
			|| evidence.kind == DiscoveryEvidenceKind::WorkspaceProbe) {
			return L("WIZARD_SOURCE_LAUNCHER");
		}
	}
	return L("WIZARD_SOURCE_KNOWN");
}

filesystem::path RecommendedBackupRoot() {
	return KnownUserFolders::Resolver{}.ResolveRecommendedBackupRoot(GetAppPaths());
}

void BeginDiscovery(WizardRuntime& runtime) {
	if (!runtime.scanTaskName.empty()) {
		TaskCoordinator::Instance().RequestStop(runtime.scanTaskName);
	}
	const uint64_t generation = BeginWizardDiscovery(runtime.session);
	runtime.advancedCustomDraft.reset();
	runtime.scanTaskName = L"onboarding-discovery-" + to_wstring(generation);
	runtime.scanning = true;
	runtime.errorKey.clear();
	const auto configs = ConfigSnapshot();
	const auto manualLocations = runtime.manualLocations;
	const auto mailbox = runtime.mailbox;
	const wstring taskName = runtime.scanTaskName;
	const bool submitted = TaskCoordinator::Instance().Submit(
		taskName, {L"minecraft-discovery"},
		[generation, configs, manualLocations, mailbox](stop_token stopToken) mutable {
			MinecraftDiscoveryResult result;
			try {
				auto service = CreateDefaultMinecraftDiscoveryService();
				result = service.Discover(
					configs, std::move(manualLocations), stopToken);
			}
			catch (...) {
				result.diagnostics.push_back({
					"discovery_failed", "aggregator", {}, {}});
			}
			lock_guard<mutex> lock(mailbox->mutex);
			if (!mailbox->discovery
				|| generation >= mailbox->discovery->generation) {
				mailbox->discovery = {generation, std::move(result)};
			}
		});
	if (!submitted) {
		runtime.scanning = false;
		runtime.errorKey = "WIZARD_TASK_SUBMIT_FAILED";
	}
}

const vector<ConfigDraft>& RebuildRuntimeDrafts(WizardRuntime& runtime) {
	if (!runtime.advancedCustomDraft) {
		return RebuildWizardDrafts(runtime.session, ConfigSnapshot());
	}
	runtime.session.drafts = ResolveUniqueConfigDrafts(
		{*runtime.advancedCustomDraft},
		runtime.session.defaultBackupRoot,
		ConfigSnapshot());
	InvalidateWizardReadiness(runtime.session);
	return runtime.session.drafts;
}

void BeginReadiness(WizardRuntime& runtime, ReadinessPurpose purpose) {
	if (!runtime.readinessTaskName.empty()) {
		TaskCoordinator::Instance().RequestStop(runtime.readinessTaskName);
	}
	++runtime.readinessGeneration;
	if (runtime.readinessGeneration == 0) ++runtime.readinessGeneration;
	const uint64_t generation = runtime.readinessGeneration;
	runtime.readinessTaskName = L"onboarding-readiness-" + to_wstring(generation);
	runtime.readinessRunning = true;
	runtime.finalizing = purpose == ReadinessPurpose::Final;
	runtime.errorKey.clear();
	runtime.session.readiness = {};
	const auto drafts = runtime.session.drafts;
	const auto configs = ConfigSnapshot();
	const auto appPaths = GetAppPaths();
	const auto mailbox = runtime.mailbox;
	const wstring taskName = runtime.readinessTaskName;
	const bool submitted = TaskCoordinator::Instance().Submit(
		taskName, {L"minecraft-readiness"},
		[generation, purpose, drafts, configs, appPaths, mailbox](stop_token stopToken) {
			BatchReadinessResult result;
			try {
				result = BatchReadinessService(appPaths).CheckBatch(
					drafts, configs, stopToken);
			}
			catch (...) {
				result.report.issues.push_back({
					"readiness_unexpected_failure",
					ReadinessSeverity::Blocking, {}, {}});
			}
			lock_guard<mutex> lock(mailbox->mutex);
			if (!mailbox->readiness
				|| generation >= mailbox->readiness->generation) {
				mailbox->readiness = {generation, purpose, std::move(result)};
			}
		});
	if (!submitted) {
		runtime.readinessRunning = false;
		runtime.finalizing = false;
		runtime.errorKey = "WIZARD_TASK_SUBMIT_FAILED";
	}
}

void CommitReadyDrafts(WizardRuntime& runtime) {
	ConfigBatchCreationRequest request;
	request.drafts = runtime.session.drafts;
	request.factoryContext.resolvedSevenZip =
		runtime.session.readiness.resolvedSevenZip;
	request.defaultBackupRoot = runtime.session.defaultBackupRoot;
	request.markCoreValidationPending = true;
	const auto result = ConfigBatchCreationService{}.Commit(request);
	if (!result.success) {
		runtime.errorKey = "WIZARD_COMMIT_FAILED";
		return;
	}
	runtime.session.committedConfigIndices = result.configIndices;
	runtime.session.stage = WizardStage::CoreValidation;
	runtime.coreValidationStarted = false;
	runtime.coreValidationStartFailed = false;
	runtime.errorKey.clear();
}

void ConsumeCompletions(WizardRuntime& runtime) {
	optional<DiscoveryCompletion> discovery;
	optional<ReadinessCompletion> readiness;
	{
		lock_guard<mutex> lock(runtime.mailbox->mutex);
		discovery.swap(runtime.mailbox->discovery);
		readiness.swap(runtime.mailbox->readiness);
	}
	if (discovery
		&& ApplyWizardDiscoveryResult(runtime.session,
			discovery->generation, std::move(discovery->result))) {
		runtime.scanning = false;
	}
	if (readiness && readiness->generation == runtime.readinessGeneration) {
		runtime.readinessRunning = false;
		runtime.session.readiness = std::move(readiness->result);
		const bool shouldCommit = readiness->purpose == ReadinessPurpose::Final
			&& runtime.session.readiness.report.ready;
		runtime.finalizing = false;
		if (shouldCommit) CommitReadyDrafts(runtime);
	}
}

void Initialize(WizardRuntime& runtime) {
	if (runtime.initialized) return;
	runtime.initialized = true;
	for (int index = 0; index < 2; ++index) {
		if (g_CurrentLang == lang_codes[index]) runtime.languageIndex = index;
	}
	const filesystem::path defaultRoot = !g_defaultBackupRootPath.empty()
		? filesystem::path(g_defaultBackupRootPath)
		: RecommendedBackupRoot();
	SetWizardDefaultBackupRoot(runtime.session, defaultRoot);
	SetPathBuffer(runtime.backupRootBuffer, defaultRoot);
	BeginDiscovery(runtime);
}

void DrawLanguageSelector(WizardRuntime& runtime) {
	ImGui::TextUnformatted(L("LANGUAGE"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(GetUiMetrics().Em(12.0f));
	if (ImGui::Combo(
		"##WizardLanguage", &runtime.languageIndex, langs, IM_ARRAYSIZE(langs))) {
		SetLanguage(lang_codes[runtime.languageIndex]);
	}
	ImGui::Separator();
}

void DrawDiscoveryStage(WizardRuntime& runtime) {
	ImGui::TextUnformatted(L("WIZARD_DISCOVER_TITLE"));
	ImGui::TextWrapped("%s", L("WIZARD_DISCOVER_DESC"));
	ImGui::Spacing();

	if (runtime.scanning) ImGui::BeginDisabled();
	if (ImGui::Button(L("WIZARD_RESCAN"))) BeginDiscovery(runtime);
	if (runtime.scanning) ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button(L("WIZARD_MANUAL_ADD"))) {
		const auto selected = GetDesktopServices()->SelectFolder();
		if (!selected.path.empty()) {
			runtime.manualLocations.push_back({
				selected.path,
				DiscoveryLocationKind::Manual,
				{{DiscoveryEvidenceKind::Manual, "manual", selected.path}}});
			BeginDiscovery(runtime);
		}
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(runtime.scanning);
	const string selectAllNewLabel =
		string(L("WIZARD_SELECT_ALL_NEW")) + "##WizardSelectAllNew";
	if (ImGui::Button(selectAllNewLabel.c_str())) {
		for (const auto& candidate : runtime.session.discovery.instances) {
			if (candidate.alreadyConfigured) continue;
			SetWizardInstanceSelected(
				runtime.session,
				BuildWizardInstanceKey(candidate.instance), true);
		}
	}
	ImGui::EndDisabled();

	if (runtime.scanning) {
		ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Info),
			"%s", L("WIZARD_SCANNING"));
	}
	// 空结果提示只负责解释“没扫到 Minecraft”；普通文件夹入口
	// 不再挂在这个条件下，发现 Minecraft 时也必须保持可用。
	if (!runtime.scanning && runtime.session.discovery.instances.empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", L("WIZARD_DISCOVERY_EMPTY"));
		ImGui::TextWrapped("%s", L("WIZARD_DISCOVERY_HINT"));
	}
	if (!runtime.errorKey.empty()) {
		ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Error),
			"%s", L(runtime.errorKey.c_str()));
	}

	for (size_t index = 0;
		index < runtime.session.discovery.instances.size(); ++index) {
		const auto& candidate = runtime.session.discovery.instances[index];
		const auto& instance = candidate.instance;
		const wstring key = BuildWizardInstanceKey(instance);
		bool selected = runtime.session.selectedInstanceKeys.contains(key);
		ImGui::PushID(static_cast<int>(index));
		if (candidate.alreadyConfigured) ImGui::BeginDisabled();
		if (ImGui::Checkbox("##Selected", &selected)) {
			SetWizardInstanceSelected(runtime.session, key, selected);
		}
		if (candidate.alreadyConfigured) ImGui::EndDisabled();
		ImGui::SameLine();
		const string name = wstring_to_utf8(instance.suggestedName);
		ImGui::TextUnformatted(name.empty() ? "Minecraft" : name.c_str());
		ImGui::Indent();
		ImGui::Text("%s: %s", L("WIZARD_SOURCE"), EvidenceLabel(instance));
		const wstring count = MineFormatMessage(
			"WIZARD_WORLD_COUNT", instance.worlds.size());
		ImGui::TextUnformatted(wstring_to_utf8(count).c_str());
		if (candidate.alreadyConfigured) {
			ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Muted),
				"%s", L("WIZARD_ALREADY_ADDED"));
		}
		ImGui::Unindent();
		ImGui::Separator();
		ImGui::PopID();
	}

	// 普通文件夹入口与 discovery 结果完全解耦：只要不在扫描中，
	// 即使发现了 Minecraft 实例也始终显示，与候选列表做轻微视觉分隔。
	if (!runtime.scanning) {
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextWrapped("%s", L("WIZARD_ADVANCED_CUSTOM_DESC"));
		if (ImGui::Button(L("WIZARD_ADVANCED_CUSTOM"), ImVec2(-1, 0))) {
			const auto selected = GetDesktopServices()->SelectFolder();
			if (!selected.path.empty()) {
				runtime.advancedCustomDraft = BuildCustomFolderDraft(selected.path);
				if (runtime.advancedCustomDraft) {
					// 保持现有互斥模式：custom folder 取代本次 Minecraft 选择，
					// 不允许一次 onboarding 混合两类配置。
					runtime.session.selectedInstanceKeys.clear();
					RebuildRuntimeDrafts(runtime);
					runtime.session.stage = WizardStage::BackupLocation;
					runtime.errorKey.clear();
				}
				else {
					runtime.errorKey = "WIZARD_ADVANCED_CUSTOM_INVALID";
				}
			}
		}
	}

	// discovery 为空且未选择时，用户仍可走 custom folder 入口，
	// 此时“请选择至少一个实例”没有信息量，不再显示。
	if (!runtime.session.discovery.instances.empty()
		&& runtime.session.selectedInstanceKeys.empty()) {
		ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Warning),
			"%s", L("WIZARD_SELECT_AT_LEAST_ONE"));
	}
	ImGui::BeginDisabled(runtime.scanning
		|| runtime.session.selectedInstanceKeys.empty());
	if (ImGui::Button(L("BUTTON_NEXT"), ImVec2(-1, 0))) {
		runtime.advancedCustomDraft.reset();
		RebuildRuntimeDrafts(runtime);
		if (!runtime.session.drafts.empty()) {
			runtime.session.stage = WizardStage::BackupLocation;
		}
	}
	ImGui::EndDisabled();
}

void DrawBackupStage(WizardRuntime& runtime) {
	ImGui::TextUnformatted(L("WIZARD_BACKUP_TITLE"));
	ImGui::TextWrapped("%s", L("WIZARD_BACKUP_DESC"));
	ImGui::Spacing();

	if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
		const auto selected = GetDesktopServices()->SelectFolder();
		if (!selected.path.empty()) {
			SetWizardDefaultBackupRoot(runtime.session, selected.path);
			SetPathBuffer(runtime.backupRootBuffer, selected.path);
			RebuildRuntimeDrafts(runtime);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_RESTORE_RECOMMENDED"))) {
		const auto recommended = RecommendedBackupRoot();
		SetWizardDefaultBackupRoot(runtime.session, recommended);
		SetPathBuffer(runtime.backupRootBuffer, recommended);
		RebuildRuntimeDrafts(runtime);
	}
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::InputText(
		"##WizardBackupRoot", runtime.backupRootBuffer.data(),
		runtime.backupRootBuffer.size())) {
		SetWizardDefaultBackupRoot(runtime.session,
			filesystem::path(utf8_to_wstring(runtime.backupRootBuffer.data())));
		RebuildRuntimeDrafts(runtime);
	}

	const bool rootValid = !runtime.session.defaultBackupRoot.empty()
		&& runtime.session.defaultBackupRoot.is_absolute();
	if (!rootValid) {
		ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Error),
			"%s", L("WIZARD_BACKUP_ABSOLUTE_REQUIRED"));
	}
	ImGui::Spacing();
	ImGui::TextUnformatted(L("WIZARD_BACKUP_PREVIEW"));
	for (const auto& draft : runtime.session.drafts) {
		ImGui::BulletText("%s", draft.name.c_str());
		ImGui::Indent();
		ImGui::TextWrapped("%s", wstring_to_utf8(draft.backupPath.wstring()).c_str());
		ImGui::Unindent();
	}

	const float width = CalcPairButtonWidth(L("BUTTON_PREVIOUS"), L("BUTTON_NEXT"));
	if (ImGui::Button(L("BUTTON_PREVIOUS"), ImVec2(width, 0))) {
		runtime.advancedCustomDraft.reset();
		runtime.session.drafts.clear();
		InvalidateWizardReadiness(runtime.session);
		runtime.session.stage = WizardStage::Discover;
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!rootValid || runtime.session.drafts.empty());
	if (ImGui::Button(L("BUTTON_NEXT"), ImVec2(width, 0))) {
		runtime.session.stage = WizardStage::Ready;
		BeginReadiness(runtime, ReadinessPurpose::Preview);
	}
	ImGui::EndDisabled();
}

void DrawReadyStage(WizardRuntime& runtime) {
	ImGui::TextUnformatted(L("WIZARD_READY_TITLE"));
	ImGui::TextWrapped("%s", L("WIZARD_READY_DESC"));
	ImGui::Spacing();
	if (runtime.readinessRunning) {
		ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Info), "%s",
			L(runtime.finalizing
				? "WIZARD_FINAL_CHECKING" : "WIZARD_READINESS_CHECKING"));
	}
	else if (runtime.session.readiness.report.ready) {
		ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Success),
			"%s", L("WIZARD_READY_OK"));
	}
	DrawMinecraftReadinessIssues(runtime.session.readiness);
	if (!runtime.errorKey.empty()) {
		ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Error),
			"%s", L(runtime.errorKey.c_str()));
	}

	if (!runtime.readinessRunning && !runtime.session.readiness.report.ready) {
		if (ImGui::Button(L("BUTTON_RETRY"), ImVec2(-1, 0))) {
			BeginReadiness(runtime, ReadinessPurpose::Preview);
		}
	}
	const float width = CalcPairButtonWidth(
		L("BUTTON_PREVIOUS"), L("BUTTON_FINISH_CONFIG"));
	ImGui::BeginDisabled(runtime.readinessRunning);
	if (ImGui::Button(L("BUTTON_PREVIOUS"), ImVec2(width, 0))) {
		runtime.session.stage = WizardStage::BackupLocation;
		InvalidateWizardReadiness(runtime.session);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!runtime.session.readiness.report.ready);
	if (ImGui::Button(L("BUTTON_FINISH_CONFIG"), ImVec2(width, 0))) {
		BeginReadiness(runtime, ReadinessPurpose::Final);
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
}

void FinishWizard(bool& showConfigWizard, bool openSettings) {
	showConfigWizard = false;
	g_OnboardingActive = false;
	g_appState.showMainApp = true;
	if (openSettings) showSettings = true;
}

void DrawCoreValidationStage(WizardRuntime& runtime, bool& showConfigWizard) {
	if (!runtime.coreValidationStarted && !runtime.coreValidationStartFailed) {
		runtime.coreValidationStarted = StartCoreValidationAsync(true);
		runtime.coreValidationStartFailed = !runtime.coreValidationStarted;
	}
	ImGui::TextUnformatted(L("WIZARD_CORE_VALIDATION_TITLE"));
	if (runtime.coreValidationStartFailed) {
		ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Error),
			"%s", L("WIZARD_CORE_VALIDATION_START_FAILED"));
		if (ImGui::Button(L("BUTTON_RETRY"), ImVec2(-1, 0))) {
			runtime.coreValidationStartFailed = false;
		}
		return;
	}
	if (g_CoreValidationRunning.load() || g_CoreValidationPending.load()) {
		ImGui::TextWrapped("%s", L("WIZARD_CORE_VALIDATION_RUNNING"));
		return;
	}
	if (g_CoreValidationPassed.load()) {
		ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Success),
			"%s", L("WIZARD_CORE_VALIDATION_PASSED"));
		if (ImGui::Button(L("WIZARD_ENTER_MAIN"), ImVec2(-1, 0))) {
			FinishWizard(showConfigWizard, false);
		}
		return;
	}

	ImGui::TextColored(ThemePalette::GetStatusColor(ThemePalette::StatusColor::Error),
		"%s", L("WIZARD_CORE_VALIDATION_FAILED"));
	ImGui::TextWrapped("%s", L("WIZARD_CORE_VALIDATION_FAILED_DESC"));
	if (ImGui::Button(L("WIZARD_OPEN_LOGS"), ImVec2(-1, 0))) {
		(void)GetDesktopServices()->OpenFolder(GetAppPaths().logsRoot);
	}
	if (ImGui::Button(L("WIZARD_OPEN_SETTINGS"), ImVec2(-1, 0))) {
		FinishWizard(showConfigWizard, true);
	}
	if (ImGui::Button(L("WIZARD_ENTER_MAIN"), ImVec2(-1, 0))) {
		FinishWizard(showConfigWizard, false);
	}
}

} // namespace

void ShowConfigWizard(bool& showConfigWizard) {
	WizardRuntime& runtime = Runtime();
	Initialize(runtime);
	ConsumeCompletions(runtime);

	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowViewport(viewport->ID);
	ImGui::SetNextWindowPos(
		viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(
		ImVec2(GetUiMetrics().Em(42.0f), GetUiMetrics().Em(34.0f)),
		ImGuiCond_FirstUseEver);
	const bool visible = ImGui::Begin(
		L("WIZARD_TITLE"), &runtime.windowOpen,
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);
	if (visible) {
		DrawLanguageSelector(runtime);
		switch (runtime.session.stage) {
		case WizardStage::Discover:
			DrawDiscoveryStage(runtime);
			break;
		case WizardStage::BackupLocation:
			DrawBackupStage(runtime);
			break;
		case WizardStage::Ready:
			DrawReadyStage(runtime);
			break;
		case WizardStage::CoreValidation:
			DrawCoreValidationStage(runtime, showConfigWizard);
			break;
		}
	}
	ImGui::End();

	if (!runtime.windowOpen) {
		if (!runtime.scanTaskName.empty()) {
			TaskCoordinator::Instance().RequestStop(runtime.scanTaskName);
		}
		if (!runtime.readinessTaskName.empty()) {
			TaskCoordinator::Instance().RequestStop(runtime.readinessTaskName);
		}
		// 完成提交前关闭向导不会调用 SaveConfigs，下一次启动仍进入向导。
		g_appState.done = true;
	}
}
