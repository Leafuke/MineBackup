#include "MinecraftInstanceDiscoveryService.h"

#include "PathIdentity.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
	if (condition) return;
	++failures;
	std::cerr << "FAIL: " << message << '\n';
}

void Touch(const std::filesystem::path& path) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << "level";
}

class FakeProvider final : public IMinecraftDiscoveryProvider {
public:
	std::string id;
	std::vector<DiscoveryLocation> locations;
	bool fail = false;

	std::string Id() const override { return id; }
	std::vector<DiscoveryLocation> DiscoverLocations(
		const MinecraftDiscoveryContext&,
		std::stop_token stopToken) override {
		if (fail) throw std::runtime_error("injected provider failure");
		return stopToken.stop_requested() ? std::vector<DiscoveryLocation>{} : locations;
	}
};

DiscoveryLocation Location(
	const std::filesystem::path& root,
	DiscoveryEvidenceKind kind,
	std::string provider) {
	return {root, DiscoveryLocationKind::MinecraftRoot, {{kind, std::move(provider), root}}};
}

} // namespace

int main() {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path())
		/ ("MineBackupDiscoveryService-" + std::to_string(stamp));
	const auto sharedRoot = root / "shared" / ".minecraft";
	const auto knownOnlyRoot = root / "known" / ".minecraft";
	Touch(sharedRoot / "saves" / "SharedWorld" / "level.dat");
	Touch(knownOnlyRoot / "saves" / "KnownWorld" / "level.dat");

	auto known = std::make_shared<FakeProvider>();
	known->id = "known";
	known->locations = {
		Location(sharedRoot, DiscoveryEvidenceKind::KnownLocation, known->id),
		Location(knownOnlyRoot, DiscoveryEvidenceKind::KnownLocation, known->id)};
	auto pcl = std::make_shared<FakeProvider>();
	pcl->id = "pcl2";
	pcl->locations = {
		Location(sharedRoot, DiscoveryEvidenceKind::LauncherSettings, pcl->id)};
	auto failing = std::make_shared<FakeProvider>();
	failing->id = "broken";
	failing->fail = true;

	Config existing;
	existing.saveRoot = (knownOnlyRoot / "saves" / ".." / "saves").wstring();
	MinecraftInstanceDiscoveryService service({known, failing, pcl});
	const auto result = service.Discover({{7, existing}});
	Expect(result.instances.size() == 2,
		"Known and PCL locations should aggregate into two canonical instances");
	Expect(!result.instances[0].alreadyConfigured && result.instances[1].alreadyConfigured,
		"unconfigured instances should sort before an existing canonical save root");
	Expect(PathIdentity::PathsEqual(result.instances[0].instance.savesRoot,
			sharedRoot / "saves"),
		"launcher-settings evidence should win stable ordering among new instances");
	Expect(result.instances[0].instance.evidence.size() == 2,
		"duplicate Known and PCL locations should merge evidence on one instance");
	Expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& item) {
		return item.code == "provider_failed" && item.providerId == "broken";
	}), "a provider failure should not discard other discovery results");

	std::stop_source stopped;
	stopped.request_stop();
	const auto cancelled = service.Discover({}, {}, stopped.get_token());
	Expect(cancelled.instances.empty(),
		"cancelled aggregation should not inspect provider locations");

	std::error_code error;
	std::filesystem::remove_all(root, error);
	if (failures != 0) return 1;
	std::cout << "All Minecraft discovery service tests passed\n";
	return 0;
}
