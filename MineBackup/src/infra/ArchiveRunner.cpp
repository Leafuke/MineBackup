#include "ArchiveRunner.h"

#include "Logging.h"
#include "text_to_text.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <limits>

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
	bool useLowPriority,
	size_t maximumCapturedBytes,
	chrono::milliseconds timeout) const {
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
	spec.maximumCapturedBytes = maximumCapturedBytes;
	spec.timeout = timeout;
	return executor_(spec, stopToken_);
}

bool ArchiveRunner::ValidateMemberListing(const string& listing, string& error) {
 error.clear(); istringstream input(listing); string line; bool members=false; bool sawPath=false;
 while (getline(input,line)) {
  if (!line.empty() && line.back()=='\r') line.pop_back();
  if (line=="----------") {members=true; continue;}
  if (members && (line.starts_with("Symbolic Link = ") || line.starts_with("Hard Link = ")
      || (line.starts_with("Attributes = ") && line.find("l", 13) != string::npos))) {
   error="archive links are not supported"; return false;
  }
  if (!members || !line.starts_with("Path = ")) continue;
  sawPath=true; string path=line.substr(7); replace(path.begin(),path.end(),'\\','/');
  if (path.empty() || path.front()=='/' || path.find(':')!=string::npos) {error="unsupported absolute archive member: "+path; return false;}
  istringstream parts(path); string part;
  while(getline(parts,part,'/')) if(part==".." || part.empty()) {error="unsafe archive member: "+path; return false;}
 }
 if (!members || !sawPath) {error="archive member listing is missing or unrecognized"; return false;}
 return true;
}

bool ArchiveRunner::ValidateMembers(const filesystem::path& archive, string& error, bool lowPriority) const {
 const auto listed=Execute({L"l",L"-slt",L"-sccUTF-8",archive.wstring()}, {},lowPriority, 64u * 1024u * 1024u, chrono::seconds(60));
 if(listed.status!=ProcessStatus::Succeeded || listed.outputTruncated) {error="could not inspect archive member paths"; return false;}
 return ValidateMemberListing(listed.standardOutput,error);
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
