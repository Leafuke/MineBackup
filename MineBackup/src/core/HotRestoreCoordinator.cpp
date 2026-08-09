#include "HotRestoreCoordinator.h"

#include "MineBackupVersion.h"
#include "KnotLinkProtocol.h"
#include "text_to_text.h"

#include <thread>
#include <utility>

using namespace std;

namespace {

Diagnostic Failure(string eventId, string detail = {}) {
	return {std::move(eventId), DiagnosticSeverity::Error, std::move(detail)};
}

Diagnostic Warning(string eventId, string detail = {}) {
	return {std::move(eventId), DiagnosticSeverity::Warning, std::move(detail)};
}

vector<pair<string, string>> TargetFields(const HotRestoreRequest& request) {
	return {
		{"config_id", wstring_to_utf8(request.configId)},
		{"world", wstring_to_utf8(request.worldPath)}};
}

} // namespace

HotRestoreCoordinator::HotRestoreCoordinator(HotRestoreDependencies dependencies)
	: dependencies_(std::move(dependencies)) {
}

HotRestoreResult HotRestoreCoordinator::Run(
	const HotRestoreRequest& request,
	stop_token stopToken,
	const HotRestoreTimeouts& timeouts) const {
	HotRestoreResult result;
	auto emit = [&](string_view eventName,
		vector<pair<string, string>> fields = {}) {
		if (!dependencies_.transport.emit) return false;
		if (!request.requestId.empty()) fields.emplace_back("request_id", request.requestId);
		return dependencies_.transport.emit(eventName, fields);
	};
	auto cancelled = [&] {
		result.code = OperationCode::Cancelled;
		result.diagnostics.push_back(Warning("restore.cancelled"));
		(void)emit("restore_cancelled", {{"reason", "cancelled"}});
		return result;
	};
	if (stopToken.stop_requested()) return cancelled();
	if (!dependencies_.transport.reset || !dependencies_.transport.emit
		|| !dependencies_.transport.waitHandshake
		|| !dependencies_.transport.waitSaveAndExit
		|| !dependencies_.transport.waitRejoin
		|| !dependencies_.isWorldOccupied || !dependencies_.executeRestore) {
		result.diagnostics.push_back(Failure("restore.hot.runtime_missing"));
		return result;
	}

	dependencies_.transport.reset();
	if (request.handshakeComplete) {
		result.handshake = HotRestoreHandshakeStatus::Compatible;
	}
	else {
		auto handshakeFields = TargetFields(request);
		if (!request.requestId.empty()) {
			handshakeFields.emplace_back("request_id", request.requestId);
		}
		handshakeFields.emplace_back("version", MINEBACKUP_VERSION_STRING);
		handshakeFields.emplace_back("action", "restore");
		handshakeFields.emplace_back("min_mod_version",
			string(minebackup::knotlink::KnotLinkCapabilities::MinimumModVersion));
		if (!dependencies_.transport.emit("handshake", handshakeFields)) {
			result.diagnostics.push_back(Failure("restore.hot.handshake_emit_failed"));
			return result;
		}
		result.handshake = dependencies_.transport.waitHandshake(
			timeouts.handshake, stopToken);
	}
	if (stopToken.stop_requested()
		|| result.handshake == HotRestoreHandshakeStatus::Cancelled) return cancelled();
	if (result.handshake == HotRestoreHandshakeStatus::Incompatible) {
		result.diagnostics.push_back(Failure("restore.hot.mod_incompatible"));
		return result;
	}
	if (result.handshake != HotRestoreHandshakeStatus::Compatible) {
		result.diagnostics.push_back(Failure("restore.hot.handshake_timeout"));
		return result;
	}

	if (!emit("pre_hot_restore", TargetFields(request))) {
		result.diagnostics.push_back(Failure("restore.hot.save_exit_emit_failed"));
		return result;
	}
	result.saveAndExitCompleted = dependencies_.transport.waitSaveAndExit(
		timeouts.saveAndExit, stopToken);
	if (stopToken.stop_requested()) return cancelled();
	if (!result.saveAndExitCompleted) {
		result.diagnostics.push_back(Failure("restore.hot.save_exit_timeout"));
		(void)emit("restore_cancelled", {{"reason", "timeout"}});
		return result;
	}

	const auto releaseDeadline = chrono::steady_clock::now() + timeouts.worldRelease;
	while (!stopToken.stop_requested()
		&& chrono::steady_clock::now() < releaseDeadline) {
		if (!dependencies_.isWorldOccupied(request.fullWorldPath)) {
			result.worldReleased = true;
			break;
		}
		this_thread::sleep_for(timeouts.releasePoll);
	}
	if (stopToken.stop_requested()) return cancelled();
	if (!result.worldReleased) {
		result.diagnostics.push_back(Failure("restore.hot.world_release_timeout"));
		(void)emit("restore_cancelled", {{"reason", "world_occupied"}});
		return result;
	}

	result.restore = dependencies_.executeRestore(stopToken);
	result.diagnostics.insert(result.diagnostics.end(),
		result.restore.diagnostics.begin(), result.restore.diagnostics.end());
	if (!IsSuccessful(result.restore.code)) {
		result.code = result.restore.code == OperationCode::Cancelled
			? OperationCode::Cancelled : OperationCode::RestoreFailed;
		(void)emit("restore_finished", {{"status", "failure"},
			{"reason", ToString(result.restore.code)}});
		return result;
	}
	(void)emit("restore_finished", {{"status", "success"}});
	if (!emit("rejoin_world", {{"world", wstring_to_utf8(request.worldPath)}})) {
		result.code = OperationCode::Success;
		result.rejoin = HotRestoreRejoinStatus::TimedOut;
		result.diagnostics.push_back(Warning("restore.hot.rejoin_emit_failed"));
		return result;
	}
	const auto rejoin = dependencies_.transport.waitRejoin(timeouts.rejoin, stopToken);
	result.code = OperationCode::Success;
	if (stopToken.stop_requested()) {
		result.rejoin = HotRestoreRejoinStatus::Cancelled;
		result.diagnostics.push_back(Warning("restore.hot.rejoin_cancelled"));
		(void)emit("hot_restore_complete", {{"status", "restore_ok_rejoin_cancelled"}});
		return result;
	}
	if (!rejoin.has_value()) {
		result.rejoin = HotRestoreRejoinStatus::TimedOut;
		result.diagnostics.push_back(Warning("restore.hot.rejoin_timeout"));
		(void)emit("hot_restore_complete", {{"status", "restore_ok_rejoin_timeout"}});
	}
	else if (*rejoin) {
		result.rejoin = HotRestoreRejoinStatus::Succeeded;
		(void)emit("hot_restore_complete", {{"status", "full_success"}});
	}
	else {
		result.rejoin = HotRestoreRejoinStatus::Failed;
		result.diagnostics.push_back(Warning("restore.hot.rejoin_failed"));
		(void)emit("hot_restore_complete", {{"status", "restore_ok_rejoin_failed"}});
	}
	return result;
}
