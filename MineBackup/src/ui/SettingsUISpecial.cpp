#include "SettingsUIPrivate.h"

using namespace std;

void DrawUnifiedTaskManager(SpecialConfig& spCfg) {
	ImGui::SeparatorText(L("TASK_MANAGER_TITLE"));

	static int selectedTaskIndex = -1;

	if (ImGui::Button(L("TASK_ADD_BACKUP"))) {
		UnifiedTaskV2 newTask;
		newTask.id = spCfg.unifiedTasks.empty() ? 1 : (spCfg.unifiedTasks.back().id + 1);
		newTask.name = wstring_to_utf8(MineFormatMessage("TASK_DEFAULT_BACKUP_NAME", newTask.id));
		newTask.type = TaskTypeV2::Backup;
		spCfg.unifiedTasks.push_back(newTask);
	}
	ImGui::SameLine();
	if (ImGui::Button(L("TASK_ADD_COMMAND"))) {
		UnifiedTaskV2 newTask;
		newTask.id = spCfg.unifiedTasks.empty() ? 1 : (spCfg.unifiedTasks.back().id + 1);
		newTask.name = wstring_to_utf8(MineFormatMessage("TASK_DEFAULT_COMMAND_NAME", newTask.id));
		newTask.type = TaskTypeV2::Command;
		spCfg.unifiedTasks.push_back(newTask);
	}
	ImGui::SameLine();
	if (ImGui::Button(L("TASK_REMOVE")) && selectedTaskIndex >= 0 && selectedTaskIndex < static_cast<int>(spCfg.unifiedTasks.size())) {
		spCfg.unifiedTasks.erase(spCfg.unifiedTasks.begin() + selectedTaskIndex);
		selectedTaskIndex = -1;
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(selectedTaskIndex <= 0);
	if (ImGui::Button(L("TASK_MOVE_UP"))) {
		swap(spCfg.unifiedTasks[selectedTaskIndex], spCfg.unifiedTasks[selectedTaskIndex - 1]);
		selectedTaskIndex--;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(selectedTaskIndex < 0 || selectedTaskIndex >= static_cast<int>(spCfg.unifiedTasks.size()) - 1);
	if (ImGui::Button(L("TASK_MOVE_DOWN"))) {
		swap(spCfg.unifiedTasks[selectedTaskIndex], spCfg.unifiedTasks[selectedTaskIndex + 1]);
		selectedTaskIndex++;
	}
	ImGui::EndDisabled();

	ImGui::Spacing();

	if (ImGui::BeginTable("TasksTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30);
		ImGui::TableSetupColumn(L("TASK_NAME"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(L("TASK_TYPE"), ImGuiTableColumnFlags_WidthFixed, 80);
		ImGui::TableSetupColumn(L("TASK_EXEC_MODE"), ImGuiTableColumnFlags_WidthFixed, 100);
		ImGui::TableSetupColumn(L("TASK_TRIGGER"), ImGuiTableColumnFlags_WidthFixed, 80);
		ImGui::TableSetupColumn(L("TASK_ENABLED"), ImGuiTableColumnFlags_WidthFixed, 60);
		ImGui::TableHeadersRow();

		for (int i = 0; i < static_cast<int>(spCfg.unifiedTasks.size()); ++i) {
			auto& task = spCfg.unifiedTasks[i];
			ImGui::TableNextRow();
			ImGui::PushID(i);

			ImGui::TableSetColumnIndex(0);
			if (ImGui::Selectable(to_string(i + 1).c_str(), selectedTaskIndex == i, ImGuiSelectableFlags_SpanAllColumns)) {
				selectedTaskIndex = i;
			}

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%s", task.name.c_str());

			ImGui::TableSetColumnIndex(2);
			const char* typeNames[] = { L("TASK_TYPE_BACKUP"), L("TASK_TYPE_COMMAND"), L("TASK_TYPE_SCRIPT") };
			ImGui::Text("%s", typeNames[static_cast<int>(task.type)]);

			ImGui::TableSetColumnIndex(3);
			const char* execModeNames[] = { L("TASK_EXEC_SEQUENTIAL"), L("TASK_EXEC_PARALLEL") };
			ImGui::Text("%s", execModeNames[static_cast<int>(task.executionMode)]);

			ImGui::TableSetColumnIndex(4);
			const char* triggerNames[] = { L("SCHED_MODES_ONCE"), L("SCHED_MODES_INTERVAL"), L("SCHED_MODES_SCHED") };
			ImGui::Text("%s", triggerNames[static_cast<int>(task.triggerMode)]);

			ImGui::TableSetColumnIndex(5);
			ImGui::Checkbox("##enabled", &task.enabled);

			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	if (selectedTaskIndex >= 0 && selectedTaskIndex < static_cast<int>(spCfg.unifiedTasks.size())) {
		auto& task = spCfg.unifiedTasks[selectedTaskIndex];

		ImGui::Spacing();
		ImGui::SeparatorText(L("TASK_DETAILS"));

		char nameBuf[128];
		strncpy_s(nameBuf, task.name.c_str(), sizeof(nameBuf));
		ImGui::SetNextItemWidth(300);
		if (ImGui::InputText(L("TASK_NAME"), nameBuf, sizeof(nameBuf))) {
			task.name = nameBuf;
		}

		int execMode = static_cast<int>(task.executionMode);
		ImGui::Text("%s", L("TASK_EXEC_MODE_LABEL"));
		ImGui::RadioButton(L("TASK_EXEC_SEQUENTIAL"), &execMode, 0);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_TASK_EXEC_SEQUENTIAL"));
		ImGui::SameLine();
		ImGui::RadioButton(L("TASK_EXEC_PARALLEL"), &execMode, 1);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_TASK_EXEC_PARALLEL"));
		task.executionMode = static_cast<TaskExecMode>(execMode);

		ImGui::Spacing();

		if (task.type == TaskTypeV2::Backup) {
			string current_config_label = g_appState.configs.count(task.configIndex)
				? (string(L("CONFIG_N")) + to_string(task.configIndex))
				: L("TASK_NONE");
			ImGui::SetNextItemWidth(200);
			if (ImGui::BeginCombo(L("CONFIG_COMBO"), current_config_label.c_str())) {
				for (auto const& [idx, val] : g_appState.configs) {
					if (ImGui::Selectable((string(L("CONFIG_N")) + to_string(idx) + " - " + val.name).c_str(), task.configIndex == idx)) {
						task.configIndex = idx;
						task.worldIndex = val.worlds.empty() ? -1 : 0;
					}
				}
				ImGui::EndCombo();
			}

			if (g_appState.configs.count(task.configIndex)) {
				Config& selected_cfg = g_appState.configs[task.configIndex];
				string current_world_label = L("TASK_NONE");
				if (!selected_cfg.worlds.empty() && task.worldIndex >= 0 && task.worldIndex < static_cast<int>(selected_cfg.worlds.size())) {
					current_world_label = wstring_to_utf8(selected_cfg.worlds[task.worldIndex].first);
				}
				ImGui::SetNextItemWidth(200);
				if (ImGui::BeginCombo(L("WORLD_COMBO"), current_world_label.c_str())) {
					for (int w_idx = 0; w_idx < static_cast<int>(selected_cfg.worlds.size()); ++w_idx) {
						if (ImGui::Selectable(wstring_to_utf8(selected_cfg.worlds[w_idx].first).c_str(), task.worldIndex == w_idx)) {
							task.worldIndex = w_idx;
						}
					}
					ImGui::EndCombo();
				}
			}
		}
		else if (task.type == TaskTypeV2::Command) {
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
			if (ImGui::Button(L("BUTTON_SELECT_FOLDER"))) {
				wstring sel = GetDesktopServices()->SelectFolder().path.wstring();
				if (!sel.empty()) task.workingDirectory = sel;
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			if (ImGui::InputText("##workdir", workDirBuf, sizeof(workDirBuf))) {
				task.workingDirectory = utf8_to_wstring(workDirBuf);
			}
		}

		ImGui::Spacing();

		int triggerMode = static_cast<int>(task.triggerMode);
		ImGui::Text("%s", L("TASK_TRIGGER_LABEL"));
		ImGui::RadioButton(L("SCHED_MODES_ONCE"), &triggerMode, 0); ImGui::SameLine();
		ImGui::RadioButton(L("SCHED_MODES_INTERVAL"), &triggerMode, 1); ImGui::SameLine();
		ImGui::RadioButton(L("SCHED_MODES_SCHED"), &triggerMode, 2);
		task.triggerMode = static_cast<TaskTrigger>(triggerMode);

		if (task.triggerMode == TaskTrigger::Interval) {
			ImGui::SetNextItemWidth(150);
			ImGui::InputInt(L("INTERVAL_MINUTES"), &task.intervalMinutes);
			if (task.intervalMinutes < 1) task.intervalMinutes = 1;
		}
		else if (task.triggerMode == TaskTrigger::Scheduled) {
			if (ImGui::BeginTable("TaskSchedule", 2, ImGuiTableFlags_SizingFixedFit)) {
				ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(L("SCHEDULE_AT"));
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(80); ImGui::InputInt("##sched_hour", &task.schedHour);
				ImGui::SameLine(); ImGui::TextUnformatted(":"); ImGui::SameLine();
				ImGui::SetNextItemWidth(80); ImGui::InputInt("##sched_minute", &task.schedMinute);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(L("SCHEDULE_ON"));
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(80); ImGui::InputInt("##sched_month", &task.schedMonth);
				ImGui::SameLine(); ImGui::TextUnformatted("/"); ImGui::SameLine();
				ImGui::SetNextItemWidth(80); ImGui::InputInt("##sched_day", &task.schedDay);
				ImGui::SameLine(); ImGui::TextDisabled("%s", L("SCHED_EVERY_HINT"));
				ImGui::EndTable();
			}

			task.schedHour = max(0, min(23, task.schedHour));
			task.schedMinute = max(0, min(59, task.schedMinute));
			task.schedMonth = max(0, min(12, task.schedMonth));
			task.schedDay = max(0, min(31, task.schedDay));
		}
	}
}

void DrawServiceSettings(SpecialConfig& spCfg) {
	ImGui::SeparatorText(L("SERVICE_MODE_TITLE"));
	ImGui::TextWrapped("%s", L("SERVICE_MODE_DESC"));
	ImGui::Spacing();

#ifdef _WIN32
	ImGui::Text("%s: %s", L("SERVICE_CONFIGURED_NAME"),
		wstring_to_utf8(spCfg.serviceConfig.serviceName).c_str());
	static wstring cachedServiceName;
	static TaskSystem::LegacyServiceInspection cachedInspection;
	static auto inspectedAt = chrono::steady_clock::time_point{};
	const auto now = chrono::steady_clock::now();
	if (cachedServiceName != spCfg.serviceConfig.serviceName
		|| now - inspectedAt >= chrono::seconds(2)) {
		cachedServiceName = spCfg.serviceConfig.serviceName;
		cachedInspection = TaskSystem::InspectLegacyService(cachedServiceName);
		inspectedAt = now;
	}
	const auto& inspection = cachedInspection;
	if (!inspection.imagePath.empty()) {
		ImGui::TextWrapped("%s: %s", L("SERVICE_IMAGE_PATH"),
			wstring_to_utf8(inspection.imagePath).c_str());
	}
	if (inspection.state == TaskSystem::LegacyServiceState::NotInstalled) {
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
				if (!TaskSystem::RequestElevatedLegacyServiceRemoval(
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

void DrawSpecialConfigSettings(SpecialConfig& spCfg) {
	char buf[128];
	strncpy_s(buf, spCfg.name.c_str(), sizeof(buf));
	if (ImGui::InputText(L("CONFIG_NAME"), buf, sizeof(buf))) spCfg.name = buf;

	ImGui::Spacing();

	if (ImGui::BeginTabBar("SpecialConfigTabs", ImGuiTabBarFlags_None)) {
		if (ImGui::BeginTabItem(L("TAB_STARTUP"))) {
			ImGui::Spacing();
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
			if (ImGui::Button(L("BUTTON_SWITCH_TO_SP_MODE"))) {
				SetExclusiveSpecialAutoExecute(g_appState.specialConfigs,
					g_appState.currentConfigIndex, true);
				SaveConfigs();
				(void)services->RestartApplication();
				g_appState.done = true;
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(L("TAB_TASKS"))) {
			ImGui::Spacing();
			DrawUnifiedTaskManager(spCfg);
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(L("TAB_SERVICE"))) {
			ImGui::Spacing();
			DrawServiceSettings(spCfg);
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(L("TAB_BACKUP_OVERRIDES"))) {
			ImGui::Spacing();
			ImGui::Checkbox(L("BACKUP_ON_START"), &spCfg.backupOnGameStart);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_BACKUP_ON_START"));
			ImGui::Checkbox(L("USE_LOW_PRIORITY"), &spCfg.useLowPriority);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_LOW_PRIORITY"));

			int max_threads = thread::hardware_concurrency();
			ImGui::SetNextItemWidth(200);
			ImGui::SliderInt(L("CPU_THREAD_COUNT"), &spCfg.cpuThreads, 0, max_threads);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L("TIP_CPU_THREADS"));

			ImGui::SetNextItemWidth(200);
			int spMinLevel = 1;
			int spMaxLevel = 9;
			GetSpecialConfigCompressionLevelRange(spCfg, spMinLevel, spMaxLevel);
			if (spCfg.zipLevel < spMinLevel) spCfg.zipLevel = spMinLevel;
			if (spCfg.zipLevel > spMaxLevel) spCfg.zipLevel = spMaxLevel;
			ImGui::SliderInt(L("COMPRESSION_LEVEL"), &spCfg.zipLevel, spMinLevel, spMaxLevel);

			ImGui::SetNextItemWidth(150);
			ImGui::InputInt(L("BACKUPS_TO_KEEP"), &spCfg.keepCount);
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(L("TAB_APPEARANCE"))) {
			ImGui::Spacing();
			const char* theme_names[] = { L("THEME_DARK"), L("THEME_LIGHT"), L("THEME_CLASSIC"), L("THEME_WIN_LIGHT"), L("THEME_WIN_DARK"), L("THEME_NORD_LIGHT"), L("THEME_NORD_DARK"), L("THEME_CUSTOM") };
			ImGui::SetNextItemWidth(200);
			if (ImGui::Combo(L("THEME_SETTINGS"), &spCfg.theme, theme_names, IM_ARRAYSIZE(theme_names))) {
				ApplyTheme(spCfg.theme);
			}
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}
