#pragma once

#include <chrono>
#include <functional>

enum class SettingsSaveState {
	Idle,
	Pending,
	Saving,
	Saved,
	Failed
};

class SettingsAutoSaveController {
public:
	using Clock = std::chrono::steady_clock;
	using SaveCallback = std::function<bool()>;

	void MarkDirty(Clock::time_point now = Clock::now()) {
		dirty_ = true;
		lastEdit_ = now;
		state_ = SettingsSaveState::Pending;
	}

	void Tick(const SaveCallback& save, Clock::time_point now = Clock::now()) {
		if (!dirty_ || now - lastEdit_ < debounce_) return;
		SaveNow(save);
	}

	bool Flush(const SaveCallback& save) {
		return !dirty_ || SaveNow(save);
	}

	bool Retry(const SaveCallback& save) {
		return SaveNow(save);
	}

	SettingsSaveState State() const { return state_; }
	bool IsDirty() const { return dirty_; }

private:
	bool SaveNow(const SaveCallback& save) {
		state_ = SettingsSaveState::Saving;
		const bool saved = save && save();
		dirty_ = !saved;
		state_ = saved ? SettingsSaveState::Saved : SettingsSaveState::Failed;
		return saved;
	}

	static constexpr std::chrono::milliseconds debounce_{500};
	Clock::time_point lastEdit_ = Clock::now();
	SettingsSaveState state_ = SettingsSaveState::Idle;
	bool dirty_ = false;
};
