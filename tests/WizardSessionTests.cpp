#include "WizardSession.h"

#include "PathIdentity.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
	if (condition) return;
	++failures;
	std::cerr << "FAIL: " << message << '\n';
}

DiscoveredMinecraftInstance Candidate(
	const std::filesystem::path& savesRoot,
	std::wstring name,
	bool configured = false) {
	std::filesystem::create_directories(savesRoot / L"World");
	std::ofstream(savesRoot / L"World" / L"level.dat", std::ios::binary) << "level";
	InspectedMinecraftInstance instance;
	instance.edition = MinecraftEdition::Java;
	instance.savesRoot = savesRoot;
	instance.suggestedName = std::move(name);
	instance.worlds.push_back({
		savesRoot / L"World", L"World", L"World", L"My World"});
	return {std::move(instance), configured};
}

} // namespace

int main() {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path())
		/ ("MineBackupWizardSession-" + std::to_string(stamp));
	std::filesystem::create_directories(root);

	WizardSession session;
	SetWizardDefaultBackupRoot(session, root / "backups");
	const auto firstGeneration = BeginWizardDiscovery(session);
	const auto secondGeneration = BeginWizardDiscovery(session);
	MinecraftDiscoveryResult stale;
	stale.instances.push_back(Candidate(root / "stale" / "saves", L"Stale"));
	Expect(!ApplyWizardDiscoveryResult(session, firstGeneration, std::move(stale))
			&& session.discovery.instances.empty(),
		"an obsolete scan generation must not replace the current discovery result");

	MinecraftDiscoveryResult current;
	current.instances.push_back(Candidate(root / "java" / "saves", L"Create"));
	current.instances.push_back(Candidate(root / "existing" / "saves", L"Existing", true));
	Expect(ApplyWizardDiscoveryResult(session, secondGeneration, std::move(current)),
		"the current scan generation should be accepted");
	const auto selectableKey = BuildWizardInstanceKey(
		session.discovery.instances[0].instance);
	const auto configuredKey = BuildWizardInstanceKey(
		session.discovery.instances[1].instance);
	Expect(!SetWizardInstanceSelected(session, configuredKey, true)
			&& SetWizardInstanceSelected(session, selectableKey, true),
		"already-configured instances must be rejected while new instances remain selectable");

	Config existing;
	existing.name = "Create";
	existing.backupPath = (root / "occupied").wstring();
	const auto& drafts = RebuildWizardDrafts(session, {{1, existing}});
	Expect(drafts.size() == 1 && drafts[0].name == "Create (2)",
		"draft generation should reuse batch naming against existing configurations");
	Expect(PathIdentity::PathsEqual(
		drafts[0].backupPath, root / "backups" / "Create (2)"),
		"draft generation should preview the final default backup location");
	Expect(!std::filesystem::exists(root / "backups"),
		"draft planning must not create directories or persist first-run state");
	Expect(drafts[0].worlds.size() == 1
			&& drafts[0].worlds[0].first == L"World"
			&& drafts[0].worlds[0].second == L"My World",
		"Inspector-validated worlds should flow into the draft without rescanning the filesystem");

	session.readiness.report.ready = true;
	SetWizardDefaultBackupRoot(session, root / "changed");
	Expect(session.drafts.empty() && !session.readiness.report.ready,
		"changing the backup root should invalidate draft previews and readiness");

	std::error_code error;
	std::filesystem::remove_all(root, error);
	if (failures != 0) return 1;
	std::cout << "All wizard session tests passed\n";
	return 0;
}
