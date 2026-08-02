#include "DesktopUiLifecycle.h"

DesktopUiAction DesktopUiLifecycle::HideToTray(Clock::time_point now) noexcept {
	if (state_ != DesktopUiState::Visible) return DesktopUiAction::None;
	state_ = DesktopUiState::HiddenWarm;
	hiddenSince_ = now;
	return DesktopUiAction::HideWarm;
}

DesktopUiAction DesktopUiLifecycle::RequestShow() noexcept {
	if (state_ == DesktopUiState::Visible) return DesktopUiAction::None;
	if (state_ == DesktopUiState::HiddenWarm) {
		state_ = DesktopUiState::Visible;
		return DesktopUiAction::ShowExisting;
	}
	if (restorePending_) return DesktopUiAction::None;
	restorePending_ = true;
	return DesktopUiAction::CreateAndShow;
}

DesktopUiAction DesktopUiLifecycle::Tick(Clock::time_point now) noexcept {
	if (state_ != DesktopUiState::HiddenWarm
		|| now - hiddenSince_ < ColdDelay) {
		return DesktopUiAction::None;
	}
	state_ = DesktopUiState::HiddenCold;
	return DesktopUiAction::UnloadSession;
}

DesktopUiAction DesktopUiLifecycle::RequestExit() const noexcept {
	return HasLiveSession() ? DesktopUiAction::UnloadSession : DesktopUiAction::None;
}

void DesktopUiLifecycle::CompleteColdRestore(bool succeeded) noexcept {
	if (!restorePending_) return;
	restorePending_ = false;
	if (succeeded) state_ = DesktopUiState::Visible;
}
