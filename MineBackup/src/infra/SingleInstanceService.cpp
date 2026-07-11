#include "SingleInstanceService.h"

#include "json.hpp"
#include "text_to_text.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <Aclapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace std;

namespace {

constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kMaximumPayloadBytes = 64u * 1024u;

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
            && GetTokenInformation(token, TokenUser, tokenUser.data(), required, &required) != FALSE;
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
            || !SetSecurityDescriptorDacl(&descriptor_, TRUE, accessControlList_, FALSE)) {
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

string RequestTypeName(InstanceRequestType type) {
    switch (type) {
    case InstanceRequestType::Activate: return "Activate";
    case InstanceRequestType::SelectConfig: return "SelectConfig";
    case InstanceRequestType::RunSpecial: return "RunSpecial";
    }
    return {};
}

bool EncodeRequest(const InstanceRequest& request, vector<uint8_t>& message, wstring& error) {
    nlohmann::json root;
    root["version"] = kProtocolVersion;
    root["type"] = RequestTypeName(request.type);
    root["stableId"] = wstring_to_utf8(request.stableId);
    const string payload = root.dump();
    if (payload.empty() || payload.size() > kMaximumPayloadBytes) {
        error = L"The instance request exceeds the protocol size limit.";
        return false;
    }
    const uint32_t length = static_cast<uint32_t>(payload.size());
    message.resize(sizeof(length) + payload.size());
    memcpy(message.data(), &length, sizeof(length));
    memcpy(message.data() + sizeof(length), payload.data(), payload.size());
    return true;
}

bool DecodeRequest(const vector<uint8_t>& message, InstanceRequest& request, wstring& error) {
    if (message.size() < sizeof(uint32_t)) {
        error = L"The instance request is truncated.";
        return false;
    }
    uint32_t length = 0;
    memcpy(&length, message.data(), sizeof(length));
    if (length == 0 || length > kMaximumPayloadBytes || message.size() != sizeof(length) + length) {
        error = L"The instance request has an invalid length prefix.";
        return false;
    }
    const string payload(reinterpret_cast<const char*>(message.data() + sizeof(length)), length);
    const auto root = nlohmann::json::parse(payload, nullptr, false);
    if (root.is_discarded() || !root.is_object() || root.value("version", 0u) != kProtocolVersion) {
        error = L"The instance request has an unsupported protocol version.";
        return false;
    }
    const string type = root.value("type", string{});
    if (type == "Activate") request.type = InstanceRequestType::Activate;
    else if (type == "SelectConfig") request.type = InstanceRequestType::SelectConfig;
    else if (type == "RunSpecial") request.type = InstanceRequestType::RunSpecial;
    else {
        error = L"The instance request type is invalid.";
        return false;
    }
    request.stableId = utf8_to_wstring(root.value("stableId", string{}));
    if (request.type != InstanceRequestType::Activate && request.stableId.empty()) {
        error = L"The instance request requires a stable identifier.";
        return false;
    }
    return true;
}

} // namespace

struct SingleInstanceService::Impl {
    wstring token;
#ifdef _WIN32
    HANDLE mutex = nullptr;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    wstring pipeName;
#else
    int lockDescriptor = -1;
    int socketDescriptor = -1;
    filesystem::path socketPath;
#endif
};

SingleInstanceService::SingleInstanceService() : impl_(make_unique<Impl>()) {}

SingleInstanceService::~SingleInstanceService() {
#ifdef _WIN32
    if (impl_->pipe != INVALID_HANDLE_VALUE) CloseHandle(impl_->pipe);
    if (impl_->mutex) {
        ReleaseMutex(impl_->mutex);
        CloseHandle(impl_->mutex);
    }
#else
    if (impl_->socketDescriptor >= 0) close(impl_->socketDescriptor);
    if (impl_->lockDescriptor >= 0) {
        flock(impl_->lockDescriptor, LOCK_UN);
        close(impl_->lockDescriptor);
        error_code ignored;
        filesystem::remove(impl_->socketPath, ignored);
    }
#endif
}

InstanceAcquireResult SingleInstanceService::Acquire(
    const wstring& profileIdentity, const filesystem::path& runtimeRoot, wstring& error) {
    error.clear();
    if (profileIdentity.empty()) {
        error = L"The profile identity is empty.";
        return InstanceAcquireResult::Failed;
    }
    impl_->token = EndpointToken(profileIdentity);
#ifdef _WIN32
    CurrentUserSecurity security;
    if (!security.Initialize(error)) return InstanceAcquireResult::Failed;
    const wstring mutexName = L"Local\\MineBackup.Profile." + impl_->token;
    impl_->pipeName = L"\\\\.\\pipe\\MineBackup.Profile." + impl_->token;
    impl_->mutex = CreateMutexW(security.Attributes(), TRUE, mutexName.c_str());
    if (!impl_->mutex) {
        error = L"Could not create the profile instance mutex.";
        return InstanceAcquireResult::Failed;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(impl_->mutex);
        impl_->mutex = nullptr;
        return InstanceAcquireResult::AlreadyRunning;
    }
    impl_->pipe = CreateNamedPipeW(impl_->pipeName.c_str(), PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT, 1,
        kMaximumPayloadBytes + sizeof(uint32_t), kMaximumPayloadBytes + sizeof(uint32_t), 0, security.Attributes());
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
    impl_->socketPath = runtimeRoot / (L"instance-" + impl_->token + L".sock");
    impl_->lockDescriptor = open(lockPath.c_str(), O_RDWR | O_CREAT, 0600);
    if (impl_->lockDescriptor < 0) {
        error = L"Could not open the profile instance lock.";
        return InstanceAcquireResult::Failed;
    }
    if (flock(impl_->lockDescriptor, LOCK_EX | LOCK_NB) != 0) {
        close(impl_->lockDescriptor);
        impl_->lockDescriptor = -1;
        return errno == EWOULDBLOCK ? InstanceAcquireResult::AlreadyRunning : InstanceAcquireResult::Failed;
    }
    filesystem::remove(impl_->socketPath, fileError);
    impl_->socketDescriptor = socket(AF_UNIX, SOCK_STREAM, 0);
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
    if (bind(impl_->socketDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
        || chmod(impl_->socketPath.c_str(), 0600) != 0 || listen(impl_->socketDescriptor, 4) != 0) {
        error = L"Could not bind the profile instance socket.";
        return InstanceAcquireResult::Failed;
    }
    fcntl(impl_->socketDescriptor, F_SETFL, fcntl(impl_->socketDescriptor, F_GETFL) | O_NONBLOCK);
#endif
    return InstanceAcquireResult::Acquired;
}

bool SingleInstanceService::Send(const InstanceRequest& request, wstring& error) const {
    vector<uint8_t> message;
    if (!EncodeRequest(request, message, error)) return false;
#ifdef _WIN32
    if (!WaitNamedPipeW(impl_->pipeName.c_str(), 2000)) {
        error = L"The primary MineBackup instance is not responding.";
        return false;
    }
    HANDLE pipe = CreateFileW(impl_->pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = L"Could not connect to the primary MineBackup instance.";
        return false;
    }
    DWORD written = 0;
    const bool success = WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)
        && written == message.size();
    CloseHandle(pipe);
#else
    const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        error = L"Could not create a socket for the primary MineBackup instance.";
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const string nativePath = impl_->socketPath.string();
    if (nativePath.size() >= sizeof(address.sun_path)) {
        close(descriptor);
        error = L"The profile instance socket path is too long.";
        return false;
    }
    memcpy(address.sun_path, nativePath.c_str(), nativePath.size() + 1);
    bool success = connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    size_t offset = 0;
    while (success && offset < message.size()) {
        const ssize_t sent = write(descriptor, message.data() + offset, message.size() - offset);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) success = false;
        else offset += static_cast<size_t>(sent);
    }
    close(descriptor);
#endif
    if (!success) error = L"Could not deliver the request to the primary MineBackup instance.";
    return success;
}

vector<InstanceRequest> SingleInstanceService::PollRequests(wstring& error) {
    vector<InstanceRequest> requests;
    error.clear();
#ifdef _WIN32
    if (impl_->pipe == INVALID_HANDLE_VALUE) return requests;
    const BOOL connected = ConnectNamedPipe(impl_->pipe, nullptr);
    const DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && connectError != ERROR_PIPE_CONNECTED && connectError != ERROR_NO_DATA) {
        if (connectError == ERROR_PIPE_LISTENING) return requests;
        error = L"The profile instance pipe failed while accepting a request.";
        return requests;
    }
    array<uint8_t, kMaximumPayloadBytes + sizeof(uint32_t)> buffer{};
    DWORD bytesRead = 0;
    if (ReadFile(impl_->pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0) {
        vector<uint8_t> message(buffer.begin(), buffer.begin() + bytesRead);
        InstanceRequest request;
        if (DecodeRequest(message, request, error)) requests.push_back(std::move(request));
    }
    DisconnectNamedPipe(impl_->pipe);
#else
    if (impl_->socketDescriptor < 0) return requests;
    for (;;) {
        const int client = accept(impl_->socketDescriptor, nullptr, nullptr);
        if (client < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) error = L"The profile instance socket failed while accepting a request.";
            break;
        }
        vector<uint8_t> message;
        array<uint8_t, 4096> buffer{};
        for (;;) {
            const ssize_t count = read(client, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            message.insert(message.end(), buffer.begin(), buffer.begin() + count);
            if (message.size() > kMaximumPayloadBytes + sizeof(uint32_t)) break;
        }
        close(client);
        InstanceRequest request;
        if (DecodeRequest(message, request, error)) requests.push_back(std::move(request));
    }
#endif
    return requests;
}
