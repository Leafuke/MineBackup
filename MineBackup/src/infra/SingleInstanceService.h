#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

inline constexpr std::uint32_t kInstanceMaximumFramePayloadBytes =
	8u * 1024u * 1024u;

enum class InstanceRequestType {
	Activate,
	SelectConfig
};

struct InstanceRequest {
	InstanceRequestType type = InstanceRequestType::Activate;
	std::wstring stableId;
};

enum class InstanceRuntimeRole {
	Unknown,
	Desktop,
	Serve
};

enum class InstanceControlRequestType {
	Probe,
	Execute,
	Status,
	Cancel,
	Stop
};

struct InstanceControlRequest {
	std::string requestId;
	InstanceControlRequestType type = InstanceControlRequestType::Probe;
	std::vector<std::wstring> arguments;
	std::string operationId;
};

struct InstanceControlResponse {
	std::string requestId;
	bool accepted = false;
	InstanceRuntimeRole role = InstanceRuntimeRole::Unknown;
	std::vector<std::string> capabilities;
	std::string operationId;
	int exitCode = 5;
	std::string payload;
	std::string error;
};

struct InstanceControlExchange {
	std::uint64_t connectionId = 0;
	InstanceControlRequest request;
};

enum class InstanceAcquireResult {
	Acquired,
	AlreadyRunning,
	Failed
};

class SingleInstanceService {
public:
	SingleInstanceService();
	~SingleInstanceService();
	SingleInstanceService(const SingleInstanceService&) = delete;
	SingleInstanceService& operator=(const SingleInstanceService&) = delete;

	InstanceAcquireResult Acquire(
		const std::wstring& profileIdentity,
		const std::filesystem::path& runtimeRoot,
		std::wstring& error);

	// Protocol v1 compatibility for desktop activation only.
	bool Send(const InstanceRequest& request, std::wstring& error) const;
	std::vector<InstanceRequest> PollRequests(std::wstring& error);

	// Protocol v2 request/response channel used by the headless runtime.
	bool Exchange(
		const InstanceControlRequest& request,
		InstanceControlResponse& response,
		std::wstring& error,
		std::chrono::milliseconds timeout = std::chrono::minutes(30)) const;
	std::vector<InstanceControlExchange> PollControlRequests(std::wstring& error);
	bool Reply(
		std::uint64_t connectionId,
		const InstanceControlResponse& response,
		std::wstring& error);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
