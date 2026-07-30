#include "ArchiveRunner.h"

#include "Logging.h"
#include "text_to_text.h"

using namespace std;

ArchiveRunner ArchiveRunner::Resolve(
	const filesystem::path& configuredExecutable,
	const AppPaths& paths,
	stop_token stopToken,
	ProcessExecutor executor) {
	return ArchiveRunner(
		ExternalToolManager::ResolveSevenZip(configuredExecutable, paths, stopToken),
		stopToken,
		std::move(executor));
}

ArchiveRunner::ArchiveRunner(
	ExternalToolResolution resolution,
	stop_token stopToken,
	ProcessExecutor executor)
	: resolution_(std::move(resolution)),
	  stopToken_(stopToken),
	  executor_(executor ? std::move(executor) : ProcessRunner::Run) {
}

bool ArchiveRunner::IsAvailable() const {
	return resolution_.available && !resolution_.executable.empty();
}

const ExternalToolResolution& ArchiveRunner::Resolution() const {
	return resolution_;
}

ProcessResult ArchiveRunner::Execute(
	vector<wstring> arguments,
	const filesystem::path& workingDirectory,
	bool useLowPriority) const {
	ProcessResult unavailable;
	if (!IsAvailable()) {
		unavailable.status = ProcessStatus::FailedToStart;
		unavailable.error = resolution_.diagnostic.empty()
			? L"No verified 7-Zip executable is available."
			: resolution_.diagnostic;
		return unavailable;
	}

	ProcessSpec spec;
	spec.executable = resolution_.executable;
	spec.arguments = std::move(arguments);
	spec.workingDirectory = workingDirectory;
	spec.useLowPriority = useLowPriority;
	return executor_(spec, stopToken_);
}

bool ArchiveRunner::ExecuteLogged(
	vector<wstring> arguments,
	const filesystem::path& workingDirectory,
	bool useLowPriority) const {
	minebackup::logging::ScopedLogContext context{{
		"executable", wstring_to_utf8(resolution_.executable.filename().wstring())},
		{"working_directory", workingDirectory.empty() ? "default" : "custom"}};
	MB_LOG_DEBUG(minebackup::logging::LogCategory::Process,
		"process.started", "External process started.");
	const auto result = Execute(std::move(arguments), workingDirectory, useLowPriority);
	if (!result.standardOutput.empty()) {
		minebackup::logging::LogRaw(
			minebackup::logging::LogCategory::Process,
			"process.stdout",
			result.standardOutput,
			minebackup::logging::LogLevel::Debug,
			MB_LOG_SOURCE);
	}
	if (!result.standardError.empty()) {
		minebackup::logging::LogRaw(
			minebackup::logging::LogCategory::Process,
			"process.stderr",
			result.standardError,
			minebackup::logging::LogLevel::Debug,
			MB_LOG_SOURCE);
	}
	if (result.status == ProcessStatus::Succeeded) {
		MB_LOG_INFO(
			minebackup::logging::LogCategory::Process,
			"process.completed",
			"External process completed successfully.");
		return true;
	}
	MB_LOG_ERROR(
		minebackup::logging::LogCategory::Process,
		"process.failed",
		"External process failed with exit code {}: {}",
		result.exitCode,
		wstring_to_utf8(result.error));
	return false;
}

vector<wstring> ArchiveRunner::BuildCreateArguments(
	const Config& config,
	int compressionLevel,
	const filesystem::path& archive) {
	return {
		L"a",
		L"-t" + config.zipFormat,
		L"-m0=" + config.zipMethod,
		L"-mx=" + to_wstring(compressionLevel),
		config.cpuThreads == 0 ? L"-mmt" : L"-mmt" + to_wstring(config.cpuThreads),
		L"-ssw",
		archive.wstring()
	};
}
