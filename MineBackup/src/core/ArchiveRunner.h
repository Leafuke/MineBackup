#pragma once

#include "AppPaths.h"
#include "AppState.h"
#include "ExternalToolManager.h"
#include "ProcessRunner.h"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

class ArchiveRunner {
public:
	using ProcessExecutor = std::function<ProcessResult(const ProcessSpec&, std::stop_token)>;

	static ArchiveRunner Resolve(
		const std::filesystem::path& configuredExecutable,
		const AppPaths& paths,
		std::stop_token stopToken = {},
		ProcessExecutor executor = {});

	// 测试入口：调用方可以提供已解析的工具和纯内存进程执行器。
	ArchiveRunner(
		ExternalToolResolution resolution,
		std::stop_token stopToken = {},
		ProcessExecutor executor = {});

	bool IsAvailable() const;
	const ExternalToolResolution& Resolution() const;

	ProcessResult Execute(
		std::vector<std::wstring> arguments,
		const std::filesystem::path& workingDirectory = {},
		bool useLowPriority = false) const;
	bool ExecuteLogged(
		std::vector<std::wstring> arguments,
		const std::filesystem::path& workingDirectory = {},
		bool useLowPriority = false) const;

	static std::vector<std::wstring> BuildCreateArguments(
		const Config& config,
		int compressionLevel,
		const std::filesystem::path& archive);

private:
	ExternalToolResolution resolution_;
	std::stop_token stopToken_;
	ProcessExecutor executor_;
};
