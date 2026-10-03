#pragma once

#include "OperationResult.h"
#include "RestoreService.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class HotRestoreHandshakeStatus {
	Compatible,
	Incompatible,
	TimedOut,
	Cancelled
};

enum class HotRestoreRejoinStatus {
	NotRequested,
	Succeeded,
	Failed,
	TimedOut,
	Cancelled
};

struct HotRestoreRequest {
	std::wstring configId;
	std::wstring worldPath;
	std::filesystem::path fullWorldPath;
	std::string requestId;
	bool handshakeComplete = false;
};

struct HotRestoreTimeouts {
	std::chrono::milliseconds handshake{3000};
	std::chrono::milliseconds postHandshake{100};
	std::chrono::milliseconds saveAndExit{10000};
	std::chrono::milliseconds worldRelease{15000};
	std::chrono::milliseconds restoreFinishedDelay{100};
	std::chrono::milliseconds postRestoreStabilize{3000};
	std::chrono::milliseconds rejoin{30000};
	std::chrono::milliseconds releasePoll{100};
};

struct HotRestoreTransport {
	std::function<void()> reset;
	std::function<bool(std::string_view,
		const std::vector<std::pair<std::string, std::string>>&)> emit;
	std::function<HotRestoreHandshakeStatus(
		std::chrono::milliseconds, std::stop_token)> waitHandshake;
	std::function<bool(std::chrono::milliseconds, std::stop_token)> waitSaveAndExit;
	std::function<std::optional<bool>(
		std::chrono::milliseconds, std::stop_token)> waitRejoin;
};

struct HotRestoreDependencies {
	HotRestoreTransport transport;
	std::function<bool(const std::filesystem::path&)> isWorldOccupied;
	std::function<RestoreResult(std::stop_token)> executeRestore;
};

struct HotRestoreResult {
	OperationCode code = OperationCode::RestoreFailed;
	RestoreResult restore;
	HotRestoreHandshakeStatus handshake = HotRestoreHandshakeStatus::TimedOut;
	HotRestoreRejoinStatus rejoin = HotRestoreRejoinStatus::NotRequested;
	bool saveAndExitCompleted = false;
	bool worldReleased = false;
	std::vector<Diagnostic> diagnostics;
};

class HotRestoreCoordinator {
public:
	explicit HotRestoreCoordinator(HotRestoreDependencies dependencies);
	HotRestoreResult Run(
		const HotRestoreRequest& request,
		std::stop_token stopToken = {},
		const HotRestoreTimeouts& timeouts = {}) const;

private:
	HotRestoreDependencies dependencies_;
};
