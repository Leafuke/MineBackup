#include "SettingsUIPrivate.h"

using namespace std;

void DrawUnifiedTaskManager(SpecialConfig& spCfg) {
	ImGui::SeparatorText(L("TASK_MANAGER_TITLE"));

	static int selectedTaskIndex = -1;
	static bool narrowShowEditor = false;
	const UiMetrics metrics = GetUiMetrics();
	const bool wideLayout = ImGui::GetContentRegionAvail().x >= metrics.Em(52.0f);

	auto addTask = [&](SpecialTaskType type) {
		SpecialTask newTask;
		newTask.taskId = FolderRewindFormat::GenerateGuidString();
		const int displayId = static_cast<int>(spCfg.specialTasks.size()) + 1;
		const char* nameKey = type == SpecialTaskType::Backup
			? "TASK_DEFAULT_BACKUP_NAME" : "TASK_DEFAULT_COMMAND_NAME";
		newTask.name = wstring_to_utf8(MineFormatMessage(nameKey, displayId));
		newTask.type = type;
		if (type == SpecialTaskType::Backup && !g_appState.configs.empty()) {
			const auto& [index, config] = *g_appState.configs.begin();
			(void)index;
			newTask.target.configId = config.configId;
			if (!config.worlds.empty()) {
				SpecialTaskStorage::TryNormalizeWorldPath(
					config.worlds.front().first, newTask.target.worldPath);
			}
		}
		spCfg.specialTasks.push_back(newTask);
		selectedTaskIndex = static_cast<int>(spCfg.specialTasks.size()) - 1;
		narrowShowEditor = !wideLayout;
	};

	if (ImGui::Button(L("TASK_ADD_MENU"))) ImGui::OpenPopup("##AddTask");
	if (ImGui::BeginPopup("##AddTask")) {
		if (ImGui::MenuItem(L("TASK_ADD_BACKUP"))) addTask(SpecialTaskType::Backup);
		if (ImGui::MenuItem(L("TASK_ADD_COMMAND"))) addTask(SpecialTaskType::Command);
		ImGui::BeginDisabled();
		ImGui::MenuItem(L("TASK_ADD_SCRIPT"));
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", L("TASK_SCRIPT_NOT_IMPLEMENTED"));
		}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(selectedTaskIndex < 0);
	if (ImGui::Button(L("TASK_REMOVE")) && selectedTaskIndex < static_cast<int>(spCfg.specialTasks.size())) {
		spCfg.specialTasks.erase(spCfg.specialTasks.begin() + selectedTaskIndex);
		selectedTaskIndex = -1;
		narrowShowEditor = false;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(selectedTaskIndex <= 0);
	if (ImGui::Button("^##TaskUp")) {
		swap(spCfg.specialTasks[selectedTaskIndex], spCfg.specialTasks[selectedTaskIndex - 1]);
		selectedTaskIndex--;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("%s", L("TASK_MOVE_UP"));
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(selectedTaskIndex < 0 || selectedTaskIndex >= static_cast<int>(spCfg.specialTasks.size()) - 1);
	if (ImGui::Button("v##TaskDown")) {
		swap(spCfg.specialTasks[selectedTaskIndex], spCfg.specialTasks[selectedTaskIndex + 1]);
		selectedTaskIndex++;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("%s", L("TASK_MOVE_DOWN"));
	}
	ImGui::EndDisabled();

	ImGui::Spacing();

	if (!wideLayout && narrowShowEditor && ImGui::Button(L("TASK_BACK_TO_LIST"))) {
		narrowShowEditor = false;
	}

	if (wideLayout || !narrowShowEditor) {
		const float listWidth = wideLayout ? metrics.Em(20.0f) : ImGui::GetContentRegionAvail().x;
		ImGui::BeginChild("##TaskListPane", ImVec2(listWidth, wideLayout ? metrics.Em(28.0f) : 0.0f),
			ImGuiChildFlags_Borders);
	if (ImGui::BeginTable("TasksTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, metrics.Em(2.0f));
		ImGui::TableSetupColumn(L("TASK_NAME"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();

		for (int i = 0; i < static_cast<int>(spCfg.specialTasks.size()); ++i) {
			auto& task = spCfg.specialTasks[i];
			ImGui::TableNextRow();
			ImGui::PushID(i);

			ImGui::TableSetColumnIndex(0);
			if (ImGui::Selectable(to_string(i + 1).c_str(), selectedTaskIndex == i, ImGuiSelectableFlags_SpanAllColumns)) {
				selectedTaskIndex = i;
				narrowShowEditor = !wideLayout;
			}

			ImGui::TableSetColumnIndex(1);
			TextEllipsisWithTooltip(task.name.c_str(), ImGui::GetContentRegionAvail().x);

			ImGui::PopID();
		}

		ImGui::EndTable();
	}
		ImGui::EndChild();
	}

	if (wideLayout) {
		ImGui::SameLine();
		ImGui::BeginChild("##TaskEditorPane", ImVec2(0.0f, metrics.Em(28.0f)),
			ImGuiChildFlags_Borders);
	}

	if (selectedTaskIndex >= 0 && selectedTaskIndex < static_cast<int>(spCfg.specialTasks.size())
		&& (wideLayout || narrowShowEditor)) {
		auto& task = spCfg.specialTasks[selectedTaskIndex];

		ImGui::Spacing();
		ImGui::SeparatorText(L("TASK_DETAILS"));

		char nameBuf[128];
		strncpy_s(nameBuf, task.name.c_str(), sizeof(nameBuf));
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText(L("TASK_NAME"), nameBuf, sizeof(nameBuf))) {
			task.name = nameBuf;
		}
		if (ImGui::SmallButton(L("TASK_COPY_ID"))) {
			ImGui::SetClipboardText(wstring_to_utf8(task.taskId).c_str());
		}

		int execMode = static_cast<int>(task.executionMode);
		ImGui::Text("%s", L("TASK_EXEC_MODE_LABEL"));
		ImGui::RadioButton(L("TASK_EXEC_SEQUENTIAL"), &execMode, 0);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_TASK_EXEC_SEQUENTIAL"));
		ImGui::SameLine();
		ImGui::RadioButton(L("TASK_EXEC_PARALLEL"), &execMode, 1);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_TASK_EXEC_PARALLEL"));
		task.executionMode = static_cast<SpecialTaskExecutionMode>(execMode);

		ImGui::Spacing();

		if (task.type == SpecialTaskType::Backup) {
			auto selectedConfig = find_if(g_appState.configs.begin(), g_appState.configs.end(),
				[&](const auto& item) { return item.second.configId == task.target.configId; });
			string current_config_label = selectedConfig != g_appState.configs.end()
				? (string(L("CONFIG_N")) + to_string(selectedConfig->first))
				: L("TASK_NONE");
			SetStandardControlWidth();
			if (ImGui::BeginCombo(L("CONFIG_COMBO"), current_config_label.c_str())) {
				for (auto const& [idx, val] : g_appState.configs) {
					if (ImGui::Selectable((string(L("CONFIG_N")) + to_string(idx) + " - " + val.name).c_str(), task.target.configId == val.configId)) {
						task.target.configId = val.configId;
						task.target.worldPath.clear();
						if (!val.worlds.empty()) SpecialTaskStorage::TryNormalizeWorldPath(
							val.worlds.front().first, task.target.worldPath);
					}
				}
				ImGui::EndCombo();
			}

			selectedConfig = find_if(g_appState.configs.begin(), g_appState.configs.end(),
				[&](const auto& item) { return item.second.configId == task.target.configId; });
			if (selectedConfig != g_appState.configs.end()) {
				Config& selected_cfg = selectedConfig->second;
				string current_world_label = L("TASK_NONE");
				for (const auto& world : selected_cfg.worlds) {
					wstring normalized;
					if (SpecialTaskStorage::TryNormalizeWorldPath(world.first, normalized)
						&& normalized == task.target.worldPath) {
						current_world_label = wstring_to_utf8(world.first);
						break;
					}
				}
				SetStandardControlWidth();
				if (ImGui::BeginCombo(L("WORLD_COMBO"), current_world_label.c_str())) {
					for (int w_idx = 0; w_idx < static_cast<int>(selected_cfg.worlds.size()); ++w_idx) {
						wstring normalized;
						SpecialTaskStorage::TryNormalizeWorldPath(
							selected_cfg.worlds[w_idx].first, normalized);
						if (ImGui::Selectable(wstring_to_utf8(selected_cfg.worlds[w_idx].first).c_str(), task.target.worldPath == normalized)) {
							task.target.worldPath = std::move(normalized);
						}
					}
					ImGui::EndCombo();
				}
			}
		}
		else if (task.type == SpecialTaskType::Command) {
			ImGui::TextWrapped("%s", L("TASK_COMMAND_WARNING"));
			char cmdBuf[512];
			strncpy_s(cmdBuf, wstring_to_utf8(task.command).c_str(), sizeof(cmdBuf));
			ImGui::Text("%s", L("TASK_COMMAND_LABEL"));
			ImGui::SetNextItemWidth(-1);
			if (ImGui::InputText("##command", cmdBuf, sizeof(cmdBuf))) {
				task.command = utf8_to_wstring(cmdBuf);
			}

			char workDirBuf[256];
			strncpy_s(workDirBuf, wstring_to_utf8(task.workingDirectory).c_str(), sizeof(workDirBuf));
			ImGui::Text("%s", L("TASK_WORKDIR_LABEL"));
			const bool workDirBrowseInline =
				ImGui::GetContentRegionAvail().x >= GetUiMetrics().Em(25.0f);
			SetStandardControlWidth();
			if (ImGui::InputText("##workdir", workDirBuf, sizeof(workDirBuf))) {
				task.workingDirectory = utf8_to_wstring(workDirBuf);
			}
			if (workDirBrowseInline) ImGui::SameLine();
			if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
				wstring sel = GetDesktopServices()->SelectFolder().path.wstring();
				if (!sel.empty()) task.workingDirectory = sel;
			}
		}

		ImGui::Spacing();

		int triggerMode = static_cast<int>(task.trigger.type);
		ImGui::Text("%s", L("TASK_TRIGGER_LABEL"));
		if (task.type == SpecialTaskType::Command) {
			triggerMode = 0;
			ImGui::TextDisabled("%s", L("SCHED_MODES_ONCE"));
		}
		else {
			ImGui::RadioButton(L("SCHED_MODES_ONCE"), &triggerMode, 0); ImGui::SameLine();
			ImGui::RadioButton(L("SCHED_MODES_INTERVAL"), &triggerMode, 1); ImGui::SameLine();
			ImGui::RadioButton(L("SCHED_MODES_SCHED"), &triggerMode, 2);
		}
		task.trigger.type = static_cast<SpecialTaskTriggerType>(triggerMode);

		if (task.trigger.type == SpecialTaskTriggerType::Interval) {
			SetStandardControlWidth(10.0f);
			ImGui::InputInt(L("INTERVAL_MINUTES"), &task.trigger.intervalMinutes);
			if (task.trigger.intervalMinutes < 1) task.trigger.intervalMinutes = 1;
		}
		else if (task.trigger.type == SpecialTaskTriggerType::Scheduled) {
			if (ImGui::BeginTable("TaskSchedule", 2, ImGuiTableFlags_SizingFixedFit)) {
				ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(L("SCHEDULE_AT"));
				ImGui::TableNextColumn();
				const float scheduleFieldWidth = GetUiMetrics().Em(5.0f);
				ImGui::SetNextItemWidth(scheduleFieldWidth); ImGui::InputInt("##sched_hour", &task.trigger.hour);
				ImGui::SameLine(); ImGui::TextUnformatted(":"); ImGui::SameLine();
				ImGui::SetNextItemWidth(scheduleFieldWidth); ImGui::InputInt("##sched_minute", &task.trigger.minute);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(L("SCHEDULE_ON"));
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(scheduleFieldWidth); ImGui::InputInt("##sched_month", &task.trigger.month);
				ImGui::SameLine(); ImGui::TextUnformatted("/"); ImGui::SameLine();
				ImGui::SetNextItemWidth(scheduleFieldWidth); ImGui::InputInt("##sched_day", &task.trigger.day);
				ImGui::SameLine(); ImGui::TextDisabled("%s", L("SCHED_EVERY_HINT"));
				ImGui::EndTable();
			}

			task.trigger.hour = max(0, min(23, task.trigger.hour));
			task.trigger.minute = max(0, min(59, task.trigger.minute));
			task.trigger.month = max(0, min(12, task.trigger.month));
			task.trigger.day = max(0, min(31, task.trigger.day));
		}
	}
	else if (wideLayout) {
		ImGui::TextDisabled("%s", L("TASK_SELECT_TO_EDIT"));
	}
	if (wideLayout) ImGui::EndChild();
}

void DrawServiceSettings(SpecialConfig& spCfg) {
	ImGui::SeparatorText(L("SERVICE_MODE_TITLE"));
	ImGui::TextWrapped("%s", L("SERVICE_MODE_DESC"));
	ImGui::Spacing();

#ifdef _WIN32
	ImGui::Text("%s: %s", L("SERVICE_CONFIGURED_NAME"),
		wstring_to_utf8(spCfg.serviceConfig.serviceName).c_str());
	static wstring cachedServiceName;
	static LegacyServiceCleanup::Inspection cachedInspection;
	static auto inspectedAt = chrono::steady_clock::time_point{};
	const auto now = chrono::steady_clock::now();
	if (cachedServiceName != spCfg.serviceConfig.serviceName
		|| now - inspectedAt >= chrono::seconds(2)) {
		cachedServiceName = spCfg.serviceConfig.serviceName;
		cachedInspection = LegacyServiceCleanup::Inspect(cachedServiceName);
		inspectedAt = now;
	}
	const auto& inspection = cachedInspection;
	if (!inspection.imagePath.empty()) {
		ImGui::TextWrapped("%s: %s", L("SERVICE_IMAGE_PATH"),
			wstring_to_utf8(inspection.imagePath).c_str());
	}
	if (inspection.state == LegacyServiceCleanup::State::NotInstalled) {
		ImGui::TextDisabled("%s", L("SERVICE_STATUS_NOT_INSTALLED"));
	}
	else if (inspection.CanRemove()) {
		ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%s%s",
			L("SERVICE_SAFE_TO_REMOVE"), inspection.running ? L("SERVICE_STATUS_RUNNING_SUFFIX") : "");
		if (ImGui::Button(L("SERVICE_REMOVE_VALIDATED"))) {
			const string prompt = string(L("SERVICE_REMOVE_CONFIRM")) + "\n\n"
				+ wstring_to_utf8(inspection.imagePath);
			if (ConfirmMessageBox("MineBackup", prompt)) {
				wstring error;
				if (!LegacyServiceCleanup::RequestElevatedRemoval(
						spCfg.serviceConfig.serviceName, error)) {
					MessageBoxWin("MineBackup", wstring_to_utf8(error), 2);
				}
				else {
					MessageBoxWin("MineBackup", L("SERVICE_REMOVAL_LAUNCHED"), 0);
					inspectedAt = chrono::steady_clock::time_point{};
				}
			}
		}
	}
	else {
		ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f), "%s",
			L("SERVICE_UNSAFE_MANUAL"));
		ImGui::TextWrapped("%s", wstring_to_utf8(inspection.diagnostic).c_str());
	}
#else
	ImGui::TextDisabled("%s", L("SERVICE_NOT_SUPPORTED"));
#endif
}

void DrawSpecialConfigSettings(SpecialConfig& spCfg, SpecialSettingsPage page) {
	char buf[128];
	strncpy_s(buf, spCfg.name.c_str(), sizeof(buf));
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::InputText(L("CONFIG_NAME"), buf, sizeof(buf))) spCfg.name = buf;

	ImGui::Spacing();

	if (page == SpecialSettingsPage::Tasks) {
		DrawUnifiedTaskManager(spCfg);
		return;
	}
	if (page == SpecialSettingsPage::LegacyCleanup) {
		DrawServiceSettings(spCfg);
		return;
	}
	if (page == SpecialSettingsPage::Backup) {
		ImGui::Checkbox(L("BACKUP_ON_START"), &spCfg.backupOnGameStart);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_BACKUP_ON_START"));
		ImGui::Checkbox(L("USE_LOW_PRIORITY"), &spCfg.useLowPriority);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_LOW_PRIORITY"));

		const int maxThreads = (std::max)(1u, thread::hardware_concurrency());
		const float fieldWidth = (std::min)(GetUiMetrics().Em(18.0f),
			ImGui::GetContentRegionAvail().x);
		ImGui::SetNextItemWidth(fieldWidth);
		ImGui::SliderInt(L("CPU_THREAD_COUNT"), &spCfg.cpuThreads, 0, maxThreads);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_CPU_THREADS"));

		int minLevel = 1;
		int maxLevel = 9;
		GetSpecialConfigCompressionLevelRange(spCfg, minLevel, maxLevel);
		spCfg.zipLevel = (std::clamp)(spCfg.zipLevel, minLevel, maxLevel);
		ImGui::SetNextItemWidth(fieldWidth);
		ImGui::SliderInt(L("COMPRESSION_LEVEL"), &spCfg.zipLevel, minLevel, maxLevel);
		ImGui::SetNextItemWidth(fieldWidth);
		ImGui::InputInt(L("BACKUPS_TO_KEEP"), &spCfg.keepCount);
		return;
	}

	if (ImGui::Checkbox(L("EXECUTE_ON_STARTUP"), &spCfg.autoExecute)) {
		SetExclusiveSpecialAutoExecute(g_appState.specialConfigs,
			g_appState.currentConfigIndex, spCfg.autoExecute);
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_EXECUTE_ON_STARTUP"));
	ImGui::Checkbox(L("EXIT_WHEN_FINISHED"), &spCfg.exitAfterExecution);

	const auto services = GetDesktopServices();
	const auto autostartCapability = services->Capabilities().autostart;
	map<int, bool> previousStartupSelections;
	for (const auto& [index, config] : g_appState.specialConfigs) {
		previousStartupSelections[index] = config.runOnStartup;
	}
	ImGui::BeginDisabled(!autostartCapability.IsAvailable());
	if (ImGui::Checkbox(L("RUN_ON_WINDOWS_STARTUP"), &spCfg.runOnStartup)) {
		SetExclusiveSpecialRunOnStartup(g_appState.specialConfigs,
			g_appState.currentConfigIndex, spCfg.runOnStartup);
		const bool enabled = g_RunOnStartup
			|| FindSpecialRunOnStartup(g_appState.specialConfigs).has_value();
		const auto status = services->SetAutostart(enabled);
		if (!status.IsAvailable()) {
			for (const auto& [index, selected] : previousStartupSelections) {
				g_appState.specialConfigs[index].runOnStartup = selected;
			}
			MessageBoxWin("MineBackup", L("AUTOSTART_OPERATION_FAILED"), 2);
		}
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)
		&& !autostartCapability.IsAvailable()) {
		ImGui::SetTooltip("%s", wstring_to_utf8(autostartCapability.diagnostic).c_str());
	}

	ImGui::Checkbox(L("HIDE_CONSOLE_WINDOW"), &spCfg.hideWindow);
	ImGui::Spacing();
	if (ImGui::Button(L("SPECIAL_COPY_CLI_COMMAND"))) {
		const string command = "minebackup-cli run-special "
			+ wstring_to_utf8(spCfg.specialConfigId);
		ImGui::SetClipboardText(command.c_str());
	}
	ImGui::SameLine();
	if (ImGui::Button(L("BUTTON_SWITCH_TO_SP_MODE"))) {
		SetExclusiveSpecialAutoExecute(g_appState.specialConfigs,
			g_appState.currentConfigIndex, true);
		SaveConfigs();
		if (services->RestartApplication().IsAvailable()) g_appState.done = true;
	}
}
