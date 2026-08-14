#include "SingleInstanceService.h"

#include "json.hpp"
#include "text_to_text.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <Aclapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace std;

namespace {

constexpr uint32_t kLegacyProtocolVersion = 1;
constexpr uint32_t kControlProtocolVersion = 2;
constexpr uint32_t kMaximumLegacyPayloadBytes = 64u * 1024u;
constexpr uint32_t kMaximumPayloadBytes = 8u * 1024u * 1024u;

#ifdef _WIN32
class CurrentUserSecurity {
public:
	~CurrentUserSecurity() {
		if (accessControlList_) LocalFree(accessControlList_);
	}

	bool Initialize(wstring& error) {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
			error = L"Could not inspect the current user for instance IPC security.";
			return false;
		}
		DWORD required = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &required);
		vector<uint8_t> tokenUser(required);
		const bool read = required != 0
			&& GetTokenInformation(token, TokenUser,
				tokenUser.data(), required, &required) != FALSE;
		CloseHandle(token);
		if (!read) {
			error = L"Could not resolve the current user for instance IPC security.";
			return false;
		}

		EXPLICIT_ACCESSW access{};
		access.grfAccessPermissions = GENERIC_ALL;
		access.grfAccessMode = SET_ACCESS;
		access.grfInheritance = NO_INHERITANCE;
		access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		access.Trustee.TrusteeType = TRUSTEE_IS_USER;
		access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(
			reinterpret_cast<TOKEN_USER*>(tokenUser.data())->User.Sid);
		if (SetEntriesInAclW(1, &access, nullptr, &accessControlList_) != ERROR_SUCCESS
			|| !InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION)
			|| !SetSecurityDescriptorDacl(
				&descriptor_, TRUE, accessControlList_, FALSE)) {
			error = L"Could not create the current-user ACL for instance IPC.";
			return false;
		}
		attributes_.nLength = sizeof(attributes_);
		attributes_.lpSecurityDescriptor = &descriptor_;
		attributes_.bInheritHandle = FALSE;
		return true;
	}

	SECURITY_ATTRIBUTES* Attributes() { return &attributes_; }

private:
	PACL accessControlList_ = nullptr;
	SECURITY_DESCRIPTOR descriptor_{};
	SECURITY_ATTRIBUTES attributes_{};
};
#endif

wstring EndpointToken(const wstring& identity) {
	uint64_t hash = 1469598103934665603ull;
	const auto* bytes = reinterpret_cast<const unsigned char*>(identity.data());
	for (size_t index = 0; index < identity.size() * sizeof(wchar_t); ++index) {
		hash ^= bytes[index];
		hash *= 1099511628211ull;
	}
	wstringstream output;
	output << hex << setw(16) << setfill(L'0') << hash;
	return output.str();
}

#ifndef _WIN32
filesystem::path UserSocketDirectory(wstring& error) {
	const uid_t userId = ::geteuid();
	const filesystem::path directory = filesystem::path("/tmp")
		/ ("minebackup-" + to_string(static_cast<uintmax_t>(userId)));
	error_code fileError;
	filesystem::create_directory(directory, fileError);
	if (fileError && fileError != errc::file_exists) {
		error = L"Could not create the current-user instance socket directory.";
		return {};
	}

	struct stat status{};
	if (::lstat(directory.c_str(), &status) != 0
		|| !S_ISDIR(status.st_mode)
		|| status.st_uid != userId) {
		error = L"The current-user instance socket directory is not secure.";
		return {};
	}
	if (::chmod(directory.c_str(), S_IRWXU) != 0) {
		error = L"Could not secure the current-user instance socket directory.";
		return {};
	}
	return directory;
}
#endif

string LegacyTypeName(InstanceRequestType type) {
	return type == InstanceRequestType::SelectConfig ? "SelectConfig" : "Activate";
}

string ControlTypeName(InstanceControlRequestType type) {
	switch (type) {
	case InstanceControlRequestType::Probe: return "probe";
	case InstanceControlRequestType::Execute: return "execute";
	case InstanceControlRequestType::Status: return "status";
	case InstanceControlRequestType::Cancel: return "cancel";
	case InstanceControlRequestType::Stop: return "stop";
	}
	return {};
}

string RoleName(InstanceRuntimeRole role) {
	switch (role) {
	case InstanceRuntimeRole::Desktop: return "desktop";
	case InstanceRuntimeRole::Serve: return "serve";
	case InstanceRuntimeRole::Unknown: return "unknown";
	}
	return "unknown";
}

bool ParseControlType(const string& value, InstanceControlRequestType& type) {
	if (value == "probe") type = InstanceControlRequestType::Probe;
	else if (value == "execute") type = InstanceControlRequestType::Execute;
	else if (value == "status") type = InstanceControlRequestType::Status;
	else if (value == "cancel") type = InstanceControlRequestType::Cancel;
	else if (value == "stop") type = InstanceControlRequestType::Stop;
	else return false;
	return true;
}

InstanceRuntimeRole ParseRole(const string& value) {
	if (value == "desktop") return InstanceRuntimeRole::Desktop;
	if (value == "serve") return InstanceRuntimeRole::Serve;
	return InstanceRuntimeRole::Unknown;
}

bool FrameJson(const nlohmann::json& root, vector<uint8_t>& message, wstring& error) {
	const string payload = root.dump();
	if (payload.empty() || payload.size() > kMaximumPayloadBytes) {
		error = L"The instance message exceeds the protocol size limit.";
		return false;
	}
	const uint32_t length = static_cast<uint32_t>(payload.size());
	message.resize(sizeof(length) + payload.size());
	memcpy(message.data(), &length, sizeof(length));
	memcpy(message.data() + sizeof(length), payload.data(), payload.size());
	return true;
}

bool ParseFrame(const vector<uint8_t>& message, nlohmann::json& root, wstring& error) {
	if (message.size() < sizeof(uint32_t)) {
		error = L"The instance message is truncated.";
		return false;
	}
	uint32_t length = 0;
	memcpy(&length, message.data(), sizeof(length));
	if (length == 0 || length > kMaximumPayloadBytes
		|| message.size() != sizeof(length) + length) {
		error = L"The instance message has an invalid length prefix.";
		return false;
	}
	root = nlohmann::json::parse(string(
		reinterpret_cast<const char*>(message.data() + sizeof(length)), length),
		nullptr, false);
	if (root.is_discarded() || !root.is_object()) {
		error = L"The instance message is not valid JSON.";
		return false;
	}
	return true;
}

bool EncodeLegacy(const InstanceRequest& request, vector<uint8_t>& message, wstring& error) {
	const nlohmann::json root{
		{"version", kLegacyProtocolVersion},
		{"type", LegacyTypeName(request.type)},
		{"stableId", wstring_to_utf8(request.stableId)}};
	if (root.dump().size() > kMaximumLegacyPayloadBytes) {
		error = L"The legacy instance request exceeds the protocol size limit.";
		return false;
	}
	return FrameJson(root, message, error);
}

bool DecodeLegacy(const vector<uint8_t>& message, InstanceRequest& request, wstring& error) {
	nlohmann::json root;
	if (!ParseFrame(message, root, error)
		|| root.value("version", 0u) != kLegacyProtocolVersion) {
		error = L"The instance request has an unsupported protocol version.";
		return false;
	}
	const string type = root.value("type", string{});
	if (type == "Activate") request.type = InstanceRequestType::Activate;
	else if (type == "SelectConfig") request.type = InstanceRequestType::SelectConfig;
	else {
		error = L"The instance request type is invalid.";
		return false;
	}
	request.stableId = utf8_to_wstring(root.value("stableId", string{}));
	if (request.type == InstanceRequestType::SelectConfig && request.stableId.empty()) {
		error = L"The instance request requires a stable identifier.";
		return false;
	}
	return true;
}

bool EncodeControlRequest(
	const InstanceControlRequest& request,
	vector<uint8_t>& message,
	wstring& error) {
	nlohmann::json arguments = nlohmann::json::array();
	for (const auto& argument : request.arguments) {
		arguments.push_back(wstring_to_utf8(argument));
	}
	return FrameJson({
		{"version", kControlProtocolVersion},
		{"message", "request"},
		{"requestId", request.requestId},
		{"type", ControlTypeName(request.type)},
		{"arguments", std::move(arguments)},
		{"operationId", request.operationId}}, message, error);
}

bool DecodeControlRequest(
	const vector<uint8_t>& message,
	InstanceControlRequest& request,
	wstring& error) {
	nlohmann::json root;
	if (!ParseFrame(message, root, error)
		|| root.value("version", 0u) != kControlProtocolVersion
		|| root.value("message", string{}) != "request"
		|| !root.contains("arguments") || !root["arguments"].is_array()) {
		error = L"The control request has an unsupported protocol envelope.";
		return false;
	}
	request.requestId = root.value("requestId", string{});
	request.operationId = root.value("operationId", string{});
	if (request.requestId.empty()
		|| !ParseControlType(root.value("type", string{}), request.type)) {
		error = L"The control request identity or type is invalid.";
		return false;
	}
	for (const auto& argument : root["arguments"]) {
		if (!argument.is_string()) {
			error = L"The control request arguments are invalid.";
			return false;
		}
		request.arguments.push_back(utf8_to_wstring(argument.get<string>()));
	}
	if (request.type == InstanceControlRequestType::Cancel
		&& request.operationId.empty()) {
		error = L"A cancel request requires an operation identifier.";
		return false;
	}
	return true;
}

bool EncodeControlResponse(
	const InstanceControlResponse& response,
	vector<uint8_t>& message,
	wstring& error) {
	return FrameJson({
		{"version", kControlProtocolVersion},
		{"message", "response"},
		{"requestId", response.requestId},
		{"accepted", response.accepted},
		{"role", RoleName(response.role)},
		{"capabilities", response.capabilities},
		{"operationId", response.operationId},
		{"exitCode", response.exitCode},
		{"payload", response.payload},
		{"error", response.error}}, message, error);
}

bool DecodeControlResponse(
	const vector<uint8_t>& message,
	InstanceControlResponse& response,
	wstring& error) {
	nlohmann::json root;
	if (!ParseFrame(message, root, error)
		|| root.value("version", 0u) != kControlProtocolVersion
		|| root.value("message", string{}) != "response"
		|| !root.contains("capabilities") || !root["capabilities"].is_array()) {
		error = L"The control response has an unsupported protocol envelope.";
		return false;
	}
	response.requestId = root.value("requestId", string{});
	response.accepted = root.value("accepted", false);
	response.role = ParseRole(root.value("role", string{}));
	response.operationId = root.value("operationId", string{});
	response.exitCode = root.value("exitCode", 5);
	response.payload = root.value("payload", string{});
	response.error = root.value("error", string{});
	for (const auto& capability : root["capabilities"]) {
		if (!capability.is_string()) {
			error = L"The control response capabilities are invalid.";
			return false;
		}
		response.capabilities.push_back(capability.get<string>());
	}
	return !response.requestId.empty();
}

#ifdef _WIN32
bool WriteMessage(HANDLE handle, const vector<uint8_t>& message, wstring& error) {
	DWORD written = 0;
	if (!WriteFile(handle, message.data(), static_cast<DWORD>(message.size()),
			&written, nullptr) || written != message.size()) {
		error = L"Could not write the instance message.";
		return false;
	}
	return true;
}

bool ReadMessage(
	HANDLE handle,
	vector<uint8_t>& message,
	chrono::milliseconds timeout,
	wstring& error) {
	const auto deadline = chrono::steady_clock::now() + timeout;
	DWORD available = 0;
	while (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)
		|| available < sizeof(uint32_t)) {
		const DWORD code = GetLastError();
		if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED) {
			error = L"The instance connection closed before a response was received.";
			return false;
		}
		if (chrono::steady_clock::now() >= deadline) {
			error = L"The instance request timed out.";
			return false;
		}
		this_thread::sleep_for(chrono::milliseconds(5));
	}
	uint32_t length = 0;
	DWORD read = 0;
	const BOOL headerRead = ReadFile(
		handle, &length, sizeof(length), &read, nullptr);
	if ((!headerRead && GetLastError() != ERROR_MORE_DATA)
		|| read != sizeof(length) || length == 0 || length > kMaximumPayloadBytes) {
		error = L"The instance message header is invalid.";
		return false;
	}
	message.resize(sizeof(length) + length);
	memcpy(message.data(), &length, sizeof(length));
	size_t offset = sizeof(length);
	while (offset < message.size()) {
		read = 0;
		const BOOL success = ReadFile(handle, message.data() + offset,
			static_cast<DWORD>(message.size() - offset), &read, nullptr);
		if ((!success && GetLastError() != ERROR_MORE_DATA) || read == 0) {
			error = L"The instance message payload is truncated.";
			return false;
		}
		offset += read;
	}
	return true;
}
#else
bool WaitForDescriptor(int descriptor, short events, chrono::milliseconds timeout) {
	pollfd state{descriptor, events, 0};
	int result = 0;
	do {
		result = ::poll(&state, 1, static_cast<int>(min<long long>(
			timeout.count(), (numeric_limits<int>::max)())));
	} while (result < 0 && errno == EINTR);
	return result > 0 && (state.revents & events) != 0;
}

bool WriteMessage(int descriptor, const vector<uint8_t>& message, wstring& error) {
#ifdef SO_NOSIGPIPE
	const int suppressSignal = 1;
	(void)::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE,
		&suppressSignal, sizeof(suppressSignal));
#endif
	size_t offset = 0;
	while (offset < message.size()) {
#ifdef MSG_NOSIGNAL
		const ssize_t sent = ::send(descriptor, message.data() + offset,
			message.size() - offset, MSG_NOSIGNAL);
#else
		const ssize_t sent = ::write(
			descriptor, message.data() + offset, message.size() - offset);
#endif
		if (sent < 0 && errno == EINTR) continue;
		if (sent <= 0) {
			error = L"Could not write the instance message.";
			return false;
		}
		offset += static_cast<size_t>(sent);
	}
	return true;
}

bool ReadExact(
	int descriptor,
	uint8_t* output,
	size_t size,
	chrono::steady_clock::time_point deadline,
	wstring& error) {
	size_t offset = 0;
	while (offset < size) {
		const auto remaining = chrono::duration_cast<chrono::milliseconds>(
			deadline - chrono::steady_clock::now());
		if (remaining <= chrono::milliseconds::zero()
			|| !WaitForDescriptor(descriptor, POLLIN, remaining)) {
			error = L"The instance request timed out.";
			return false;
		}
		const ssize_t count = ::read(descriptor, output + offset, size - offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) {
			error = L"The instance connection closed before the message completed.";
			return false;
		}
		offset += static_cast<size_t>(count);
	}
	return true;
}

bool ReadMessage(
	int descriptor,
	vector<uint8_t>& message,
	chrono::milliseconds timeout,
	wstring& error) {
	const auto deadline = chrono::steady_clock::now() + timeout;
	uint32_t length = 0;
	if (!ReadExact(descriptor, reinterpret_cast<uint8_t*>(&length),
			sizeof(length), deadline, error)
		|| length == 0 || length > kMaximumPayloadBytes) {
		if (error.empty()) error = L"The instance message header is invalid.";
		return false;
	}
	message.resize(sizeof(length) + length);
	memcpy(message.data(), &length, sizeof(length));
	return ReadExact(descriptor, message.data() + sizeof(length),
		length, deadline, error);
}
#endif

} // namespace

struct SingleInstanceService::Impl {
	wstring token;
	uint64_t nextConnectionId = 1;
#ifdef _WIN32
	HANDLE mutex = nullptr;
	HANDLE pipe = INVALID_HANDLE_VALUE;
	wstring pipeName;
	unique_ptr<CurrentUserSecurity> security;
	map<uint64_t, HANDLE> clients;
#else
	int lockDescriptor = -1;
	int socketDescriptor = -1;
	filesystem::path socketPath;
	map<uint64_t, int> clients;
#endif
};

#ifdef _WIN32
namespace {
HANDLE CreateServerPipe(
	const wstring& pipeName,
	CurrentUserSecurity& security) {
	return CreateNamedPipeW(
		pipeName.c_str(),
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
		PIPE_UNLIMITED_INSTANCES,
		64u * 1024u,
		64u * 1024u,
		0,
		security.Attributes());
}

bool AcceptPipe(HANDLE pipe, bool& connected, wstring& error) {
	connected = false;
	const BOOL accepted = ConnectNamedPipe(pipe, nullptr);
	const DWORD code = accepted ? ERROR_SUCCESS : GetLastError();
	if (accepted || code == ERROR_PIPE_CONNECTED || code == ERROR_NO_DATA) {
		connected = true;
		return true;
	}
	if (code == ERROR_PIPE_LISTENING) return true;
	error = L"The profile instance pipe failed while accepting a request.";
	return false;
}

HANDLE ConnectClientPipe(
	const wstring& pipeName,
	DWORD access,
	chrono::milliseconds timeout,
	wstring& error) {
	const auto deadline = chrono::steady_clock::now() + timeout;
	for (;;) {
		const auto remaining = chrono::duration_cast<chrono::milliseconds>(
			deadline - chrono::steady_clock::now());
		if (remaining <= chrono::milliseconds::zero()) {
			error = L"The primary MineBackup instance is not responding.";
			return INVALID_HANDLE_VALUE;
		}
		const DWORD wait = static_cast<DWORD>(min<long long>(
			max<long long>(remaining.count(), 1), 100));
		if (!WaitNamedPipeW(pipeName.c_str(), wait)) {
			const DWORD code = GetLastError();
			if (code == ERROR_SEM_TIMEOUT || code == ERROR_PIPE_BUSY
				|| code == ERROR_FILE_NOT_FOUND) {
				this_thread::sleep_for(chrono::milliseconds(5));
				continue;
			}
			error = L"The primary MineBackup instance is not responding.";
			return INVALID_HANDLE_VALUE;
		}
		HANDLE client = CreateFileW(
			pipeName.c_str(), access, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (client != INVALID_HANDLE_VALUE) return client;
		const DWORD code = GetLastError();
		if (code != ERROR_PIPE_BUSY && code != ERROR_FILE_NOT_FOUND) {
			error = L"Could not connect to the primary MineBackup instance.";
			return INVALID_HANDLE_VALUE;
		}
		this_thread::sleep_for(chrono::milliseconds(5));
	}
}
} // namespace
#endif

SingleInstanceService::SingleInstanceService() : impl_(make_unique<Impl>()) {}

SingleInstanceService::~SingleInstanceService() {
#ifdef _WIN32
	for (const auto& [id, client] : impl_->clients) {
		(void)id;
		DisconnectNamedPipe(client);
		CloseHandle(client);
	}
	if (impl_->pipe != INVALID_HANDLE_VALUE) CloseHandle(impl_->pipe);
	if (impl_->mutex) {
		ReleaseMutex(impl_->mutex);
		CloseHandle(impl_->mutex);
	}
#else
	for (const auto& [id, client] : impl_->clients) {
		(void)id;
		::close(client);
	}
	if (impl_->socketDescriptor >= 0) ::close(impl_->socketDescriptor);
	if (impl_->lockDescriptor >= 0) {
		::flock(impl_->lockDescriptor, LOCK_UN);
		::close(impl_->lockDescriptor);
		error_code ignored;
		filesystem::remove(impl_->socketPath, ignored);
	}
#endif
}

InstanceAcquireResult SingleInstanceService::Acquire(
	const wstring& profileIdentity,
	const filesystem::path& runtimeRoot,
	wstring& error) {
	error.clear();
	if (profileIdentity.empty()) {
		error = L"The profile identity is empty.";
		return InstanceAcquireResult::Failed;
	}
	impl_->token = EndpointToken(profileIdentity);
#ifdef _WIN32
	impl_->security = make_unique<CurrentUserSecurity>();
	if (!impl_->security->Initialize(error)) return InstanceAcquireResult::Failed;
	const wstring mutexName = L"Local\\MineBackup.Profile." + impl_->token;
	impl_->pipeName = L"\\\\.\\pipe\\MineBackup.Profile." + impl_->token;
	impl_->mutex = CreateMutexW(
		impl_->security->Attributes(), TRUE, mutexName.c_str());
	if (!impl_->mutex) {
		error = L"Could not create the profile instance mutex.";
		return InstanceAcquireResult::Failed;
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(impl_->mutex);
		impl_->mutex = nullptr;
		return InstanceAcquireResult::AlreadyRunning;
	}
	impl_->pipe = CreateServerPipe(impl_->pipeName, *impl_->security);
	if (impl_->pipe == INVALID_HANDLE_VALUE) {
		error = L"Could not create the profile instance pipe.";
		ReleaseMutex(impl_->mutex);
		CloseHandle(impl_->mutex);
		impl_->mutex = nullptr;
		return InstanceAcquireResult::Failed;
	}
#else
	error_code fileError;
	filesystem::create_directories(runtimeRoot, fileError);
	if (fileError) {
		error = L"Could not create the instance runtime directory.";
		return InstanceAcquireResult::Failed;
	}
	const auto lockPath = runtimeRoot / (L"instance-" + impl_->token + L".lock");
	const auto socketDirectory = UserSocketDirectory(error);
	if (socketDirectory.empty()) return InstanceAcquireResult::Failed;
	impl_->socketPath = socketDirectory / (L"instance-" + impl_->token + L".sock");
	impl_->lockDescriptor = ::open(lockPath.c_str(), O_RDWR | O_CREAT, 0600);
	if (impl_->lockDescriptor < 0) {
		error = L"Could not open the profile instance lock.";
		return InstanceAcquireResult::Failed;
	}
	if (::flock(impl_->lockDescriptor, LOCK_EX | LOCK_NB) != 0) {
		::close(impl_->lockDescriptor);
		impl_->lockDescriptor = -1;
		return errno == EWOULDBLOCK
			? InstanceAcquireResult::AlreadyRunning : InstanceAcquireResult::Failed;
	}
	filesystem::remove(impl_->socketPath, fileError);
	impl_->socketDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (impl_->socketDescriptor < 0) {
		error = L"Could not create the profile instance socket.";
		return InstanceAcquireResult::Failed;
	}
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	const string nativePath = impl_->socketPath.string();
	if (nativePath.size() >= sizeof(address.sun_path)) {
		error = L"The profile instance socket path is too long.";
		return InstanceAcquireResult::Failed;
	}
	memcpy(address.sun_path, nativePath.c_str(), nativePath.size() + 1);
	if (::bind(impl_->socketDescriptor,
			reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
		|| ::chmod(impl_->socketPath.c_str(), 0600) != 0
		|| ::listen(impl_->socketDescriptor, 16) != 0) {
		error = L"Could not bind the profile instance socket.";
		return InstanceAcquireResult::Failed;
	}
	::fcntl(impl_->socketDescriptor, F_SETFL,
		::fcntl(impl_->socketDescriptor, F_GETFL) | O_NONBLOCK);
#endif
	return InstanceAcquireResult::Acquired;
}

bool SingleInstanceService::Send(const InstanceRequest& request, wstring& error) const {
	vector<uint8_t> message;
	if (!EncodeLegacy(request, message, error)) return false;
#ifdef _WIN32
	HANDLE client = ConnectClientPipe(
		impl_->pipeName, GENERIC_WRITE, chrono::seconds(2), error);
	if (client == INVALID_HANDLE_VALUE) {
		return false;
	}
	const bool success = WriteMessage(client, message, error);
	CloseHandle(client);
#else
	const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (client < 0) {
		error = L"Could not create a socket for the primary MineBackup instance.";
		return false;
	}
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	const string nativePath = impl_->socketPath.string();
	if (nativePath.size() >= sizeof(address.sun_path)) {
		::close(client);
		error = L"The profile instance socket path is too long.";
		return false;
	}
	memcpy(address.sun_path, nativePath.c_str(), nativePath.size() + 1);
	const bool connected = ::connect(client,
		reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
	const bool success = connected && WriteMessage(client, message, error);
	::close(client);
#endif
	if (!success && error.empty()) {
		error = L"Could not deliver the request to the primary MineBackup instance.";
	}
	return success;
}

vector<InstanceRequest> SingleInstanceService::PollRequests(wstring& error) {
	vector<InstanceRequest> requests;
	error.clear();
#ifdef _WIN32
	if (impl_->pipe == INVALID_HANDLE_VALUE) return requests;
	bool connected = false;
	if (!AcceptPipe(impl_->pipe, connected, error) || !connected) return requests;
	vector<uint8_t> message;
	if (ReadMessage(impl_->pipe, message, chrono::seconds(2), error)) {
		InstanceRequest request;
		if (DecodeLegacy(message, request, error)) requests.push_back(std::move(request));
	}
	DisconnectNamedPipe(impl_->pipe);
#else
	if (impl_->socketDescriptor < 0) return requests;
	for (;;) {
		const int client = ::accept(impl_->socketDescriptor, nullptr, nullptr);
		if (client < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				error = L"The profile instance socket failed while accepting a request.";
			}
			break;
		}
		vector<uint8_t> message;
		if (ReadMessage(client, message, chrono::seconds(2), error)) {
			InstanceRequest request;
			if (DecodeLegacy(message, request, error)) requests.push_back(std::move(request));
		}
		::close(client);
	}
#endif
	return requests;
}

bool SingleInstanceService::Exchange(
	const InstanceControlRequest& request,
	InstanceControlResponse& response,
	wstring& error,
	chrono::milliseconds timeout) const {
	vector<uint8_t> message;
	if (!EncodeControlRequest(request, message, error)) return false;
#ifdef _WIN32
	HANDLE client = ConnectClientPipe(impl_->pipeName,
		GENERIC_READ | GENERIC_WRITE,
		min(timeout, chrono::duration_cast<chrono::milliseconds>(chrono::seconds(2))),
		error);
	if (client == INVALID_HANDLE_VALUE) {
		return false;
	}
	const bool written = WriteMessage(client, message, error);
	vector<uint8_t> reply;
	const bool read = written && ReadMessage(client, reply, timeout, error);
	CloseHandle(client);
#else
	const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (client < 0) {
		error = L"Could not create a socket for the primary MineBackup instance.";
		return false;
	}
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	const string nativePath = impl_->socketPath.string();
	if (nativePath.size() >= sizeof(address.sun_path)) {
		::close(client);
		error = L"The profile instance socket path is too long.";
		return false;
	}
	memcpy(address.sun_path, nativePath.c_str(), nativePath.size() + 1);
	const bool connected = ::connect(client,
		reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
	const bool written = connected && WriteMessage(client, message, error);
	vector<uint8_t> reply;
	const bool read = written && ReadMessage(client, reply, timeout, error);
	::close(client);
#endif
	if (!read) return false;
	if (!DecodeControlResponse(reply, response, error)
		|| response.requestId != request.requestId) {
		if (error.empty()) error = L"The control response requestId does not match.";
		return false;
	}
	return true;
}

vector<InstanceControlExchange> SingleInstanceService::PollControlRequests(wstring& error) {
	vector<InstanceControlExchange> requests;
	error.clear();
#ifdef _WIN32
	if (impl_->pipe == INVALID_HANDLE_VALUE) return requests;
	bool connected = false;
	if (!AcceptPipe(impl_->pipe, connected, error) || !connected) return requests;
	HANDLE client = impl_->pipe;
	impl_->pipe = CreateServerPipe(impl_->pipeName, *impl_->security);
	if (impl_->pipe == INVALID_HANDLE_VALUE) {
		error = L"Could not create the next profile instance pipe.";
		DisconnectNamedPipe(client);
		CloseHandle(client);
		return requests;
	}
	DWORD blockingMode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
	if (!SetNamedPipeHandleState(client, &blockingMode, nullptr, nullptr)) {
		error = L"Could not switch the accepted instance pipe to blocking mode.";
		DisconnectNamedPipe(client);
		CloseHandle(client);
		return requests;
	}
	vector<uint8_t> message;
	InstanceControlRequest request;
	if (!ReadMessage(client, message, chrono::seconds(2), error)
		|| !DecodeControlRequest(message, request, error)) {
		DisconnectNamedPipe(client);
		CloseHandle(client);
		return requests;
	}
	const uint64_t id = impl_->nextConnectionId++;
	impl_->clients[id] = client;
	requests.push_back({id, std::move(request)});
#else
	if (impl_->socketDescriptor < 0) return requests;
	for (;;) {
		const int client = ::accept(impl_->socketDescriptor, nullptr, nullptr);
		if (client < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				error = L"The profile instance socket failed while accepting a request.";
			}
			break;
		}
		vector<uint8_t> message;
		InstanceControlRequest request;
		if (!ReadMessage(client, message, chrono::seconds(2), error)
			|| !DecodeControlRequest(message, request, error)) {
			::close(client);
			continue;
		}
		const uint64_t id = impl_->nextConnectionId++;
		impl_->clients[id] = client;
		requests.push_back({id, std::move(request)});
	}
#endif
	return requests;
}

bool SingleInstanceService::Reply(
	uint64_t connectionId,
	const InstanceControlResponse& response,
	wstring& error) {
	const auto found = impl_->clients.find(connectionId);
	if (found == impl_->clients.end()) {
		error = L"The instance control connection no longer exists.";
		return false;
	}

	// 无论编码或写入是否成功，保留的连接都必须在本次回复结束时关闭。
	auto closeClient = [&]() {
#ifdef _WIN32
		const HANDLE client = found->second;
		FlushFileBuffers(client);
		DisconnectNamedPipe(client);
		CloseHandle(client);
#else
		const int client = found->second;
		::shutdown(client, SHUT_RDWR);
		::close(client);
#endif
		impl_->clients.erase(connectionId);
	};

	vector<uint8_t> message;
	if (!EncodeControlResponse(response, message, error)) {
		// 将超大响应降级成一个很小的协议错误，避免客户端等待到超时。
		InstanceControlResponse fallback = response;
		fallback.accepted = false;
		fallback.exitCode = 5;
		fallback.payload.clear();
		fallback.error = "instance_response_too_large";
		wstring fallbackError;
		if (EncodeControlResponse(fallback, message, fallbackError)) {
			const bool success = WriteMessage(found->second, message, error);
			(void)success;
			closeClient();
			return false;
		}
		closeClient();
		if (error.empty()) error = std::move(fallbackError);
		return false;
	}

#ifdef _WIN32
	const HANDLE client = found->second;
	const bool success = WriteMessage(client, message, error);
#else
	const int client = found->second;
	const bool success = WriteMessage(client, message, error);
#endif
	closeClient();
	return success;
}
