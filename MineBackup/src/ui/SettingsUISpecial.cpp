#include "SettingsUIPrivate.h"

#include <sstream>

using namespace std;

namespace {

string LocalizedLabel(const char* key, const char* id) {
	return string(L(key)) + "##" + id;
}

bool EditString(const char* label, string& value, size_t capacity = 256) {
	vector<char> buffer((max)(capacity, value.size() + 2), '\0');
	copy_n(value.data(), (min)(value.size(), buffer.size() - 1), buffer.data());
	if (!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
	value = buffer.data();
	return true;
}

bool EditWideString(const char* label, wstring& value, size_t capacity = 512) {
	string utf8 = wstring_to_utf8(value);
	if (!EditString(label, utf8, capacity)) return false;
	value = utf8_to_wstring(utf8);
	return true;
}

wstring JoinArguments(const vector<wstring>& arguments) {
	wostringstream output;
	for (size_t index = 0; index < arguments.size(); ++index) {
		if (index != 0) output << L'\n';
		output << arguments[index];
	}
	return output.str();
}

vector<wstring> SplitArguments(const wstring& text) {
	vector<wstring> result;
	wistringstream input(text);
	wstring line;
	while (getline(input, line)) result.push_back(line);
	return result;
}

JobStep MakeBackupStep() {
	JobStep step;
	step.stepId = FolderRewindFormat::GenerateGuidString();
	step.name = L("JOB_DEFAULT_BACKUP_STEP_NAME");
	step.type = JobStepType::Backup;
	if (!g_appState.configs.empty()) {
		const Config& config = g_appState.configs.begin()->second;
		step.backup.configId = config.configId;
		if (!config.worlds.empty()) step.backup.worldPath = config.worlds.front().first;
	}
	return step;
}

JobStep MakeProcessStep() {
	JobStep step;
	step.stepId = FolderRewindFormat::GenerateGuidString();
	step.name = L("JOB_DEFAULT_PROCESS_STEP_NAME");
	step.type = JobStepType::Process;
	step.process.maximumCapturedBytes = 4u * 1024u * 1024u;
	return step;
}

JobStage MakeStage() {
	JobStage stage;
	stage.stageId = FolderRewindFormat::GenerateGuidString();
	stage.name = L("JOB_DEFAULT_STAGE_NAME");
	stage.steps.push_back(MakeBackupStep());
	return stage;
}

Job MakeJob() {
	Job job;
	job.jobId = FolderRewindFormat::GenerateGuidString();
	job.name = L("JOB_DEFAULT_NAME");
	job.stages.push_back(MakeStage());
	return job;
}

void DrawBackupStep(JobStep& step) {
	ImGui::TextDisabled("%s: %s", L("JOB_BACKUP_CONFIG"),
		wstring_to_utf8(step.backup.configId).c_str());
	const string configLabel = LocalizedLabel("JOB_BACKUP_CONFIG", "JobBackupConfig");
	if (ImGui::BeginCombo(configLabel.c_str(),
		wstring_to_utf8(step.backup.configId).c_str())) {
		for (const auto& [index, config] : g_appState.configs) {
			(void)index;
			const bool selected = config.configId == step.backup.configId;
			if (ImGui::Selectable(config.name.c_str(), selected)) {
				step.backup.configId = config.configId;
				step.backup.worldPath = config.worlds.empty()
					? L"" : config.worlds.front().first;
			}
		}
		ImGui::EndCombo();
	}
	const Config* selectedConfig = nullptr;
	for (const auto& [index, config] : g_appState.configs) {
		(void)index;
		if (config.configId == step.backup.configId) selectedConfig = &config;
	}
	const string worldPreview = wstring_to_utf8(step.backup.worldPath);
	const string worldLabel = LocalizedLabel("JOB_BACKUP_WORLD", "JobBackupWorld");
	if (ImGui::BeginCombo(worldLabel.c_str(), worldPreview.c_str())) {
		if (selectedConfig) {
			for (const auto& [world, description] : selectedConfig->worlds) {
				const bool selected = world == step.backup.worldPath;
				const string label = description.empty()
					? wstring_to_utf8(world) : wstring_to_utf8(description + L" (" + world + L")");
				if (ImGui::Selectable(label.c_str(), selected)) step.backup.worldPath = world;
			}
		}
		ImGui::EndCombo();
	}
	const string commentLabel = LocalizedLabel("JOB_BACKUP_COMMENT", "JobBackupComment");
	EditWideString(commentLabel.c_str(), step.backup.comment);
}

void DrawProcessStep(JobStep& step) {
	wstring executable = step.process.executable.wstring();
	const string executableLabel =
		LocalizedLabel("JOB_PROCESS_EXECUTABLE", "JobProcessExecutable");
	if (EditWideString(executableLabel.c_str(), executable)) {
		step.process.executable = executable;
	}
	wstring arguments = JoinArguments(step.process.arguments);
	string utf8Arguments = wstring_to_utf8(arguments);
	vector<char> argumentBuffer((max)(size_t{4096}, utf8Arguments.size() + 2), '\0');
	copy_n(utf8Arguments.data(),
		(min)(utf8Arguments.size(), argumentBuffer.size() - 1), argumentBuffer.data());
	const string argumentsLabel =
		LocalizedLabel("JOB_PROCESS_ARGUMENTS", "JobProcessArguments");
	if (ImGui::InputTextMultiline(argumentsLabel.c_str(),
		argumentBuffer.data(), argumentBuffer.size(), ImVec2(-1.0f, GetUiMetrics().Em(5.0f)))) {
		step.process.arguments = SplitArguments(utf8_to_wstring(argumentBuffer.data()));
	}
	wstring workingDirectory = step.process.workingDirectory.wstring();
	const string workingDirectoryLabel =
		LocalizedLabel("JOB_PROCESS_WORKING_DIRECTORY", "JobProcessWorking");
	if (EditWideString(workingDirectoryLabel.c_str(), workingDirectory)) {
		step.process.workingDirectory = workingDirectory;
	}
	int timeoutSeconds = static_cast<int>(step.process.timeout.count() / 1000);
	const string timeoutLabel = LocalizedLabel("JOB_PROCESS_TIMEOUT", "JobProcessTimeout");
	if (ImGui::InputInt(timeoutLabel.c_str(), &timeoutSeconds)) {
		timeoutSeconds = (max)(0, timeoutSeconds);
		step.process.timeout = chrono::seconds(timeoutSeconds);
	}
	const string priorityLabel =
		LocalizedLabel("JOB_PROCESS_LOW_PRIORITY", "JobProcessPriority");
	ImGui::Checkbox(priorityLabel.c_str(), &step.process.useLowPriority);
}

void DrawStep(JobStage& stage, size_t stepIndex) {
	JobStep& step = stage.steps[stepIndex];
	ImGui::PushID(static_cast<int>(stepIndex));
	const string header = step.name + "##JobStep";
	if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextDisabled("%s: %s", L("JOB_STEP_ID"),
			wstring_to_utf8(step.stepId).c_str());
		const string nameLabel = LocalizedLabel("JOB_STEP_NAME", "JobStepName");
		EditString(nameLabel.c_str(), step.name);
		ImGui::TextUnformatted(step.type == JobStepType::Backup
			? L("JOB_STEP_TYPE_BACKUP") : L("JOB_STEP_TYPE_PROCESS"));
		if (step.type == JobStepType::Backup) DrawBackupStep(step);
		else DrawProcessStep(step);
		const string deleteLabel = LocalizedLabel("JOB_STEP_DELETE", "JobStepDelete");
		if (ImGui::Button(deleteLabel.c_str())) {
			stage.steps.erase(stage.steps.begin() + static_cast<ptrdiff_t>(stepIndex));
			ImGui::PopID();
			return;
		}
	}
	ImGui::PopID();
}

} // namespace

void DrawJobSettings() {
	auto& jobs = g_appState.jobs.jobs;
	static size_t selectedJob = 0;
	static size_t selectedStage = 0;
	if (selectedJob >= jobs.size()) selectedJob = jobs.empty() ? 0 : jobs.size() - 1;

	ImGui::SeparatorText(L("JOB_SETTINGS_TITLE"));
	ImGui::TextWrapped("%s", L("JOB_SETTINGS_DESCRIPTION"));
	const string addJobLabel = LocalizedLabel("JOB_ADD", "JobAdd");
	if (ImGui::Button(addJobLabel.c_str())) {
		jobs.push_back(MakeJob());
		selectedJob = jobs.size() - 1;
		selectedStage = 0;
	}
	if (jobs.empty()) {
		ImGui::TextDisabled("%s", L("JOB_EMPTY"));
		return;
	}

	ImGui::SameLine();
	if (ImGui::BeginCombo("##JobSelector", jobs[selectedJob].name.c_str())) {
		for (size_t index = 0; index < jobs.size(); ++index) {
			if (ImGui::Selectable(jobs[index].name.c_str(), index == selectedJob)) {
				selectedJob = index;
				selectedStage = 0;
			}
		}
		ImGui::EndCombo();
	}

	Job& job = jobs[selectedJob];
	ImGui::TextDisabled("%s: %s", L("JOB_ID"),
		wstring_to_utf8(job.jobId).c_str());
	const string jobNameLabel = LocalizedLabel("JOB_NAME", "JobName");
	EditString(jobNameLabel.c_str(), job.name);
	const string deleteJobLabel = LocalizedLabel("JOB_DELETE", "JobDelete");
	if (ImGui::Button(deleteJobLabel.c_str())) {
		jobs.erase(jobs.begin() + static_cast<ptrdiff_t>(selectedJob));
		selectedJob = 0;
		selectedStage = 0;
		return;
	}
	ImGui::SameLine();
	const string addStageLabel = LocalizedLabel("JOB_STAGE_ADD", "JobStageAdd");
	if (ImGui::Button(addStageLabel.c_str())) {
		job.stages.push_back(MakeStage());
		selectedStage = job.stages.size() - 1;
	}
	if (job.stages.empty()) {
		ImGui::TextDisabled("%s", L("JOB_STAGE_EMPTY"));
		return;
	}
	if (selectedStage >= job.stages.size()) selectedStage = job.stages.size() - 1;

	ImGui::SeparatorText(L("JOB_STAGE_TITLE"));
	if (ImGui::BeginCombo("##StageSelector", job.stages[selectedStage].name.c_str())) {
		for (size_t index = 0; index < job.stages.size(); ++index) {
			if (ImGui::Selectable(job.stages[index].name.c_str(), index == selectedStage)) {
				selectedStage = index;
			}
		}
		ImGui::EndCombo();
	}
	JobStage& stage = job.stages[selectedStage];
	ImGui::TextDisabled("%s: %s", L("JOB_STAGE_ID"),
		wstring_to_utf8(stage.stageId).c_str());
	const string stageNameLabel = LocalizedLabel("JOB_STAGE_NAME", "JobStageName");
	EditString(stageNameLabel.c_str(), stage.name);
	const string deleteStageLabel =
		LocalizedLabel("JOB_STAGE_DELETE", "JobStageDelete");
	if (ImGui::Button(deleteStageLabel.c_str())) {
		job.stages.erase(job.stages.begin() + static_cast<ptrdiff_t>(selectedStage));
		selectedStage = 0;
		return;
	}
	ImGui::SameLine();
	const string addBackupLabel =
		LocalizedLabel("JOB_STEP_ADD_BACKUP", "JobStepAddBackup");
	if (ImGui::Button(addBackupLabel.c_str())) stage.steps.push_back(MakeBackupStep());
	ImGui::SameLine();
	const string addProcessLabel =
		LocalizedLabel("JOB_STEP_ADD_PROCESS", "JobStepAddProcess");
	if (ImGui::Button(addProcessLabel.c_str())) stage.steps.push_back(MakeProcessStep());

	ImGui::SeparatorText(L("JOB_STEP_TITLE"));
	for (size_t index = 0; index < stage.steps.size();) {
		const size_t before = stage.steps.size();
		DrawStep(stage, index);
		if (stage.steps.size() == before) ++index;
	}
}
