#include "RuntimeIntegration.h"

#include "KnotLinkProtocol.h"
#include "MineBackupVersion.h"
#include "text_to_text.h"
#include "knotlink/OpenSocketResponser.hpp"
#include "knotlink/SignalSender.hpp"

#include <chrono>
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
	condition_variable stateChanged;
	unique_ptr<::knotlink::SignalSender> sender;
	unique_ptr<::knotlink::OpenSocketResponser> responder;
	bool running = false;
	bool handshakeReceived = false;
	bool versionCompatible = false;
	bool worldSaved = false;
	string modVersion;

	string HandlePayload(const string& payload) {
		using namespace minebackup::knotlink;
		try {
			auto context = make_shared<KnotLinkCommandContext>(
				KnotLinkCommandRequest::Parse(payload));
			const auto& request = context->request;
			if (request.command == "HANDSHAKE_RESPONSE") {
				const string version = request.Get("mod_version");
				if (version.empty()) {
					return KnotLinkProtocolFormatter::FormatError(
						context.get(), "Missing mod_version.");
				}
				{
					lock_guard lock(stateMutex);
					modVersion = version;
					versionCompatible = KnotLinkModInfo::IsVersionCompatible(
						version, KnotLinkModInfo::MIN_MOD_VERSION);
					handshakeReceived = true;
				}
				stateChanged.notify_all();
				return KnotLinkProtocolFormatter::FormatOk(*context, {
					{"compatible", versionCompatible ? "true" : "false"},
					{"minimum_mod_version", KnotLinkModInfo::MIN_MOD_VERSION}});
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
			return KnotLinkProtocolFormatter::FormatError(
				context.get(), "The headless bridge accepts only hot-backup responses.");
		}
		catch (const exception& error) {
			return minebackup::knotlink::KnotLinkProtocolFormatter::FormatError(
				nullptr, error.what());
		}
	}

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
		const auto deadline = chrono::steady_clock::now() + timeout;
		while (!flag && !stopToken.stop_requested()) {
			if (stateChanged.wait_until(lock, deadline) == cv_status::timeout) break;
		}
		return flag && !stopToken.stop_requested();
	}
};

HeadlessKnotLinkBridge::HeadlessKnotLinkBridge()
	: implementation_(make_unique<Implementation>()) {
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
			{"min_mod_version", KnotLinkModInfo::MIN_MOD_VERSION}})
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
			{"config_id", wstring_to_utf8(request.config.configId)},
			{"world", wstring_to_utf8(request.world.relativePath)}})
		|| !implementation_->WaitFor(
			implementation_->worldSaved, chrono::seconds(10), stopToken)) {
		result.status = HotBackupStatus::Rejected;
		result.diagnostics.push_back({
			stopToken.stop_requested() ? "backup.cancelled" : "knotlink.world_save.timeout",
			stopToken.stop_requested() ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
			"The coordinated world save did not complete."});
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
