#include "ProcessInspectionService.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
	if (condition) return;
	++failures;
	std::cerr << "FAIL: " << message << '\n';
}

} // namespace

int main() {
	const auto service = CreateProcessInspectionService();
	Expect(service != nullptr, "process inspection factory should return a service");
	if (!service) return 1;

	const auto status = GetProcessInspectionAvailability();
	const auto processes = service->ListRunningProcesses({});
#ifdef _WIN32
	Expect(status == ProcessInspectionAvailability::Available,
		"Windows process inspection should be applicable");
	Expect(!processes.empty(),
		"Windows process inspection should find at least the test process");
	Expect(std::all_of(processes.begin(), processes.end(), [](const auto& process) {
		return !process.executableName.empty()
			&& !process.executablePath.empty()
			&& process.executablePath.is_absolute()
			&& process.executablePath.filename().wstring() == process.executableName;
	}), "Windows results should contain absolute image paths and matching names");
#else
	Expect(status == ProcessInspectionAvailability::NotApplicable,
		"non-Windows process inspection should be NotApplicable");
	Expect(processes.empty(),
		"non-Windows process inspection should return no fabricated results");
#endif

	std::stop_source cancelled;
	cancelled.request_stop();
	Expect(service->ListRunningProcesses(cancelled.get_token()).empty(),
		"cancelled process inspection should not enumerate processes");

	if (failures != 0) return 1;
	std::cout << "All process inspection tests passed\n";
	return 0;
}
