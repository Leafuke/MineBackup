#pragma once

#include <chrono>

enum class DesktopUiState {
	Visible,
	HiddenWarm,
	HiddenCold
};

enum class DesktopUiAction {
	None,
	HideWarm,
	ShowExisting,
	UnloadSession,
	CreateAndShow
};

class DesktopUiLifecycle {
public:
	using Clock = std::chrono::steady_clock;
	static constexpr auto ColdDelay = std::chrono::seconds(10);

	explicit DesktopUiLifecycle(DesktopUiState initialState = DesktopUiState::Visible)
		: state_(initialState) {}

	DesktopUiAction HideToTray(Clock::time_point now) noexcept;
	DesktopUiAction RequestShow() noexcept;
	DesktopUiAction Tick(Clock::time_point now) noexcept;
	DesktopUiAction RequestExit() const noexcept;
	void CompleteColdRestore(bool succeeded) noexcept;

	[[nodiscard]] DesktopUiState State() const noexcept { return state_; }
	[[nodiscard]] bool HasLiveSession() const noexcept {
		return state_ != DesktopUiState::HiddenCold;
	}

private:
	DesktopUiState state_ = DesktopUiState::Visible;
	Clock::time_point hiddenSince_{};
	bool restorePending_ = false;
};
