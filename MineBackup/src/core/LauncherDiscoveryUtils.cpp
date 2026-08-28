#include "LauncherDiscoveryUtils.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace LauncherDiscoveryUtils {

namespace {

std::wstring LowerForSort(std::wstring value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
	return value;
}

} // namespace

std::optional<std::filesystem::path> ReadEnvironmentPath(std::string_view name) {
	if (name.empty()) return std::nullopt;
#ifdef _WIN32
	std::wstring wideName(name.begin(), name.end());
	const DWORD required = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
	if (required == 0) return std::nullopt;
	std::wstring value(required, L'\0');
	const DWORD written = GetEnvironmentVariableW(
		wideName.c_str(), value.data(), static_cast<DWORD>(value.size()));
	if (written == 0 || written >= required) return std::nullopt;
	value.resize(written);
	if (value.empty()) return std::nullopt;
	return std::filesystem::path(value);
#else
	std::string strName(name);
	const char* val = std::getenv(strName.c_str());
	if (val == nullptr || *val == '\0') return std::nullopt;
	return std::filesystem::path(val);
#endif
}

std::optional<std::filesystem::path> ResolveHomeDirectory() {
#ifdef _WIN32
	return ReadEnvironmentPath("USERPROFILE");
#else
	return ReadEnvironmentPath("HOME");
#endif
}

std::optional<std::filesystem::path> ResolveUserDataRoot() {
#ifdef _WIN32
	return ReadEnvironmentPath("APPDATA");
#elif defined(__APPLE__)
	auto home = ResolveHomeDirectory();
	if (!home.has_value() || home->empty()) return std::nullopt;
	return *home / "Library" / "Application Support";
#else
	// Linux: 优先读取绝对路径的 XDG_DATA_HOME，否则回退到 HOME/.local/share
	if (auto xdg = ReadEnvironmentPath("XDG_DATA_HOME"); xdg.has_value() && xdg->is_absolute()) {
		return xdg;
	}
	auto home = ResolveHomeDirectory();
	if (!home.has_value() || home->empty()) return std::nullopt;
	return *home / ".local" / "share";
#endif
}

BoundedTextFileResult ReadBoundedTextFile(const std::filesystem::path& path) {
	BoundedTextFileResult result;
	if (path.empty()) {
		result.status = BoundedTextFileStatus::Missing;
		return result;
	}

	std::error_code ec;
	const bool exists = std::filesystem::exists(path, ec);
	if (ec) {
		if (IsMissingFilesystemError(ec)) {
			result.status = BoundedTextFileStatus::Missing;
		}
		else {
			result.status = BoundedTextFileStatus::ReadFailed;
			result.filesystemError = ec;
		}
		return result;
	}
	if (!exists) {
		result.status = BoundedTextFileStatus::Missing;
		return result;
	}

	const bool regular = std::filesystem::is_regular_file(path, ec);
	if (ec) {
		result.status = BoundedTextFileStatus::ReadFailed;
		result.filesystemError = ec;
		return result;
	}
	if (!regular) {
		result.status = BoundedTextFileStatus::NotRegular;
		return result;
	}

	const auto size = std::filesystem::file_size(path, ec);
	if (ec) {
		result.status = BoundedTextFileStatus::ReadFailed;
		result.filesystemError = ec;
		return result;
	}
	if (size > MaximumConfigBytes) {
		result.status = BoundedTextFileStatus::TooLarge;
		return result;
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream.is_open()) {
		result.status = BoundedTextFileStatus::ReadFailed;
		return result;
	}

	result.contents.resize(static_cast<std::size_t>(size));
	if (size > 0) {
		stream.read(result.contents.data(), static_cast<std::streamsize>(size));
		const auto bytesRead = static_cast<std::size_t>(stream.gcount());
		if (bytesRead != size && !stream.eof()) {
			result.status = BoundedTextFileStatus::ReadFailed;
			result.contents.clear();
			return result;
		}
		result.contents.resize(bytesRead);
	}

	result.status = BoundedTextFileStatus::Loaded;
	return result;
}

bool IsDirectory(
	const std::filesystem::path& path,
	std::error_code& error) {
	error.clear();
	const bool directory = std::filesystem::is_directory(path, error);
	return directory && !error;
}

bool IsRegularFile(
	const std::filesystem::path& path,
	std::error_code& error) {
	error.clear();
	const bool regular = std::filesystem::is_regular_file(path, error);
	return regular && !error;
}

std::vector<std::filesystem::path> ListChildDirectories(
	const std::filesystem::path& root,
	std::error_code& error) {
	error.clear();
	std::vector<std::filesystem::path> children;
	if (root.empty()) return children;

	std::filesystem::directory_iterator iterator(
		root, std::filesystem::directory_options::skip_permission_denied, error);
	if (error) return children;

	const std::filesystem::directory_iterator end;
	for (; iterator != end; iterator.increment(error)) {
		if (error) {
			error.clear();
			continue;
		}
		const auto current = iterator->path();
		std::error_code statusError;
		if (IsDirectory(current, statusError)) {
			children.push_back(current);
		}
	}
	error.clear();

	std::sort(children.begin(), children.end(),
		[](const std::filesystem::path& left, const std::filesystem::path& right) {
			const auto leftName = LowerForSort(left.filename().wstring());
			const auto rightName = LowerForSort(right.filename().wstring());
			if (leftName != rightName) return leftName < rightName;
			return left.lexically_normal().wstring() < right.lexically_normal().wstring();
		});

	return children;
}

bool IsMissingFilesystemError(std::error_code error) {
	if (!error) return false;
	if (error == std::errc::no_such_file_or_directory) return true;
#ifdef _WIN32
	if (error.category() == std::system_category()) {
		if (error.value() == 2 /* ERROR_FILE_NOT_FOUND */ ||
			error.value() == 3 /* ERROR_PATH_NOT_FOUND */) {
			return true;
		}
	}
#endif
	return false;
}

} // namespace LauncherDiscoveryUtils
