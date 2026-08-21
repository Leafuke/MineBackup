#pragma once

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

struct RunningProcessInfo {
	std::wstring executableName;
	std::filesystem::path executablePath;
};

enum class ProcessInspectionAvailability {
	Available,
	NotApplicable
};

class IProcessInspectionService {
public:
	virtual ~IProcessInspectionService() = default;
	virtual std::vector<RunningProcessInfo> ListRunningProcesses(
		std::stop_token stopToken) = 0;
};

// 供 Provider 区分“当前平台不适用”和“适用但没有命中进程”。
ProcessInspectionAvailability GetProcessInspectionAvailability() noexcept;

std::shared_ptr<IProcessInspectionService> CreateProcessInspectionService();
