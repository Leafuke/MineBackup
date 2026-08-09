#pragma once

#include "BackupService.h"
#include "HotRestoreCoordinator.h"
#include "KnotLinkCommandDispatcher.h"

#include <functional>
#include <memory>
#include <stop_token>

class ICloudPostHook {
public:
	virtual ~ICloudPostHook() = default;
	virtual CloudPostResult Run(
		const BackupRequest& request,
		const HistoryEntry& historyEntry,
		std::stop_token stopToken) = 0;
};

class IHotBackupBridge {
public:
	virtual ~IHotBackupBridge() = default;
	virtual HotBackupPreparation Prepare(
		const BackupRequest& request,
		std::stop_token stopToken) = 0;
};

class IRuntimeEventSink {
public:
	virtual ~IRuntimeEventSink() = default;
	virtual void Publish(const BackupRuntimeEvent& event) = 0;
};

class NoopCloudPostHook final : public ICloudPostHook {
public:
	CloudPostResult Run(
		const BackupRequest& request,
		const HistoryEntry& historyEntry,
		std::stop_token stopToken) override;
};

class NetworkDisabledCloudPostHook final : public ICloudPostHook {
public:
	CloudPostResult Run(
		const BackupRequest& request,
		const HistoryEntry& historyEntry,
		std::stop_token stopToken) override;
};

class CallbackCloudPostHook final : public ICloudPostHook {
public:
	using Callback = std::function<CloudPostResult(
		const BackupRequest&,
		const HistoryEntry&,
		std::stop_token)>;

	explicit CallbackCloudPostHook(Callback callback);
	CloudPostResult Run(
		const BackupRequest& request,
		const HistoryEntry& historyEntry,
		std::stop_token stopToken) override;

private:
	Callback callback_;
};

class NoKnotLinkBridge final : public IHotBackupBridge {
public:
	HotBackupPreparation Prepare(
		const BackupRequest& request,
		std::stop_token stopToken) override;
};

class NetworkDisabledKnotLinkBridge final : public IHotBackupBridge {
public:
	HotBackupPreparation Prepare(
		const BackupRequest& request,
		std::stop_token stopToken) override;
};

class CallbackHotBackupBridge final : public IHotBackupBridge {
public:
	using Callback = std::function<HotBackupPreparation(
		const BackupRequest&,
		std::stop_token)>;

	explicit CallbackHotBackupBridge(Callback callback);
	HotBackupPreparation Prepare(
		const BackupRequest& request,
		std::stop_token stopToken) override;

private:
	Callback callback_;
};

class HeadlessKnotLinkBridge final
	: public IHotBackupBridge,
	  public IRuntimeEventSink {
public:
	HeadlessKnotLinkBridge();
	~HeadlessKnotLinkBridge() override;

	HeadlessKnotLinkBridge(const HeadlessKnotLinkBridge&) = delete;
	HeadlessKnotLinkBridge& operator=(const HeadlessKnotLinkBridge&) = delete;

	bool Start();
	void Stop();
	bool IsRunning() const noexcept;
	void SetCommandHandler(
		minebackup::knotlink::KnotLinkCommandDispatcher::Handler handler);
	HotRestoreResult CoordinateRestore(
		const HotRestoreRequest& request,
		std::function<RestoreResult(std::stop_token)> executeRestore,
		std::stop_token stopToken = {},
		const HotRestoreTimeouts& timeouts = {});
	HotBackupPreparation Prepare(
		const BackupRequest& request,
		std::stop_token stopToken) override;
	void Publish(const BackupRuntimeEvent& event) override;

private:
	struct Implementation;
	std::unique_ptr<Implementation> implementation_;
};

class NoopRuntimeEventSink final : public IRuntimeEventSink {
public:
	void Publish(const BackupRuntimeEvent& event) override;
};

class CallbackRuntimeEventSink final : public IRuntimeEventSink {
public:
	using Callback = std::function<void(const BackupRuntimeEvent&)>;

	explicit CallbackRuntimeEventSink(Callback callback);
	void Publish(const BackupRuntimeEvent& event) override;

private:
	Callback callback_;
};
