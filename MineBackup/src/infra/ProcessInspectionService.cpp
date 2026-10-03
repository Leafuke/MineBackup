#include "ProcessInspectionService.h"

#include "Logging.h"

#include <algorithm>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

using namespace std;

namespace {

#ifdef _WIN32

class ScopedHandle final {
public:
	explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
	~ScopedHandle() {
		if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
	}

	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;

	ScopedHandle(ScopedHandle&& other) noexcept
		: handle_(exchange(other.handle_, nullptr)) {}
	ScopedHandle& operator=(ScopedHandle&& other) noexcept {
		if (this == &other) return *this;
		if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
		handle_ = exchange(other.handle_, nullptr);
		return *this;
	}

	[[nodiscard]] HANDLE get() const noexcept { return handle_; }
	[[nodiscard]] explicit operator bool() const noexcept {
		return handle_ && handle_ != INVALID_HANDLE_VALUE;
	}

private:
	HANDLE handle_ = nullptr;
};

void LogProcessAccessFailure(const char* eventId, DWORD processId, DWORD error) {
	// 仅记录 PID 和 Win32 错误码；这里不能泄露进程路径或完整进程清单。
	MB_LOG_DEBUG(minebackup::logging::LogCategory::Process, eventId,
		"pid={} win32Error={}", processId, error);
}

bool TryReadExecutableImagePath(HANDLE process, filesystem::path& imagePath,
	DWORD& error) {
	constexpr size_t kInitialBufferSize = 1024;
	constexpr size_t kMaximumBufferSize = 32768;
	vector<wchar_t> buffer(kInitialBufferSize);

	for (;;) {
		DWORD characterCount = static_cast<DWORD>(buffer.size());
		if (QueryFullProcessImageNameW(process, 0, buffer.data(), &characterCount)) {
			if (characterCount == 0) {
				error = ERROR_FILE_NOT_FOUND;
				return false;
			}
			imagePath = filesystem::path(wstring(buffer.data(), characterCount));
			return !imagePath.empty();
		}

		error = GetLastError();
		if (error != ERROR_INSUFFICIENT_BUFFER
			|| buffer.size() >= kMaximumBufferSize) {
			return false;
		}
		const size_t doubled = buffer.size() > kMaximumBufferSize / 2
			? kMaximumBufferSize : buffer.size() * 2;
		buffer.resize(doubled);
	}
}

class WindowsProcessInspectionService final : public IProcessInspectionService {
public:
	vector<RunningProcessInfo> ListRunningProcesses(stop_token stopToken) override {
		vector<RunningProcessInfo> processes;
		if (stopToken.stop_requested()) return processes;

		ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
		if (!snapshot) {
			MB_LOG_DEBUG(minebackup::logging::LogCategory::Process,
				"discovery.process_snapshot_failed", "win32Error={}", GetLastError());
			return processes;
		}

		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		BOOL hasProcess = Process32FirstW(snapshot.get(), &entry);
		if (!hasProcess) {
			const DWORD error = GetLastError();
			if (error != ERROR_NO_MORE_FILES) {
				MB_LOG_DEBUG(minebackup::logging::LogCategory::Process,
					"discovery.process_enumeration_failed", "win32Error={}", error);
			}
			return processes;
		}

		while (hasProcess) {
			if (stopToken.stop_requested()) break;

			const DWORD processId = entry.th32ProcessID;
			// 只申请读取 image path 所需的最小权限，不访问命令行、环境或 token。
			ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
				FALSE, processId));
			if (!process) {
				LogProcessAccessFailure("discovery.process_open_skipped", processId,
					GetLastError());
			}
			else {
				filesystem::path imagePath;
				DWORD error = ERROR_SUCCESS;
				if (!TryReadExecutableImagePath(process.get(), imagePath, error)) {
					LogProcessAccessFailure("discovery.process_image_skipped", processId,
						error);
				}
				else {
					const wstring executableName = imagePath.filename().wstring();
					if (!executableName.empty()) {
						processes.push_back({executableName, std::move(imagePath)});
					}
					else {
						LogProcessAccessFailure("discovery.process_image_skipped", processId,
							ERROR_INVALID_DATA);
					}
				}
			}

			hasProcess = Process32NextW(snapshot.get(), &entry);
			if (!hasProcess) {
				const DWORD error = GetLastError();
				if (error != ERROR_NO_MORE_FILES) {
					MB_LOG_DEBUG(minebackup::logging::LogCategory::Process,
						"discovery.process_enumeration_failed", "win32Error={}", error);
				}
			}
		}
		return processes;
	}
};

#else

class UnavailableProcessInspectionService final : public IProcessInspectionService {
public:
	vector<RunningProcessInfo> ListRunningProcesses(stop_token) override {
		// Linux/macOS 本轮不伪造进程结果；调用方通过平台状态识别 NotApplicable。
		return {};
	}
};

#endif

} // namespace

ProcessInspectionAvailability GetProcessInspectionAvailability() noexcept {
#ifdef _WIN32
	return ProcessInspectionAvailability::Available;
#else
	return ProcessInspectionAvailability::NotApplicable;
#endif
}

shared_ptr<IProcessInspectionService> CreateProcessInspectionService() {
#ifdef _WIN32
	return make_shared<WindowsProcessInspectionService>();
#else
	return make_shared<UnavailableProcessInspectionService>();
#endif
}
