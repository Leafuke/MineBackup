#pragma once

#include "DataModels.h"
#include "ProcessRunner.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

struct RcloneClientOptions {
	std::filesystem::path executable;
	std::filesystem::path workingDirectory;
	std::chrono::milliseconds timeout{0};
	int retryCount = 0;
	bool useLowPriority = false;
	std::stop_token stopToken;
};

struct RcloneExecutionResult {
	CloudCommandResult command;
	int attemptCount = 0;
	bool cancelled = false;
};

class RcloneClient {
public:
	using ProcessExecutor = std::function<ProcessResult(const ProcessSpec&, std::stop_token)>;

	explicit RcloneClient(RcloneClientOptions options, ProcessExecutor executor = {});

	RcloneExecutionResult CopyTo(
		const std::wstring& sourcePath,
		const std::wstring& destinationPath) const;
	RcloneExecutionResult Copy(
		const std::wstring& sourcePath,
		const std::wstring& destinationPath) const;
	RcloneExecutionResult Execute(const std::vector<std::wstring>& arguments) const;

	static ProcessSpec BuildCopyToCommand(
		const std::filesystem::path& executable,
		const std::wstring& sourcePath,
		const std::wstring& destinationPath);
	static ProcessSpec BuildCopyCommand(
		const std::filesystem::path& executable,
		const std::wstring& sourcePath,
		const std::wstring& destinationPath);
	static bool IsRemoteObjectMissing(const CloudCommandResult& result);

private:
	RcloneClientOptions options_;
	ProcessExecutor executor_;
};
