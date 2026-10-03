#include "RcloneClient.h"

#include "Logging.h"
#include "text_to_text.h"

#include <algorithm>
#include <cwctype>
#include <utility>

using namespace std;

namespace {
	ProcessResult RunProcess(const ProcessSpec& spec, stop_token stopToken) {
		return ProcessRunner::Run(spec, stopToken);
	}

	void LogProcessOutput(const ProcessResult& process) {
		if (!process.standardOutput.empty()) {
			minebackup::logging::LogRaw(
				minebackup::logging::LogCategory::Process,
				"process.stdout",
				process.standardOutput,
				minebackup::logging::LogLevel::Debug,
				MB_LOG_SOURCE);
		}
		if (!process.standardError.empty()) {
			minebackup::logging::LogRaw(
				minebackup::logging::LogCategory::Process,
				"process.stderr",
				process.standardError,
				minebackup::logging::LogLevel::Debug,
				MB_LOG_SOURCE);
		}
	}
}

RcloneClient::RcloneClient(RcloneClientOptions options, ProcessExecutor executor)
	: options_(std::move(options)),
	  executor_(executor ? std::move(executor) : ProcessExecutor(RunProcess)) {
	options_.retryCount = max(0, options_.retryCount);
}

RcloneExecutionResult RcloneClient::CopyTo(
	const wstring& sourcePath,
	const wstring& destinationPath) const {
	return Execute({L"copyto", sourcePath, destinationPath, L"--progress"});
}

RcloneExecutionResult RcloneClient::Copy(
	const wstring& sourcePath,
	const wstring& destinationPath) const {
	return Execute({L"copy", sourcePath, destinationPath, L"--progress"});
}

RcloneExecutionResult RcloneClient::Execute(const vector<wstring>& arguments) const {
	RcloneExecutionResult result;
	for (int attempt = 0; attempt <= options_.retryCount; ++attempt) {
		if (options_.stopToken.stop_requested()) {
			result.cancelled = true;
			result.command.detail = L"rclone command cancelled.";
			break;
		}

		ProcessSpec invocation;
		invocation.executable = options_.executable;
		invocation.arguments = arguments;
		invocation.workingDirectory = options_.workingDirectory;
		invocation.timeout = options_.timeout;
		invocation.useLowPriority = options_.useLowPriority;

		const ProcessResult process = executor_(invocation, options_.stopToken);
		++result.attemptCount;
		LogProcessOutput(process);

		result.command.success = process.status == ProcessStatus::Succeeded;
		result.command.exitCode = process.exitCode;
		result.command.timedOut = process.status == ProcessStatus::TimedOut;
		result.cancelled = process.status == ProcessStatus::Cancelled;
		if (!process.error.empty()) {
			result.command.detail = process.error;
		}
		else if (!process.standardError.empty()) {
			result.command.detail = utf8_to_wstring(process.standardError);
		}
		else if (!result.command.success) {
			result.command.detail = L"rclone failed with exit code " + to_wstring(process.exitCode);
		}

		if (result.command.success) {
			MB_LOG_DEBUG(
				minebackup::logging::LogCategory::Cloud,
				"cloud.command.completed",
				"Cloud command completed (attempt={}, exit_code=0)",
				attempt + 1);
			break;
		}

		MB_LOG_ERROR(
			minebackup::logging::LogCategory::Cloud,
			result.command.timedOut ? "cloud.command.timeout"
				: (result.cancelled ? "cloud.command.cancelled" : "cloud.command.failed"),
			"Cloud command failed (attempt={}, exit_code={}, timed_out={}, cancelled={})",
			attempt + 1,
			process.exitCode,
			result.command.timedOut,
			result.cancelled);

		// 取消来自任务协调器，继续重试只会延迟关闭，并且不会产生不同结果。
		if (result.cancelled) {
			break;
		}
	}
	return result;
}

ProcessSpec RcloneClient::BuildCopyToCommand(
	const filesystem::path& executable,
	const wstring& sourcePath,
	const wstring& destinationPath) {
	ProcessSpec spec;
	spec.executable = executable;
	spec.arguments = {L"copyto", sourcePath, destinationPath, L"--progress"};
	return spec;
}

ProcessSpec RcloneClient::BuildCopyCommand(
	const filesystem::path& executable,
	const wstring& sourcePath,
	const wstring& destinationPath) {
	ProcessSpec spec;
	spec.executable = executable;
	spec.arguments = {L"copy", sourcePath, destinationPath, L"--progress"};
	return spec;
}

bool RcloneClient::IsRemoteObjectMissing(const CloudCommandResult& result) {
	if (result.success || result.timedOut) return false;
	// rclone 的 3/4 分别表示目录或文件不存在；认证、配置和后端错误必须继续上报。
	if (result.exitCode == 3 || result.exitCode == 4) return true;
	wstring detail = result.detail;
	transform(detail.begin(), detail.end(), detail.begin(), ::towlower);
	if (detail.find(L"config file") != wstring::npos
		|| detail.find(L"didn't find section") != wstring::npos
		|| detail.find(L"failed to create file system") != wstring::npos
		|| detail.find(L"authentication") != wstring::npos
		|| detail.find(L"unauthorized") != wstring::npos) {
		return false;
	}
	return detail.find(L"object not found") != wstring::npos
		|| detail.find(L"directory not found") != wstring::npos
		|| detail.find(L"file not found") != wstring::npos;
}
