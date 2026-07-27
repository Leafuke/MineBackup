/*
 * KnotLink SDK - C++
 * Copyright (c) 2024-2026 KnotLink Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TCP_CLIENT_HPP
#define TCP_CLIENT_HPP

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using sock_t = SOCKET;
  #define SOCK_INVALID INVALID_SOCKET
  #define SOCK_ERRNO WSAGetLastError()
  inline void sock_init() {
      WSADATA wsaData;
      if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
          throw std::runtime_error("WSAStartup failed");
      }
  }
  inline void sock_cleanup() { WSACleanup(); }
  inline void sock_close(sock_t socket) { closesocket(socket); }
  inline void sock_shutdown(sock_t socket) { shutdown(socket, SD_BOTH); }
  inline bool sock_set_blocking(sock_t socket, bool blocking) {
      u_long mode = blocking ? 0UL : 1UL;
      return ioctlsocket(socket, FIONBIO, &mode) == 0;
  }
  inline bool sock_connect_in_progress(int error) {
      return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
  }
  constexpr int SOCK_SEND_FLAGS = 0;
#else
  #include <arpa/inet.h>
  #include <cerrno>
  #include <netinet/in.h>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using sock_t = int;
  #define SOCK_INVALID (-1)
  #define SOCK_ERRNO errno
  inline void sock_init() {}
  inline void sock_cleanup() {}
  inline void sock_close(sock_t socket) { close(socket); }
  inline void sock_shutdown(sock_t socket) { shutdown(socket, SHUT_RDWR); }
  inline bool sock_set_blocking(sock_t socket, bool blocking) {
      const int flags = fcntl(socket, F_GETFL, 0);
      return flags >= 0 &&
             fcntl(socket, F_SETFL,
                   blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
  }
  inline bool sock_connect_in_progress(int error) {
      return error == EINPROGRESS || error == EWOULDBLOCK;
  }
  #ifdef MSG_NOSIGNAL
  constexpr int SOCK_SEND_FLAGS = MSG_NOSIGNAL;
  #else
  constexpr int SOCK_SEND_FLAGS = 0;
  #endif
#endif

namespace knotlink {

using LogCallback = std::function<void(const std::string&)>;

class TcpClient {
public:
    enum class LogLevel { Debug, Info, Warning, Error };

    TcpClient() {
        sock_init();
    }

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    ~TcpClient() {
        stop();
        sock_cleanup();
    }

    bool connectToServer(const std::string& ip, uint16_t port) {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
        if (running.load(std::memory_order_acquire)) {
            return true;
        }

        closeSocket();
        sock_t candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (candidate == SOCK_INVALID) {
            log(LogLevel::Error, "Failed to create socket");
            return false;
        }

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) != 1) {
            log(LogLevel::Error, "Invalid server address");
            sock_close(candidate);
            return false;
        }

        if (!connectWithTimeout(candidate, serverAddr, std::chrono::milliseconds(1500))) {
            log(LogLevel::Error, "Failed to connect to server");
            sock_close(candidate);
            return false;
        }

        tcpSocket_.store(candidate, std::memory_order_release);
        running.store(true, std::memory_order_release);
        readThread_ = std::thread(&TcpClient::readData, this);
        heartbeatThread_ = std::thread(&TcpClient::sendHeartbeat, this);
        return true;
    }

    bool sendData(const std::string& data) {
        if (data.empty() || data.size() > MAX_MSG_SIZE) {
            log(LogLevel::Error, "Invalid outgoing message length");
            return false;
        }

        const uint32_t netLength = htonl(static_cast<uint32_t>(data.size()));
        std::lock_guard<std::mutex> lock(sendMutex_);
        return sendAll(MAGIC, MAGIC_LEN) &&
               sendAll(reinterpret_cast<const char*>(&netLength), sizeof(netLength)) &&
               sendAll(data.data(), data.size());
    }

    void startHeartbeat() {
        // Heartbeat starts with a successful connection. Kept for source compatibility.
    }

    void stopHeartbeat() {
        stop();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
            running.store(false, std::memory_order_release);
            const sock_t socket = tcpSocket_.load(std::memory_order_acquire);
            if (socket != SOCK_INVALID) {
                sock_shutdown(socket);
            }
            heartbeatCv_.notify_all();
        }

        joinThread(heartbeatThread_);
        joinThread(readThread_);

        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
        closeSocket();
    }

    bool isRunning() const noexcept {
        return running.load(std::memory_order_acquire);
    }

    void setOnDataReceivedCallback(std::function<void(const std::string&)> callback) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        onDataReceivedCallback_ = std::move(callback);
    }

    void setLogCallback(LogCallback callback) {
        std::lock_guard<std::mutex> lock(logMutex_);
        logCallback_ = std::move(callback);
    }

    // Public for SDK 2.0 source compatibility. Prefer isRunning().
    std::atomic<bool> running{false};

private:
    std::atomic<sock_t> tcpSocket_{SOCK_INVALID};
    std::thread heartbeatThread_;
    std::thread readThread_;
    std::mutex lifecycleMutex_;
    std::mutex sendMutex_;
    std::mutex logMutex_;
    std::mutex callbackMutex_;
    std::mutex recvMutex_;
    std::condition_variable heartbeatCv_;
    std::vector<char> recvBuffer_;
    std::function<void(const std::string&)> onDataReceivedCallback_;
    LogCallback logCallback_;

    static constexpr uint32_t MAX_MSG_SIZE = 16 * 1024 * 1024;
    static constexpr char MAGIC[4] = {0x4B, 0x4B, 0x00, 0x02};
    static constexpr std::size_t MAGIC_LEN = sizeof(MAGIC);

    static void joinThread(std::thread& thread) {
        if (!thread.joinable()) {
            return;
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
            return;
        }
        thread.join();
    }

    static bool connectWithTimeout(sock_t socket, const sockaddr_in& address,
                                   std::chrono::milliseconds timeout) {
        if (!sock_set_blocking(socket, false)) {
            return false;
        }
        const int result = ::connect(
            socket,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address));
        if (result != 0 && !sock_connect_in_progress(SOCK_ERRNO)) {
            sock_set_blocking(socket, true);
            return false;
        }
        if (result == 0) {
            return sock_set_blocking(socket, true);
        }

        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(socket, &writable);
        timeval wait{};
        wait.tv_sec = static_cast<long>(timeout.count() / 1000);
        wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
#ifdef _WIN32
        const int selected = select(0, nullptr, &writable, nullptr, &wait);
#else
        const int selected = select(socket + 1, nullptr, &writable, nullptr, &wait);
#endif
        if (selected <= 0) {
            sock_set_blocking(socket, true);
            return false;
        }

        int socketError = 0;
#ifdef _WIN32
        int errorLength = sizeof(socketError);
#else
        socklen_t errorLength = sizeof(socketError);
#endif
        const bool connected =
            getsockopt(socket, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socketError), &errorLength) == 0 &&
            socketError == 0;
        return sock_set_blocking(socket, true) && connected;
    }

    void closeSocket() {
        const sock_t socket = tcpSocket_.exchange(SOCK_INVALID, std::memory_order_acq_rel);
        if (socket != SOCK_INVALID) {
            sock_close(socket);
        }
    }

    bool sendAll(const char* data, std::size_t length) {
        std::size_t sent = 0;
        while (sent < length && running.load(std::memory_order_acquire)) {
            const sock_t socket = tcpSocket_.load(std::memory_order_acquire);
            if (socket == SOCK_INVALID) {
                return false;
            }
            const std::size_t remaining = length - sent;
            const int chunkLength = remaining > static_cast<std::size_t>(INT_MAX)
                ? INT_MAX
                : static_cast<int>(remaining);
            const int result = ::send(socket, data + sent, chunkLength, SOCK_SEND_FLAGS);
            if (result <= 0) {
                handleError(SOCK_ERRNO);
                return false;
            }
            sent += static_cast<std::size_t>(result);
        }
        return sent == length;
    }

    void log(LogLevel level, const std::string& message) {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (!logCallback_) {
            return;
        }
        const char* prefix = "";
        switch (level) {
            case LogLevel::Debug: prefix = "[DEBUG] "; break;
            case LogLevel::Info: prefix = "[INFO] "; break;
            case LogLevel::Warning: prefix = "[WARN] "; break;
            case LogLevel::Error: prefix = "[ERROR] "; break;
        }
        logCallback_(prefix + message);
    }

    void sendHeartbeat() {
        std::unique_lock<std::mutex> lock(lifecycleMutex_);
        while (running.load(std::memory_order_acquire)) {
            if (heartbeatCv_.wait_for(lock, std::chrono::seconds(180), [this] {
                    return !running.load(std::memory_order_acquire);
                })) {
                break;
            }
            lock.unlock();
            sendData("heartbeat");
            lock.lock();
        }
    }

    void handleError(int socketError) {
        log(LogLevel::Error, "Socket error: " + std::to_string(socketError));
        running.store(false, std::memory_order_release);
        heartbeatCv_.notify_all();
        const sock_t socket = tcpSocket_.load(std::memory_order_acquire);
        if (socket != SOCK_INVALID) {
            sock_shutdown(socket);
        }
    }

    bool processBuffer() {
        constexpr std::size_t HEADER_LEN = MAGIC_LEN + sizeof(uint32_t);
        while (true) {
            if (recvBuffer_.size() < HEADER_LEN) {
                return true;
            }
            if (std::memcmp(recvBuffer_.data(), MAGIC, MAGIC_LEN) != 0) {
                log(LogLevel::Error, "Magic mismatch, disconnecting");
                return false;
            }

            uint32_t netLength = 0;
            std::memcpy(&netLength, recvBuffer_.data() + MAGIC_LEN, sizeof(netLength));
            const uint32_t length = ntohl(netLength);
            if (length == 0 || length > MAX_MSG_SIZE) {
                log(LogLevel::Error, "Invalid message length, disconnecting");
                return false;
            }
            if (recvBuffer_.size() < HEADER_LEN + length) {
                return true;
            }

            std::string message(recvBuffer_.data() + HEADER_LEN, length);
            recvBuffer_.erase(recvBuffer_.begin(),
                              recvBuffer_.begin() + HEADER_LEN + length);
            if (message == "heartbeat_response") {
                log(LogLevel::Debug, "Heartbeat response received");
                continue;
            }

            std::function<void(const std::string&)> callback;
            {
                std::lock_guard<std::mutex> callbackLock(callbackMutex_);
                callback = onDataReceivedCallback_;
            }
            if (callback) {
                callback(message);
            }
        }
    }

    void readData() {
        char chunk[4096];
        while (running.load(std::memory_order_acquire)) {
            const sock_t socket = tcpSocket_.load(std::memory_order_acquire);
            if (socket == SOCK_INVALID) {
                break;
            }
            const int bytesRead = ::recv(socket, chunk, sizeof(chunk), 0);
            if (bytesRead > 0) {
                std::lock_guard<std::mutex> lock(recvMutex_);
                recvBuffer_.insert(recvBuffer_.end(), chunk, chunk + bytesRead);
                if (!processBuffer()) {
                    handleError(0);
                    break;
                }
            } else if (bytesRead == 0) {
                log(LogLevel::Info, "Server disconnected");
                running.store(false, std::memory_order_release);
                heartbeatCv_.notify_all();
                break;
            } else if (running.load(std::memory_order_acquire)) {
                handleError(SOCK_ERRNO);
                break;
            }
        }
    }
};

} // namespace knotlink

#endif // TCP_CLIENT_HPP
