#include "SettingsUIPrivate.h"

#include "AppPaths.h"
#include "AppState.h"
#include "BatchReadinessService.h"
#include "ConfigBatchCreationService.h"
#include "KnownUserFolders.h"
#include "MinecraftInstanceDiscoveryService.h"
#include "MinecraftSetupUI.h"
#include "TaskCoordinator.h"
#include "WizardSession.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

using namespace std;

namespace {

struct DiscoveryCompletion {
	uint64_t generation = 0;
	MinecraftDiscoveryResult result;
};

struct ReadinessCompletion {
	uint64_t generation = 0;
	BatchReadinessResult result;
};

struct MinecraftSettingsMailbox {
	std::mutex mutex;
	optional<DiscoveryCompletion> discovery;
	optional<ReadinessCompletion> readiness;
};

struct MinecraftSettingsRuntime {
	WizardSession session;
	shared_ptr<MinecraftSettingsMailbox> mailbox =
		make_shared<MinecraftSettingsMailbox>();
	uint64_t readinessGeneration = 0;
	wstring discoveryTaskName;
	wstring readinessTaskName;
	string errorKey;
	size_t lastAddedCount = 0;
	bool scanning = false;
	bool checking = false;
	bool addSucceeded = false;
};

MinecraftSettingsRuntime& MinecraftRuntime() {
	static MinecraftSettingsRuntime runtime;
	return runtime;
}

map<int, Config> ConfigSnapshot() {
	lock_guard<mutex> lock(g_appState.configsMutex);
	return g_appState.configs;
}

filesystem::path CurrentDefaultBackupRoot() {
	if (!g_defaultBackupRootPath.empty()) {
		return filesystem::path(g_defaultBackupRootPath);
	}
	return KnownUserFolders::Resolver{}.ResolveRecommendedBackupRoot(GetAppPaths());
}

void BeginMinecraftDiscovery(
	MinecraftSettingsRuntime& runtime,
	bool preserveSuccess = false) {
	if (!runtime.discoveryTaskName.empty()) {
		TaskCoordinator::Instance().RequestStop(runtime.discoveryTaskName);
	}
	const uint64_t generation = BeginWizardDiscovery(runtime.session);
	runtime.discoveryTaskName = L"settings-minecraft-discovery-" + to_wstring(generation);
	runtime.scanning = true;
	runtime.errorKey.clear();
	if (!preserveSuccess) runtime.addSucceeded = false;
	const auto configs = ConfigSnapshot();
	const auto mailbox = runtime.mailbox;
	const bool submitted = TaskCoordinator::Instance().Submit(
		runtime.discoveryTaskName, {L"minecraft-discovery"},
		[generation, configs, mailbox](stop_token stopToken) {
			MinecraftDiscoveryResult result;
			try {
				result = CreateDefaultMinecraftDiscoveryService().Discover(
					configs, {}, stopToken);
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
		runtime.errorKey = "SETTINGS_MINECRAFT_TASK_FAILED";
	}
}

void BeginMinecraftReadiness(MinecraftSettingsRuntime& runtime) {
	if (!runtime.readinessTaskName.empty()) {
		TaskCoordinator::Instance().RequestStop(runtime.readinessTaskName);
	}
	SetWizardDefaultBackupRoot(runtime.session, CurrentDefaultBackupRoot());
	RebuildWizardDrafts(runtime.session, ConfigSnapshot());
	if (runtime.session.drafts.empty()) {
		runtime.errorKey = "SETTINGS_MINECRAFT_SELECT_REQUIRED";
		return;
	}

	++runtime.readinessGeneration;
	if (runtime.readinessGeneration == 0) ++runtime.readinessGeneration;
	const uint64_t generation = runtime.readinessGeneration;
	runtime.readinessTaskName = L"settings-minecraft-readiness-" + to_wstring(generation);
	runtime.checking = true;
	runtime.addSucceeded = false;
	runtime.errorKey.clear();
	const auto drafts = runtime.session.drafts;
	const auto configs = ConfigSnapshot();
	const auto appPaths = GetAppPaths();
	const auto mailbox = runtime.mailbox;
	const bool submitted = TaskCoordinator::Instance().Submit(
		runtime.readinessTaskName, {L"minecraft-readiness"},
		[generation, drafts, configs, appPaths, mailbox](stop_token stopToken) {
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
				mailbox->readiness = {generation, std::move(result)};
			}
		});
	if (!submitted) {
		runtime.checking = false;
		runtime.errorKey = "SETTINGS_MINECRAFT_TASK_FAILED";
	}
}

void CommitSelectedInstances(MinecraftSettingsRuntime& runtime) {
	ConfigBatchCreationRequest request;
	request.drafts = runtime.session.drafts;
	request.factoryContext.resolvedSevenZip =
		runtime.session.readiness.resolvedSevenZip;
	request.defaultBackupRoot = runtime.session.defaultBackupRoot;
	request.markCoreValidationPending = false;
	const auto result = ConfigBatchCreationService{}.Commit(request);
	if (!result.success) {
		runtime.errorKey = "SETTINGS_MINECRAFT_COMMIT_FAILED";
		return;
	}

	runtime.lastAddedCount = result.configIndices.size();
	runtime.addSucceeded = true;
	runtime.errorKey.clear();
	runtime.session.selectedInstanceKeys.clear();
	NotifySettingsPersistenceCompleted();
	BeginMinecraftDiscovery(runtime, true);
}

void ConsumeMinecraftCompletions(MinecraftSettingsRuntime& runtime) {
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
		runtime.checking = false;
		runtime.session.readiness = std::move(readiness->result);
		if (runtime.session.readiness.report.ready) {
			CommitSelectedInstances(runtime);
		}
	}
}

void DrawMinecraftCandidate(
	MinecraftSettingsRuntime& runtime,
	const DiscoveredMinecraftInstance& candidate,
	int id) {
	const auto& instance = candidate.instance;
	const wstring key = BuildWizardInstanceKey(instance);
	bool selected = runtime.session.selectedInstanceKeys.contains(key);
	ImGui::PushID(id);
	if (candidate.alreadyConfigured || runtime.scanning || runtime.checking) {
		ImGui::BeginDisabled();
	}
	if (ImGui::Checkbox("##MinecraftInstance", &selected)) {
		SetWizardInstanceSelected(runtime.session, key, selected);
		SuppressSettingsAutoSaveForCurrentFrame();
	}
	if (candidate.alreadyConfigured || runtime.scanning || runtime.checking) {
		ImGui::EndDisabled();
	}
	ImGui::SameLine();
	const string name = wstring_to_utf8(instance.suggestedName);
	ImGui::TextUnformatted(name.empty() ? "Minecraft" : name.c_str());
	ImGui::Indent();
	const wstring worldCount = MineFormatMessage(
		"WIZARD_WORLD_COUNT", instance.worlds.size());
	ImGui::TextUnformatted(wstring_to_utf8(worldCount).c_str());
	if (candidate.alreadyConfigured) {
		ImGui::TextColored(ImVec4(0.55f, 0.75f, 0.55f, 1.0f),
			"%s", L("SETTINGS_MINECRAFT_ALREADY_ADDED"));
	}
	ImGui::Unindent();
	ImGui::PopID();
}

void DrawMinecraftInstancesSection() {
	MinecraftSettingsRuntime& runtime = MinecraftRuntime();
	ConsumeMinecraftCompletions(runtime);

	ImGui::Spacing();
	ImGui::SeparatorText(L("SETTINGS_MINECRAFT_TITLE"));
	ImGui::TextWrapped("%s", L("SETTINGS_MINECRAFT_DESC"));
	ImGui::BeginDisabled(runtime.scanning || runtime.checking);
	if (ImGui::Button(L("SETTINGS_MINECRAFT_RESCAN"))) {
		BeginMinecraftDiscovery(runtime);
		SuppressSettingsAutoSaveForCurrentFrame();
	}
	ImGui::EndDisabled();
	if (runtime.scanning) {
		ImGui::TextColored(ImVec4(0.35f, 0.65f, 1.0f, 1.0f),
			"%s", L("SETTINGS_MINECRAFT_SCANNING"));
	}
	if (runtime.checking) {
		ImGui::TextColored(ImVec4(0.35f, 0.65f, 1.0f, 1.0f),
			"%s", L("SETTINGS_MINECRAFT_CHECKING"));
	}
	if (runtime.addSucceeded) {
		const wstring message = MineFormatMessage(
			"SETTINGS_MINECRAFT_ADDED_SUCCESS", runtime.lastAddedCount);
		ImGui::TextColored(ImVec4(0.35f, 0.8f, 0.45f, 1.0f),
			"%s", wstring_to_utf8(message).c_str());
	}
	if (!runtime.errorKey.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
			"%s", L(runtime.errorKey.c_str()));
	}

	if (!runtime.scanning && runtime.session.scanGeneration != 0
		&& runtime.session.discovery.instances.empty()) {
		ImGui::TextWrapped("%s", L("SETTINGS_MINECRAFT_EMPTY"));
	}

	const auto drawGroup = [&](bool alreadyConfigured, const char* headingKey) {
		const bool any = any_of(
			runtime.session.discovery.instances.begin(),
			runtime.session.discovery.instances.end(),
			[&](const auto& candidate) {
				return candidate.alreadyConfigured == alreadyConfigured;
			});
		if (!any) return;
		ImGui::TextUnformatted(L(headingKey));
		for (size_t index = 0;
			index < runtime.session.discovery.instances.size(); ++index) {
			const auto& candidate = runtime.session.discovery.instances[index];
			if (candidate.alreadyConfigured != alreadyConfigured) continue;
			DrawMinecraftCandidate(runtime, candidate, static_cast<int>(index));
		}
	};
	drawGroup(false, "SETTINGS_MINECRAFT_NEW");
	drawGroup(true, "SETTINGS_MINECRAFT_ADDED");

	if (!runtime.session.readiness.report.ready) {
		DrawMinecraftReadinessIssues(runtime.session.readiness);
	}
	const bool canAdd = !runtime.session.selectedInstanceKeys.empty()
		&& !runtime.scanning && !runtime.checking;
	ImGui::BeginDisabled(!canAdd);
	if (ImGui::Button(L("SETTINGS_MINECRAFT_ADD_SELECTED"), ImVec2(-1, 0))) {
		BeginMinecraftReadiness(runtime);
		SuppressSettingsAutoSaveForCurrentFrame();
	}
	ImGui::EndDisabled();
}

} // namespace

void DrawApplicationSettings() {
	ImGui::SeparatorText(L("DEFAULT_BACKUP_ROOT_TITLE"));
	ImGui::TextWrapped("%s", L("DEFAULT_BACKUP_ROOT_DESCRIPTION"));
	ImGui::Spacing();

	const filesystem::path recommended =
		KnownUserFolders::Resolver{}.ResolveRecommendedBackupRoot(GetAppPaths());
	if (g_defaultBackupRootPath.empty() && !recommended.empty()) {
		g_defaultBackupRootPath = recommended.wstring();
	}

	const string currentPath = wstring_to_utf8(g_defaultBackupRootPath);
	ImGui::TextWrapped("%s", currentPath.c_str());
	ImGui::Spacing();
	if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
		const auto selected = GetDesktopServices()->SelectFolder().path;
		if (!selected.empty() && selected.is_absolute()) {
			g_defaultBackupRootPath = selected.lexically_normal().wstring();
		}
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(recommended.empty());
	if (ImGui::Button(L("BUTTON_RESTORE_RECOMMENDED"))) {
		g_defaultBackupRootPath = recommended.wstring();
	}
	ImGui::EndDisabled();

	DrawMinecraftInstancesSection();
}
