#include "Pcl2ProcessDiscoveryProvider.h"

#include "text_to_text.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <utility>

using namespace std;

namespace {

constexpr uintmax_t MaximumSetupBytes = 1024u * 1024u;
constexpr wchar_t Pcl2ExecutableName[] = L"Plain Craft Launcher 2.exe";
constexpr wchar_t PclceExecutableName[] = L"PCL2_CE_Release_x64.exe";
constexpr wchar_t PclceExecutableName2[] = L"PCL2_CE_Beta_x64.exe";

BoundedTextFileResult ReadTextFile(
	const filesystem::path& path,
	uintmax_t maximumBytes) {
	error_code error;
	const bool exists = filesystem::exists(path, error);
	if (error) return {BoundedTextFileStatus::ReadFailed, {}, error};
	if (!exists) return {BoundedTextFileStatus::Missing};
	if (!filesystem::is_regular_file(path, error)) {
		return {error ? BoundedTextFileStatus::ReadFailed
			: BoundedTextFileStatus::NotRegular, {}, error};
	}
	const auto size = filesystem::file_size(path, error);
	if (error) return {BoundedTextFileStatus::ReadFailed, {}, error};
	if (size > maximumBytes) return {BoundedTextFileStatus::TooLarge};
	ifstream input(path, ios::binary);
	if (!input) return {BoundedTextFileStatus::ReadFailed};
	string contents(static_cast<size_t>(size), '\0');
	input.read(contents.data(), static_cast<streamsize>(contents.size()));
	contents.resize(static_cast<size_t>(input.gcount()));
	if (input.bad()) return {BoundedTextFileStatus::ReadFailed};
	return {BoundedTextFileStatus::Loaded, std::move(contents)};
}

bool IsDirectory(const filesystem::path& path, error_code& error) {
	error.clear();
	return filesystem::is_directory(path, error) && !error;
}

vector<filesystem::path> ListChildDirectories(
	const filesystem::path& root,
	error_code& error) {
	vector<filesystem::path> children;
	error.clear();
	filesystem::directory_iterator iterator(
		root, filesystem::directory_options::skip_permission_denied, error);
	if (error) return children;
	const filesystem::directory_iterator end;
	for (; iterator != end; iterator.increment(error)) {
		if (error) break;
		error_code statusError;
		if (iterator->is_directory(statusError) && !statusError) {
			children.push_back(iterator->path());
		}
	}
	return children;
}

wstring Lower(wstring value) {
	transform(value.begin(), value.end(), value.begin(), ::towlower);
	return value;
}

string Trim(string value) {
	const auto keep = [](unsigned char character) {
		return character != ' ' && character != '\t' && character != '\r'
			&& character != '\n';
	};
	value.erase(value.begin(), find_if(value.begin(), value.end(), keep));
	value.erase(find_if(value.rbegin(), value.rend(), keep).base(), value.end());
	return value;
}

bool EqualsAsciiIgnoreCase(string left, string right) {
	if (left.size() != right.size()) return false;
	for (size_t index = 0; index < left.size(); ++index) {
		const auto fold = [](unsigned char character) {
			return character >= 'A' && character <= 'Z'
				? static_cast<unsigned char>(character - 'A' + 'a') : character;
		};
		if (fold(static_cast<unsigned char>(left[index]))
			!= fold(static_cast<unsigned char>(right[index]))) return false;
	}
	return true;
}

bool IsMissing(error_code error) {
	return error == make_error_code(errc::no_such_file_or_directory)
		|| error == make_error_code(errc::not_a_directory);
}

void Report(
	const MinecraftDiscoveryContext& context,
	string code,
	const filesystem::path& relatedPath,
	error_code error = {}) {
	if (context.reportDiagnostic) {
		context.reportDiagnostic({std::move(code), "pcl2", relatedPath, error});
	}
}

optional<string> LaunchFolderValue(
	const MinecraftDiscoveryContext& context,
	const filesystem::path& setupPath,
	const BoundedTextFileResult& file) {
	if (file.status != BoundedTextFileStatus::Loaded) {
		if (file.status == BoundedTextFileStatus::Missing) return nullopt;
		const char* code = file.status == BoundedTextFileStatus::TooLarge
			? "pcl2_setup_too_large"
			: file.status == BoundedTextFileStatus::NotRegular
				? "pcl2_setup_not_regular" : "pcl2_setup_read_failed";
		Report(context, code, setupPath, file.filesystemError);
		return nullopt;
	}

	const auto sanitized = SanitizeUtf8(file.contents, MaximumSetupBytes);
	if (sanitized.invalidUtf8Replaced || sanitized.truncated
		|| sanitized.value.find('\0') != string::npos) {
		Report(context, "pcl2_setup_malformed", setupPath);
	}
	optional<string> selected;
	size_t lineStart = 0;
	while (lineStart <= sanitized.value.size()) {
		const size_t lineEnd = sanitized.value.find('\n', lineStart);
		string line = sanitized.value.substr(lineStart,
			lineEnd == string::npos ? string::npos : lineEnd - lineStart);
		if (lineStart == 0 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
		const size_t separator = line.find(':');
		if (separator != string::npos
			&& EqualsAsciiIgnoreCase(Trim(line.substr(0, separator)), "LaunchFolderSelect")) {
			const string value = Trim(line.substr(separator + 1));
			if (!value.empty()) selected = value;
		}
		if (lineEnd == string::npos) break;
		lineStart = lineEnd + 1;
	}
	return selected;
}

optional<filesystem::path> ExpandLaunchFolder(
	const MinecraftDiscoveryContext& context,
	const filesystem::path& executableDirectory,
	const filesystem::path& setupPath,
	const string& value) {
	const wstring wide = utf8_to_wstring(value);
	if (wide.empty()) {
		Report(context, "pcl2_launch_folder_malformed", setupPath);
		return nullopt;
	}
	if (wide.front() == L'$') {
		// PCL 的 $ 以启动器目录为锚点；字符串拼接保留其后已有的分隔符语义。
		return filesystem::path(executableDirectory.wstring() + wide.substr(1)).lexically_normal();
	}
	filesystem::path path(wide);
	if (path.is_absolute()) return path.lexically_normal();
	Report(context, "pcl2_launch_folder_relative", setupPath);
	return nullopt;
}

} // namespace

Pcl2ProcessDiscoveryProvider::Pcl2ProcessDiscoveryProvider(
	shared_ptr<IProcessInspectionService> processInspection,
	Pcl2DiscoveryDependencies dependencies)
	: processInspection_(std::move(processInspection)),
	  dependencies_(std::move(dependencies)) {
	if (!dependencies_.readTextFile) dependencies_.readTextFile = ReadTextFile;
	if (!dependencies_.isDirectory) dependencies_.isDirectory = IsDirectory;
	if (!dependencies_.listChildDirectories) {
		dependencies_.listChildDirectories = ListChildDirectories;
	}
}

string Pcl2ProcessDiscoveryProvider::Id() const {
	return "pcl2";
}

vector<DiscoveryLocation> Pcl2ProcessDiscoveryProvider::DiscoverLocations(
	const MinecraftDiscoveryContext& context,
	stop_token stopToken) {
	vector<DiscoveryLocation> locations;
	if (!processInspection_ || stopToken.stop_requested()) return locations;
	const auto processes = processInspection_->ListRunningProcesses(stopToken);
	for (const auto& process : processes) {
		if (stopToken.stop_requested()) break;
		if (Lower(process.executableName) != Lower(Pcl2ExecutableName) && Lower(process.executableName) != Lower(PclceExecutableName) && Lower(process.executableName) != Lower(PclceExecutableName2))
			continue;
		if (process.executablePath.empty()) {
			Report(context, "pcl2_image_path_unavailable", {});
			continue;
		}
		const filesystem::path executableDirectory = process.executablePath.parent_path();
		if (executableDirectory.empty()) {
			Report(context, "pcl2_image_path_invalid", process.executablePath);
			continue;
		}
		const DiscoveryEvidence processEvidence{
			DiscoveryEvidenceKind::LauncherProcess, Id(), process.executablePath};
		const auto addWorkspace = [&](const filesystem::path& path) {
			locations.push_back({path, DiscoveryLocationKind::MinecraftRoot, {
				processEvidence,
				{DiscoveryEvidenceKind::WorkspaceProbe, Id(), executableDirectory}}});
		};

		const filesystem::path setupPath = executableDirectory / L"PCL" / L"Setup.ini";
		const auto setup = dependencies_.readTextFile(setupPath, MaximumSetupBytes);
		if (const auto selected = LaunchFolderValue(context, setupPath, setup)) {
			if (const auto expanded = ExpandLaunchFolder(
				context, executableDirectory, setupPath, *selected)) {
				locations.push_back({*expanded, DiscoveryLocationKind::MinecraftRoot, {{
					DiscoveryEvidenceKind::LauncherSettings, Id(), setupPath}, processEvidence}});
			}
		}

		addWorkspace(executableDirectory);
		error_code statusError;
		if (dependencies_.isDirectory(executableDirectory / L".minecraft", statusError)) {
			addWorkspace(executableDirectory / L".minecraft");
		}
		else if (statusError && !IsMissing(statusError)) {
			Report(context, "pcl2_workspace_unreadable",
				executableDirectory / L".minecraft", statusError);
		}

		error_code listError;
		for (const auto& child : dependencies_.listChildDirectories(
			executableDirectory, listError)) {
			if (stopToken.stop_requested()) break;
			bool accepted = Lower(child.filename().wstring()) == L".minecraft";
			if (!accepted) {
				error_code versionsError;
				accepted = dependencies_.isDirectory(child / L"versions", versionsError);
				if (versionsError && !IsMissing(versionsError)) {
					Report(context, "pcl2_workspace_unreadable",
						child / L"versions", versionsError);
				}
			}
			if (accepted) addWorkspace(child);
		}
		if (listError) {
			Report(context, "pcl2_workspace_children_unreadable",
				executableDirectory, listError);
		}
	}
	return locations;
}
