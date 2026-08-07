#pragma once

#include <filesystem>
#include <string>

namespace LegacyServiceCleanup {

enum class State {
	Unsupported,
	NotInstalled,
	Removable,
	Unsafe,
	QueryFailed
};

struct Inspection {
	State state = State::Unsupported;
	std::wstring serviceName;
	std::wstring imagePath;
	std::filesystem::path executable;
	bool running = false;
	std::wstring diagnostic;

	[[nodiscard]] bool CanRemove() const noexcept {
		return state == State::Removable;
	}
};

[[nodiscard]] Inspection Inspect(const std::wstring& serviceName);
bool RequestElevatedRemoval(const std::wstring& serviceName, std::wstring& error);
bool RemoveAfterValidation(const std::wstring& serviceName, std::wstring& error);

} // namespace LegacyServiceCleanup
