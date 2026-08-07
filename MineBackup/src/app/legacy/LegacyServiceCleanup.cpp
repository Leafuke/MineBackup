#include "LegacyServiceCleanup.h"

#include "LegacyServicePolicy.h"

#ifdef _WIN32
#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>
#endif

#include <chrono>
#include <cwctype>
#include <vector>

using namespace std;

namespace LegacyServiceCleanup {

#ifdef _WIN32
namespace {

struct ServiceHandle {
	SC_HANDLE value = nullptr;
	~ServiceHandle() { if (value) CloseServiceHandle(value); }
	ServiceHandle(const ServiceHandle&) = delete;
	ServiceHandle& operator=(const ServiceHandle&) = delete;
	ServiceHandle() = default;
};

bool IsValidServiceName(const wstring& serviceName) {
	if (serviceName.empty() || serviceName.size() > 256
		|| serviceName.find_first_of(L"/\\") != wstring::npos) {
		return false;
	}
	for (const wchar_t character : serviceName) {
		if (iswcntrl(character)) return false;
	}
	return true;
}

wstring WindowsErrorMessage(DWORD code) {
	wchar_t* buffer = nullptr;
	const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
		| FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
	wstring message = length && buffer
		? wstring(buffer, length) : L"Windows error " + to_wstring(code);
	if (buffer) LocalFree(buffer);
	while (!message.empty() && iswspace(message.back())) message.pop_back();
	return message;
}

bool IsLegacyMineBackupExecutable(const filesystem::path& executable) {
	error_code error;
	if (!filesystem::is_regular_file(executable, error)) return false;
	HMODULE module = LoadLibraryExW(executable.c_str(), nullptr,
		LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (!module) return false;
	const bool hasSevenZip = FindResourceW(module, MAKEINTRESOURCEW(101), L"EXE") != nullptr;
	const bool hasIcon = FindResourceW(
		module, MAKEINTRESOURCEW(102), MAKEINTRESOURCEW(14)) != nullptr;
	FreeLibrary(module);
	return hasSevenZip && hasIcon;
}

Inspection InspectHandle(const wstring& serviceName, SC_HANDLE service) {
	Inspection inspection;
	inspection.serviceName = serviceName;
	DWORD required = 0;
	QueryServiceConfigW(service, nullptr, 0, &required);
	const DWORD sizeQueryError = GetLastError();
	if (sizeQueryError != ERROR_INSUFFICIENT_BUFFER || required == 0) {
		inspection.state = State::QueryFailed;
		inspection.diagnostic = L"Could not read the legacy service configuration: "
			+ WindowsErrorMessage(sizeQueryError);
		return inspection;
	}
	vector<BYTE> storage(required);
	auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
	if (!QueryServiceConfigW(service, config, required, &required)) {
		inspection.state = State::QueryFailed;
		inspection.diagnostic = L"Could not read the legacy service ImagePath: "
			+ WindowsErrorMessage(GetLastError());
		return inspection;
	}
	inspection.imagePath = config->lpBinaryPathName ? config->lpBinaryPathName : L"";
	const auto parsed = ParseLegacyServiceImagePath(inspection.imagePath);
	if (!parsed.valid) {
		inspection.state = State::Unsafe;
		inspection.diagnostic = parsed.diagnostic;
		return inspection;
	}
	inspection.executable = parsed.executable;
	if (!IsLegacyMineBackupExecutable(inspection.executable)) {
		inspection.state = State::Unsafe;
		inspection.diagnostic = L"The service executable is missing or does not contain MineBackup resources.";
		return inspection;
	}
	SERVICE_STATUS_PROCESS status{};
	DWORD statusBytes = 0;
	if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
			reinterpret_cast<BYTE*>(&status), sizeof(status), &statusBytes)) {
		inspection.running = status.dwCurrentState != SERVICE_STOPPED;
	}
	inspection.state = State::Removable;
	inspection.diagnostic = L"The service ImagePath was verified as a legacy MineBackup service.";
	return inspection;
}

wstring QuoteWindowsArgument(const wstring& argument) {
	wstring quoted = L"\"";
	size_t backslashes = 0;
	for (const wchar_t character : argument) {
		if (character == L'\\') {
			++backslashes;
			continue;
		}
		if (character == L'\"') {
			quoted.append(backslashes * 2 + 1, L'\\');
			quoted.push_back(L'\"');
			backslashes = 0;
			continue;
		}
		quoted.append(backslashes, L'\\');
		backslashes = 0;
		quoted.push_back(character);
	}
	quoted.append(backslashes * 2, L'\\');
	quoted.push_back(L'\"');
	return quoted;
}

} // namespace

Inspection Inspect(const wstring& serviceName) {
	Inspection inspection;
	inspection.serviceName = serviceName;
	if (!IsValidServiceName(serviceName)) {
		inspection.state = State::Unsafe;
		inspection.diagnostic = L"The legacy service name is empty or invalid.";
		return inspection;
	}
	ServiceHandle manager;
	manager.value = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!manager.value) {
		inspection.state = State::QueryFailed;
		inspection.diagnostic = L"Could not open Windows Service Control Manager: "
			+ WindowsErrorMessage(GetLastError());
		return inspection;
	}
	ServiceHandle service;
	service.value = OpenServiceW(manager.value, serviceName.c_str(),
		SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
	if (!service.value) {
		const DWORD code = GetLastError();
		if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
			inspection.state = State::NotInstalled;
			inspection.diagnostic = L"No legacy service with this configured name is installed.";
		}
		else {
			inspection.state = State::QueryFailed;
			inspection.diagnostic = L"Could not inspect the configured legacy service: "
				+ WindowsErrorMessage(code);
		}
		return inspection;
	}
	return InspectHandle(serviceName, service.value);
}

bool RequestElevatedRemoval(const wstring& serviceName, wstring& error) {
	error.clear();
	const auto inspection = Inspect(serviceName);
	if (!inspection.CanRemove()) {
		error = inspection.diagnostic;
		return false;
	}
	vector<wchar_t> executable(32768);
	const DWORD length = GetModuleFileNameW(
		nullptr, executable.data(), static_cast<DWORD>(executable.size()));
	if (length == 0 || length >= executable.size()) {
		error = L"Could not locate the current MineBackup executable.";
		return false;
	}
	const wstring parameters = L"--cleanup-legacy-service "
		+ QuoteWindowsArgument(serviceName);
	SHELLEXECUTEINFOW request{sizeof(request)};
	request.fMask = SEE_MASK_NOASYNC;
	request.lpVerb = L"runas";
	request.lpFile = executable.data();
	request.lpParameters = parameters.c_str();
	request.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExW(&request)) {
		const DWORD code = GetLastError();
		error = code == ERROR_CANCELLED
			? L"Administrator approval was cancelled; the service was not changed."
			: L"Could not start the validated service cleanup helper: "
				+ WindowsErrorMessage(code);
		return false;
	}
	return true;
}

bool RemoveAfterValidation(const wstring& serviceName, wstring& error) {
	error.clear();
	if (!IsValidServiceName(serviceName)) {
		error = L"The legacy service name is empty or invalid.";
		return false;
	}
	ServiceHandle manager;
	manager.value = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!manager.value) {
		error = L"Could not open Windows Service Control Manager: "
			+ WindowsErrorMessage(GetLastError());
		return false;
	}
	ServiceHandle service;
	service.value = OpenServiceW(manager.value, serviceName.c_str(),
		SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);
	if (!service.value) {
		error = L"Could not open the legacy service for removal: "
			+ WindowsErrorMessage(GetLastError());
		return false;
	}
	const auto inspection = InspectHandle(serviceName, service.value);
	if (!inspection.CanRemove()) {
		error = L"Safety validation refused to remove the service: " + inspection.diagnostic;
		return false;
	}

	SERVICE_STATUS_PROCESS status{};
	DWORD statusBytes = 0;
	if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
			reinterpret_cast<BYTE*>(&status), sizeof(status), &statusBytes)) {
		error = L"Could not query the legacy service state: "
			+ WindowsErrorMessage(GetLastError());
		return false;
	}
	if (status.dwCurrentState != SERVICE_STOPPED) {
		SERVICE_STATUS ignored{};
		if (!ControlService(service.value, SERVICE_CONTROL_STOP, &ignored)) {
			const DWORD code = GetLastError();
			if (code == ERROR_SERVICE_NOT_ACTIVE) status.dwCurrentState = SERVICE_STOPPED;
			else {
				error = L"Could not stop the legacy service: " + WindowsErrorMessage(code);
				return false;
			}
		}
		const auto deadline = chrono::steady_clock::now() + chrono::seconds(15);
		while (status.dwCurrentState != SERVICE_STOPPED
			&& chrono::steady_clock::now() < deadline) {
			Sleep(250);
			if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
					reinterpret_cast<BYTE*>(&status), sizeof(status), &statusBytes)) {
				error = L"Could not verify that the legacy service stopped: "
					+ WindowsErrorMessage(GetLastError());
				return false;
			}
		}
		if (status.dwCurrentState != SERVICE_STOPPED) {
			error = L"The legacy service did not stop within 15 seconds and was not deleted.";
			return false;
		}
	}
	if (!DeleteService(service.value)) {
		const DWORD code = GetLastError();
		if (code != ERROR_SERVICE_MARKED_FOR_DELETE) {
			error = L"Could not delete the validated legacy service: "
				+ WindowsErrorMessage(code);
			return false;
		}
	}
	return true;
}

#else

Inspection Inspect(const wstring& serviceName) {
	return {State::Unsupported, serviceName, {}, {}, false,
		L"Legacy Windows service cleanup is unavailable on this platform."};
}

bool RequestElevatedRemoval(const wstring&, wstring& error) {
	error = L"Legacy Windows service cleanup is unavailable on this platform.";
	return false;
}

bool RemoveAfterValidation(const wstring&, wstring& error) {
	error = L"Legacy Windows service cleanup is unavailable on this platform.";
	return false;
}

#endif

} // namespace LegacyServiceCleanup
