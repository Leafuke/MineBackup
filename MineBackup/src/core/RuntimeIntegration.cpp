#include "RuntimeIntegration.h"

#include "KnotLinkProtocol.h"
#include "MineBackupVersion.h"
#include "RuntimeFileLock.h"
#include "text_to_text.h"
#include "knotlink/OpenSocketResponser.hpp"
#include "knotlink/SignalSender.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

using namespace std;

CloudPostResult NoopCloudPostHook::Run(
	const BackupRequest&,
	const HistoryEntry&,
	stop_token) {
	return {};
}

CloudPostResult NetworkDisabledCloudPostHook::Run(
	const BackupRequest& request,
	const HistoryEntry&,
	stop_token) {
	CloudPostResult result;
	if (request.config.cloudSyncEnabled) {
		result.diagnostics.push_back({
			"cloud.network_disabled", DiagnosticSeverity::Info,
			"Cloud post-processing was skipped because network access is disabled."});
	}
	return result;
}

CallbackCloudPostHook::CallbackCloudPostHook(Callback callback)
	: callback_(std::move(callback)) {
}

CloudPostResult CallbackCloudPostHook::Run(
	const BackupRequest& request,
	const HistoryEntry& historyEntry,
	stop_token stopToken) {
	return callback_ ? callback_(request, historyEntry, stopToken) : CloudPostResult{};
}

HotBackupPreparation NoKnotLinkBridge::Prepare(
	const BackupRequest&,
	stop_token stopToken) {
	HotBackupPreparation result;
	result.status = stopToken.stop_requested()
		? HotBackupStatus::Rejected
		: HotBackupStatus::Degraded;
	result.diagnostics.push_back({
		stopToken.stop_requested() ? "backup.cancelled" : "knotlink.unavailable",
		stopToken.stop_requested() ? DiagnosticSeverity::Warning : DiagnosticSeverity::Warning,
		stopToken.stop_requested()
			? "Cancellation was requested before hot-backup coordination."
			: "KnotLink is unavailable; the archive runner will use its live-file fallback."});
	return result;
}

HotBackupPreparation NetworkDisabledKnotLinkBridge::Prepare(
	const BackupRequest&,
	stop_token stopToken) {
	HotBackupPreparation result;
	result.status = stopToken.stop_requested()
		? HotBackupStatus::Rejected
		: HotBackupStatus::Degraded;
	result.diagnostics.push_back({
		stopToken.stop_requested() ? "backup.cancelled" : "knotlink.network_disabled",
		DiagnosticSeverity::Warning,
		stopToken.stop_requested()
			? "Cancellation was requested before hot-backup coordination."
			: "KnotLink coordination was skipped because network access is disabled."});
	return result;
}

CallbackHotBackupBridge::CallbackHotBackupBridge(Callback callback)
	: callback_(std::move(callback)) {
}

HotBackupPreparation CallbackHotBackupBridge::Prepare(
	const BackupRequest& request,
	stop_token stopToken) {
	return callback_ ? callback_(request, stopToken) : HotBackupPreparation{};
}

struct HeadlessKnotLinkBridge::Implementation {
	mutable mutex lifecycleMutex;
	mutex stateMutex;
	mutex handlerMutex;
	condition_variable stateChanged;
	unique_ptr<::knotlink::SignalSender> sender;
	unique_ptr<::knotlink::OpenSocketResponser> responder;
	minebackup::knotlink::KnotLinkCommandDispatcher dispatcher;
	minebackup::knotlink::KnotLinkCommandDispatcher::Handler commandHandler;
	bool running = false;
	bool handshakeReceived = false;
	bool versionCompatible = false;
	bool worldSaved = false;
	bool worldSaveAndExitComplete = false;
	bool rejoinResponseReceived = false;
	bool rejoinSuccess = false;
	string modVersion;

	string HandleCommand(
		const shared_ptr<minebackup::knotlink::KnotLinkCommandContext>& context) {
		using namespace minebackup::knotlink;
		const auto& request = context->request;
		if (request.command == "HANDSHAKE_RESPONSE") {
			const string version = request.Get("mod_version");
			if (version.empty()) {
				return KnotLinkProtocolFormatter::FormatError(
					context.get(), "Missing mod_version.");
			}
			bool compatible = false;
			{
				lock_guard lock(stateMutex);
				modVersion = version;
				compatible = KnotLinkModInfo::IsVersionCompatible(
					version, string(KnotLinkCapabilities::MinimumModVersion));
				versionCompatible = compatible;
				handshakeReceived = true;
			}
			stateChanged.notify_all();
			return KnotLinkProtocolFormatter::FormatOk(*context, {
				{"compatible", compatible ? "true" : "false"},
				{"minimum_mod_version", string(KnotLinkCapabilities::MinimumModVersion)}});
		}
		if (request.command == "WORLD_SAVED") {
			{
				lock_guard lock(stateMutex);
				worldSaved = true;
			}
			stateChanged.notify_all();
			return KnotLinkProtocolFormatter::FormatOk(
				*context, {{"message", "World save acknowledged."}});
		}
		if (request.command == "WORLD_SAVE_AND_EXIT_COMPLETE") {
			{
				lock_guard lock(stateMutex);
				worldSaveAndExitComplete = true;
			}
			stateChanged.notify_all();
			return KnotLinkProtocolFormatter::FormatOk(
				*context, {{"message", "World save-and-exit acknowledged."}});
		}
		if (request.command == "REJOIN_RESULT") {
			string value = request.Get("result");
			transform(value.begin(), value.end(), value.begin(),
				[](unsigned char character) {
					return static_cast<char>(tolower(character));
				});
			if (value != "success" && value != "failure") {
				return KnotLinkProtocolFormatter::FormatError(
					context.get(), "result must be success or failure.");
			}
			{
				lock_guard lock(stateMutex);
				rejoinSuccess = value == "success";
				rejoinResponseReceived = true;
			}
			stateChanged.notify_all();
			return KnotLinkProtocolFormatter::FormatOk(
				*context, {{"message", "Rejoin result acknowledged."}});
		}
		KnotLinkCommandDispatcher::Handler handler;
		{
			lock_guard lock(handlerMutex);
			handler = commandHandler;
		}
		if (handler) return handler(context);
		return KnotLinkProtocolFormatter::FormatError(
			context.get(), "The headless runtime does not expose command execution.");
	}

	string HandlePayload(const string& payload) { return dispatcher.Dispatch(payload); }

	bool Emit(
		string_view eventId,
		const vector<pair<string, string>>& fields) {
		lock_guard lock(lifecycleMutex);
		if (!sender) return false;
		return sender->emitt(
			minebackup::knotlink::KnotLinkProtocolFormatter::FormatEvent(
				nullptr, eventId, fields));
	}

	bool WaitFor(bool& flag, chrono::milliseconds timeout, stop_token stopToken) {
		unique_lock lock(stateMutex);
		stop_callback cancellation(stopToken, [this] { stateChanged.notify_all(); });
		const auto deadline = chrono::steady_clock::now() + timeout;
		while (!flag && !stopToken.stop_requested()) {
			if (stateChanged.wait_until(lock, deadline) == cv_status::timeout) break;
		}
		return flag && !stopToken.stop_requested();
	}
};

HeadlessKnotLinkBridge::HeadlessKnotLinkBridge()
	: implementation_(make_unique<Implementation>()) {
	implementation_->dispatcher.SetHandler(
		[implementation = implementation_.get()](const auto& context) {
			return implementation->HandleCommand(context);
		});
}

HeadlessKnotLinkBridge::~HeadlessKnotLinkBridge() {
	Stop();
}

bool HeadlessKnotLinkBridge::Start() {
	lock_guard lock(implementation_->lifecycleMutex);
	if (implementation_->running) return true;
	try {
		implementation_->sender = make_unique<::knotlink::SignalSender>(
			string(minebackup::knotlink::KnotLinkCapabilities::AppId),
			string(minebackup::knotlink::KnotLinkCapabilities::SignalId));
		implementation_->responder = make_unique<::knotlink::OpenSocketResponser>(
			string(minebackup::knotlink::KnotLinkCapabilities::AppId),
			string(minebackup::knotlink::KnotLinkCapabilities::OpenSocketId));
		implementation_->responder->setQuestionHandler(
			[this](const string& payload) {
				return implementation_->HandlePayload(payload);
			});
		implementation_->running = true;
		return true;
	}
	catch (...) {
		implementation_->responder.reset();
		implementation_->sender.reset();
		implementation_->running = false;
		return false;
	}
}

void HeadlessKnotLinkBridge::Stop() {
	unique_ptr<::knotlink::OpenSocketResponser> responder;
	unique_ptr<::knotlink::SignalSender> sender;
	{
		lock_guard lock(implementation_->lifecycleMutex);
		implementation_->running = false;
		responder = std::move(implementation_->responder);
		sender = std::move(implementation_->sender);
	}
	responder.reset();
	sender.reset();
}

bool HeadlessKnotLinkBridge::IsRunning() const noexcept {
	lock_guard lock(implementation_->lifecycleMutex);
	return implementation_->running;
}

void HeadlessKnotLinkBridge::SetCommandHandler(
	minebackup::knotlink::KnotLinkCommandDispatcher::Handler handler) {
	lock_guard lock(implementation_->handlerMutex);
	implementation_->commandHandler = std::move(handler);
}

HotRestoreResult HeadlessKnotLinkBridge::CoordinateRestore(
	const HotRestoreRequest& request,
	function<RestoreResult(stop_token)> executeRestore,
	stop_token stopToken,
	const HotRestoreTimeouts& timeouts) {
	HotRestoreDependencies dependencies;
	dependencies.transport.reset = [implementation = implementation_.get()] {
		lock_guard lock(implementation->stateMutex);
		implementation->handshakeReceived = false;
		implementation->versionCompatible = false;
		implementation->worldSaved = false;
		implementation->worldSaveAndExitComplete = false;
		implementation->rejoinResponseReceived = false;
		implementation->rejoinSuccess = false;
		implementation->modVersion.clear();
	};
	dependencies.transport.emit = [implementation = implementation_.get()](
		string_view eventName,
		const vector<pair<string, string>>& fields) {
		return implementation->Emit(eventName, fields);
	};
	dependencies.transport.waitHandshake = [implementation = implementation_.get()](
		chrono::milliseconds timeout,
		stop_token token) {
		if (!implementation->WaitFor(
				implementation->handshakeReceived, timeout, token)) {
			return token.stop_requested()
				? HotRestoreHandshakeStatus::Cancelled
				: HotRestoreHandshakeStatus::TimedOut;
		}
		lock_guard lock(implementation->stateMutex);
		return implementation->versionCompatible
			? HotRestoreHandshakeStatus::Compatible
			: HotRestoreHandshakeStatus::Incompatible;
	};
	dependencies.transport.waitSaveAndExit = [implementation = implementation_.get()](
		chrono::milliseconds timeout,
		stop_token token) {
		return implementation->WaitFor(
			implementation->worldSaveAndExitComplete, timeout, token);
	};
	dependencies.transport.waitRejoin = [implementation = implementation_.get()](
		chrono::milliseconds timeout,
		stop_token token) -> optional<bool> {
		if (!implementation->WaitFor(
				implementation->rejoinResponseReceived, timeout, token)) return nullopt;
		lock_guard lock(implementation->stateMutex);
		return implementation->rejoinSuccess;
	};
	dependencies.isWorldOccupied = IsRuntimeWorldOccupied;
	dependencies.executeRestore = std::move(executeRestore);
	return HotRestoreCoordinator(std::move(dependencies)).Run(
		request, stopToken, timeouts);
}

HotBackupPreparation HeadlessKnotLinkBridge::Prepare(
	const BackupRequest& request,
	stop_token stopToken) {
	HotBackupPreparation result;
	if (stopToken.stop_requested()) {
		result.status = HotBackupStatus::Rejected;
		result.diagnostics.push_back({
			"backup.cancelled", DiagnosticSeverity::Warning, {}});
		return result;
	}
	if (!IsRunning() && !Start()) {
		result.status = HotBackupStatus::Degraded;
		result.diagnostics.push_back({
			"knotlink.listener.unavailable", DiagnosticSeverity::Warning,
			"The local KnotLink ports are unavailable; using the live-file fallback."});
		return result;
	}
	{
		lock_guard lock(implementation_->stateMutex);
		implementation_->handshakeReceived = false;
		implementation_->versionCompatible = false;
		implementation_->worldSaved = false;
		implementation_->modVersion.clear();
	}
	if (!implementation_->Emit("handshake", {
			{"version", MINEBACKUP_VERSION_STRING},
			{"action", "backup"},
			{"world", wstring_to_utf8(request.world.relativePath)},
			{"min_mod_version", string(
				minebackup::knotlink::KnotLinkCapabilities::MinimumModVersion)}})
		|| !implementation_->WaitFor(
			implementation_->handshakeReceived, chrono::seconds(3), stopToken)) {
		result.status = stopToken.stop_requested()
			? HotBackupStatus::Rejected
			: HotBackupStatus::Degraded;
		result.diagnostics.push_back({
			stopToken.stop_requested() ? "backup.cancelled" : "knotlink.handshake.unavailable",
			DiagnosticSeverity::Warning,
			"The KnotLink handshake did not complete; using the live-file fallback."});
		return result;
	}
	{
		lock_guard lock(implementation_->stateMutex);
		if (!implementation_->versionCompatible) {
			result.status = HotBackupStatus::Degraded;
			result.diagnostics.push_back({
				"knotlink.version.incompatible", DiagnosticSeverity::Warning,
				implementation_->modVersion});
			return result;
		}
	}
	this_thread::sleep_for(chrono::milliseconds(100));
	if (!implementation_->Emit("pre_hot_backup", {
			{"config", wstring_to_utf8(request.config.configId)},
			{"config_id", wstring_to_utf8(request.config.configId)},
			{"folder", wstring_to_utf8(request.world.relativePath)},
			{"world", wstring_to_utf8(request.world.relativePath)}})
		|| !implementation_->WaitFor(
			implementation_->worldSaved, chrono::seconds(10), stopToken)) {
		result.status = stopToken.stop_requested()
			? HotBackupStatus::Rejected : HotBackupStatus::Degraded;
		result.diagnostics.push_back({
			stopToken.stop_requested() ? "backup.cancelled" : "knotlink.world_save.timeout",
			DiagnosticSeverity::Warning,
			stopToken.stop_requested()
				? "The coordinated world save was cancelled."
				: "The coordinated world save did not complete; using the 7-Zip -ssw fallback."});
		return result;
	}
	result.status = HotBackupStatus::Coordinated;
	result.diagnostics.push_back({
		"knotlink.world_save.completed", DiagnosticSeverity::Info, {}});
	return result;
}

void HeadlessKnotLinkBridge::Publish(const BackupRuntimeEvent& event) {
	if (!IsRunning()) return;
	(void)implementation_->Emit(event.eventId, event.fields);
}

void NoopRuntimeEventSink::Publish(const BackupRuntimeEvent&) {
}

CallbackRuntimeEventSink::CallbackRuntimeEventSink(Callback callback)
	: callback_(std::move(callback)) {
}

void CallbackRuntimeEventSink::Publish(const BackupRuntimeEvent& event) {
	if (callback_) callback_(event);
}
