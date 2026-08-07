#include "SpecialTaskDocumentTests.h"

#include "SpecialTaskDocument.h"

#include <map>
#include <set>

using namespace std;

namespace {

constexpr const wchar_t* kConfigId = L"11111111-1111-4111-8111-111111111111";
constexpr const wchar_t* kSpecialId = L"22222222-2222-4222-8222-222222222222";
constexpr const wchar_t* kTaskOne = L"33333333-3333-4333-8333-333333333333";
constexpr const wchar_t* kTaskTwo = L"44444444-4444-4444-8444-444444444444";

SpecialTask MakeBackupTask() {
	SpecialTask task;
	task.taskId = kTaskOne;
	task.name = "备份, \"主世界\"\n第二行";
	task.type = SpecialTaskType::Backup;
	task.executionMode = SpecialTaskExecutionMode::Parallel;
	task.trigger.type = SpecialTaskTriggerType::Scheduled;
	task.trigger.month = 0;
	task.trigger.day = 0;
	task.trigger.hour = 23;
	task.trigger.minute = 59;
	task.target.configId = kConfigId;
	task.target.worldPath = L"world/subdirectory";
	return task;
}

} // namespace

void RunSpecialTaskDocumentTests(TestContext& test, const filesystem::path& temporaryRoot) {
	SpecialTaskDocument document;
	SpecialTaskConfigDocument special;
	special.specialConfigId = kSpecialId;
	special.tasks.push_back(MakeBackupTask());
	SpecialTask command;
	command.taskId = kTaskTwo;
	command.name = "command";
	command.type = SpecialTaskType::Command;
	command.command = L"powershell -Command \"Write-Host 'A,B'\"\nnext";
	command.workingDirectory = L"工作,目录";
	special.tasks.push_back(command);
	document.specialConfigs.push_back(special);

	const auto parsed = SpecialTaskStorage::Parse(SpecialTaskStorage::Serialize(document));
	test.Expect(parsed.IsLoaded(), "special task JSON should round-trip");
	test.Expect(parsed.document.specialConfigs.size() == 1
			&& parsed.document.specialConfigs[0].tasks.size() == 2,
		"special task JSON should preserve task count");
	if (parsed.IsLoaded() && parsed.document.specialConfigs[0].tasks.size() == 2) {
		const auto& tasks = parsed.document.specialConfigs[0].tasks;
		test.Expect(tasks[0].taskId == kTaskOne && tasks[1].taskId == kTaskTwo,
			"special task JSON array order should be execution order");
		test.Expect(tasks[0].name == special.tasks[0].name
				&& tasks[1].command == command.command
				&& tasks[1].workingDirectory == command.workingDirectory,
			"special task strings should preserve commas, quotes, Unicode and newlines");
	}

	wstring normalized;
	test.Expect(SpecialTaskStorage::TryNormalizeWorldPath(
			L"world\\subdirectory", normalized)
			&& normalized == L"world/subdirectory",
		"world paths should normalize separators");
	test.Expect(!SpecialTaskStorage::TryNormalizeWorldPath(L"../world", normalized)
			&& !SpecialTaskStorage::TryNormalizeWorldPath(L"/world", normalized),
		"world paths should reject traversal and absolute paths");

	const auto future = SpecialTaskStorage::Parse(
		R"({"schemaVersion":2,"specialConfigs":[]})");
	test.Expect(future.status == SpecialTaskStorage::LoadStatus::UnsupportedSchema,
		"future special task schema should be rejected without fallback");

	map<int, Config> configs;
	configs[7].configId = kConfigId;
	configs[7].worlds = {{L"world/subdirectory", L""}};
	map<int, SpecialConfig> legacy;
	legacy[9].specialConfigId = kSpecialId;
	legacy[9].commands = {L"echo first,quoted"};
	UnifiedTaskV2 later;
	later.id = 20;
	later.name = "later command";
	later.type = TaskTypeV2::Command;
	later.triggerMode = TaskTrigger::Interval;
	later.command = L"echo later";
	UnifiedTaskV2 earlier;
	earlier.id = 10;
	earlier.name = "earlier backup";
	earlier.type = TaskTypeV2::Backup;
	earlier.configIndex = 7;
	earlier.worldIndex = 0;
	legacy[9].unifiedTasks = {later, earlier};
	AutomatedTask ignored;
	ignored.configIndex = 999;
	legacy[9].tasks.push_back(ignored);

	const auto migration = SpecialTaskStorage::MigrateLegacy(configs, legacy);
	test.Expect(migration.success, "valid legacy special tasks should migrate");
	if (migration.success && !migration.document.specialConfigs.empty()) {
		const auto& tasks = migration.document.specialConfigs[0].tasks;
		test.Expect(tasks.size() == 3
				&& tasks[0].command == L"echo first,quoted"
				&& tasks[1].name == "earlier backup"
				&& tasks[2].name == "later command",
			"migration should keep commands first and stable-sort UnifiedTask IDs");
		test.Expect(tasks[2].trigger.type == SpecialTaskTriggerType::Once,
			"legacy command triggers should normalize to once");
		set<wstring> taskIds;
		for (const auto& task : tasks) taskIds.insert(task.taskId);
		test.Expect(taskIds.size() == tasks.size(),
			"migration should assign unique stable task IDs");
	}
	test.Expect(!migration.diagnostics.empty()
			&& migration.diagnostics[0].severity == SpecialTaskStorage::DiagnosticSeverity::Warning,
		"normalized legacy command triggers should emit a warning");

	auto duplicateLegacy = legacy;
	duplicateLegacy[9].unifiedTasks.push_back(earlier);
	test.Expect(!SpecialTaskStorage::MigrateLegacy(configs, duplicateLegacy).success,
		"duplicate legacy numeric task IDs should abort migration");
	auto invalidLegacy = legacy;
	invalidLegacy[9].unifiedTasks[1].worldIndex = 99;
	test.Expect(!SpecialTaskStorage::MigrateLegacy(configs, invalidLegacy).success,
		"out-of-range legacy task targets should abort migration");

	const filesystem::path path = temporaryRoot / "special-tasks.json";
	wstring error;
	test.Expect(SpecialTaskStorage::Save(path, document, error),
		"valid special task document should save atomically");
	const string original = ReadText(path);
	auto invalidDocument = document;
	invalidDocument.specialConfigs[0].tasks[0].taskId = L"not-a-uuid";
	test.Expect(!SpecialTaskStorage::Save(path, invalidDocument, error)
			&& ReadText(path) == original,
		"failed special task writes should not replace the authoritative file");

	map<int, SpecialConfig> targets;
	targets[9].specialConfigId = kSpecialId;
	vector<SpecialTaskStorage::Diagnostic> diagnostics;
	test.Expect(SpecialTaskStorage::ApplyAndValidate(
			document, configs, targets, diagnostics)
			&& targets[9].specialTasks.size() == 2,
		"special task document should bind through stable profile identities");
}
