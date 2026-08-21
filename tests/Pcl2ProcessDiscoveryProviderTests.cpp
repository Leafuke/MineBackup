#include "Pcl2ProcessDiscoveryProvider.h"

#include "PathIdentity.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
	if (condition) return;
	++failures;
	std::cerr << "FAIL: " << message << '\n';
}

class FakeProcessInspection final : public IProcessInspectionService {
public:
	std::vector<RunningProcessInfo> processes;

	std::vector<RunningProcessInfo> ListRunningProcesses(std::stop_token stopToken) override {
		return stopToken.stop_requested() ? std::vector<RunningProcessInfo>{} : processes;
	}
};

void Write(const std::filesystem::path& path, const std::string& contents) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary | std::ios::trunc) << contents;
}

std::size_t CountPath(
	const std::vector<DiscoveryLocation>& locations,
	const std::filesystem::path& path) {
	return static_cast<std::size_t>(std::count_if(
		locations.begin(), locations.end(), [&](const auto& location) {
			return PathIdentity::PathsEqual(location.path, path);
		}));
}

bool HasDiagnostic(
	const std::vector<DiscoveryDiagnostic>& diagnostics,
	const std::string& code) {
	return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) {
		return diagnostic.code == code && diagnostic.providerId == "pcl2";
	});
}

MinecraftDiscoveryContext DiagnosticsInto(std::vector<DiscoveryDiagnostic>& diagnostics) {
	return {[&](DiscoveryDiagnostic diagnostic) {
		diagnostics.push_back(std::move(diagnostic));
	}};
}

} // namespace

int main() {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path())
		/ ("MineBackupPcl2Provider-" + std::to_string(stamp));
	const auto workspace = root / "workspace";
	const auto executable = workspace / "Plain Craft Launcher 2.exe";
	const auto setup = workspace / "PCL" / "Setup.ini";
	const auto selected = workspace / ".minecraft";
	const auto external = root / "external" / ".minecraft";
	const auto childRoot = workspace / "InstanceA";
	std::filesystem::create_directories(selected);
	std::filesystem::create_directories(external);
	std::filesystem::create_directories(childRoot / "versions");
	std::filesystem::create_directories(workspace / "nested" / "too-deep" / "versions");
	Write(executable, "test");
	Write(setup, "ignored:1\nLaunchFolderSelect:$/.minecraft\nmalformed\n");

	auto fake = std::make_shared<FakeProcessInspection>();
	fake->processes = {
		{L"Plain Craft Launcher 2 Beta.exe", root / "ignored" / "launcher.exe"},
		{L"pLaIn CrAfT lAuNcHeR 2.ExE", executable}
	};
	Pcl2ProcessDiscoveryProvider provider(fake);
	std::vector<DiscoveryDiagnostic> diagnostics;
	auto locations = provider.DiscoverLocations(DiagnosticsInto(diagnostics), {});
	Expect(CountPath(locations, selected) >= 2,
		"PCL $ selection and workspace probe should both retain evidence for aggregation");
	Expect(CountPath(locations, workspace) == 1
			&& CountPath(locations, childRoot) == 1,
		"PCL provider should check the executable directory and direct version roots");
	Expect(CountPath(locations, workspace / "nested" / "too-deep") == 0,
		"PCL workspace probing must not recurse beyond direct children");
	Expect(std::any_of(locations.begin(), locations.end(), [&](const auto& location) {
		return PathIdentity::PathsEqual(location.path, selected)
			&& std::any_of(location.evidence.begin(), location.evidence.end(), [](const auto& evidence) {
				return evidence.kind == DiscoveryEvidenceKind::LauncherSettings;
			});
	}), "Setup.ini selection should carry launcher-settings evidence");

	Write(setup, "LaunchFolderSelect:" + external.string() + "\n");
	diagnostics.clear();
	locations = provider.DiscoverLocations(DiagnosticsInto(diagnostics), {});
	Expect(CountPath(locations, external) == 1,
		"an absolute LaunchFolderSelect outside the PCL workspace should be accepted");

	Write(setup, "LaunchFolderSelect:relative/path\n");
	diagnostics.clear();
	locations = provider.DiscoverLocations(DiagnosticsInto(diagnostics), {});
	Expect(HasDiagnostic(diagnostics, "pcl2_launch_folder_relative"),
		"unexpected relative PCL selections should be ignored with a diagnostic");

	Write(setup, std::string(1024 * 1024 + 1, 'x'));
	diagnostics.clear();
	locations = provider.DiscoverLocations(DiagnosticsInto(diagnostics), {});
	Expect(HasDiagnostic(diagnostics, "pcl2_setup_too_large"),
		"PCL Setup.ini reads should enforce the one-MiB bound");

	fake->processes = {
		{L"Plain Craft Launcher 2.exe", executable},
		{L"Plain Craft Launcher 2.exe", executable},
		{L"Plain Craft Launcher 2.exe", {}}
	};
	diagnostics.clear();
	locations = provider.DiscoverLocations(DiagnosticsInto(diagnostics), {});
	Expect(CountPath(locations, workspace) == 2,
		"multiple PCL processes may return duplicates for the aggregator to merge");
	Expect(HasDiagnostic(diagnostics, "pcl2_image_path_unavailable"),
		"an unavailable image path should not stop other PCL process probes");

	std::stop_source stopped;
	stopped.request_stop();
	Expect(provider.DiscoverLocations({}, stopped.get_token()).empty(),
		"cancelled PCL discovery should not enumerate workspaces");

	std::error_code error;
	std::filesystem::remove_all(root, error);
	if (failures != 0) return 1;
	std::cout << "All PCL2 discovery provider tests passed\n";
	return 0;
}
