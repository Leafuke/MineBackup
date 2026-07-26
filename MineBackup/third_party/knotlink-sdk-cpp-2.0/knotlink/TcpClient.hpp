/*
 * KnotLink SDK - C++
 * Copyright (c) 2024-2026 KnotLink Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TCP_CLIENT_HPP
#define TCP_CLIENT_HPP

#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>

// ---- 跨平台 socket 抽象 ----
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using sock_t = SOCKET;
  #define SOCK_INVALID INVALID_SOCKET
  #define SOCK_ERRNO  WSAGetLastError()
  inline void sock_init() {
      WSADATA wsaData;
      if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
          throw std::runtime_error("WSAStartup failed");
  }
  inline void sock_cleanup() { WSACleanup(); }
  inline void sock_close(sock_t s) { closesocket(s); }
  inline bool sock_would_block(int err) { return err == WSAEWOULDBLOCK; }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  using sock_t = int;
  #define SOCK_INVALID (-1)
  #define SOCK_ERRNO  errno
  inline void sock_init() {}
  inline void sock_cleanup() {}
  inline void sock_close(sock_t s) { close(s); }
  inline bool sock_would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
#endif

namespace knotlink {

// ---- 日志回调类型 ----
using LogCallback = std::function<void(const std::string&)>;

class TcpClient {
public:
    TcpClient() : tcpSocket(SOCK_INVALID), running(false) {
        sock_init();
    }

    ~TcpClient() {
        stopHeartbeat();
        running = false;
        if (readThread.joinable()) {
            if (tcpSocket != SOCK_INVALID) sock_close(tcpSocket);
            readThread.join();
        }
        if (tcpSocket != SOCK_INVALID) sock_close(tcpSocket);
        sock_cleanup();
    }

    bool connectToServer(const std::string& ip, uint16_t port) {
        tcpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcpSocket == SOCK_INVALID) {
            log(LogLevel::Error, "Failed to create socket");
            return false;
        }

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr);

        if (connect(tcpSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
            log(LogLevel::Error, "Failed to connect to server");
            sock_close(tcpSocket);
            tcpSocket = SOCK_INVALID;
            return false;
        }

        running = true;
        readThread = std::thread(&TcpClient::readData, this);
        startHeartbeat();
        return true;
    }

    void sendData(const std::string& data) {
        if (tcpSocket == SOCK_INVALID) {
            log(LogLevel::Warning, "Cannot send, socket not connected");
            return;
        }

        uint32_t len = static_cast<uint32_t>(data.size());
        uint32_t netLen = htonl(len);

        std::lock_guard<std::mutex> lock(sendMutex);
        if (::send(tcpSocket, MAGIC, MAGIC_LEN, 0) == -1) {
            handleError(SOCK_ERRNO);
            return;
        }
        if (::send(tcpSocket, (const char*)&netLen, sizeof(netLen), 0) == -1) {
            handleError(SOCK_ERRNO);
            return;
        }
        if (::send(tcpSocket, data.c_str(), static_cast<int>(data.size()), 0) == -1) {
            handleError(SOCK_ERRNO);
        }
    }

    void startHeartbeat() {
        heartBeatThread = std::thread(&TcpClient::sendHeartbeat, this);
    }

    void stopHeartbeat() {
        if (heartBeatThread.joinable()) {
            heartBeatThread.join();
        }
    }

    void setOnDataReceivedCallback(const std::function<void(const std::string&)>& callback) {
        onDataReceivedCallback = callback;
    }

    // ---- 日志 ----
    enum class LogLevel { Debug, Info, Warning, Error };

    void setLogCallback(LogCallback callback) {
        std::lock_guard<std::mutex> lock(logMutex);
        logCallback = std::move(callback);
    }

    bool running;

private:
    sock_t tcpSocket;
    std::thread heartBeatThread;
    std::thread readThread;
    std::mutex sendMutex;
    std::mutex logMutex;
    std::string heartbeatMessage = "heartbeat";
    std::string heartbeatResponse = "heartbeat_response";
    std::function<void(const std::string&)> onDataReceivedCallback;
    LogCallback logCallback;

    std::vector<char> recvBuffer;
    std::mutex recvMutex;
    static constexpr uint32_t MAX_MSG_SIZE = 16 * 1024 * 1024;
    static constexpr char MAGIC[4] = {0x4B, 0x4B, 0x00, 0x02};  // KK + 版本号 2.0
    static constexpr int MAGIC_LEN = 4;

    // ---- 日志输出 ----
    void log(LogLevel level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(logMutex);
        if (logCallback) {
            const char* prefix;
            switch (level) {
                case LogLevel::Debug:   prefix = "[DEBUG] "; break;
                case LogLevel::Info:    prefix = "[INFO] ";  break;
                case LogLevel::Warning: prefix = "[WARN] ";  break;
                case LogLevel::Error:   prefix = "[ERROR] "; break;
                default:                prefix = "";
            }
            logCallback(prefix + msg);
        }
    }

    void sendHeartbeat() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(180));
            if (running) {
                sendData(heartbeatMessage);
            }
        }
    }

    void handleError(int socketError) {
        log(LogLevel::Error, "Socket error: " + std::to_string(socketError));
        running = false;
    }

    void processBuffer() {
        const int HEADER_LEN = MAGIC_LEN + 4;  // 8: 魔数 + 长度字段
        while (true) {
            if (recvBuffer.size() < HEADER_LEN) break;

            // 校验魔数
            if (memcmp(recvBuffer.data(), MAGIC, MAGIC_LEN) != 0) {
                log(LogLevel::Error, "Magic mismatch, disconnecting");
                running = false;
                sock_close(tcpSocket);
                return;
            }

            uint32_t netLen;
            memcpy(&netLen, recvBuffer.data() + MAGIC_LEN, 4);
            uint32_t len = ntohl(netLen);
            if (len == 0 || len > MAX_MSG_SIZE) {
                log(LogLevel::Error, "Invalid message length: " + std::to_string(len) + ", disconnecting");
                running = false;
                sock_close(tcpSocket);
                return;
            }
            if (recvBuffer.size() < HEADER_LEN + len) break;

            std::string msg(recvBuffer.data() + HEADER_LEN, len);
            recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + HEADER_LEN + len);

            if (msg == heartbeatResponse) {
                log(LogLevel::Debug, "Heartbeat response received");
            } else {
                log(LogLevel::Debug, "Received: " + msg);
                if (onDataReceivedCallback) {
                    onDataReceivedCallback(msg);
                }
            }
        }
    }

    void readData() {
        char chunk[4096];
        while (running) {
            int bytesRead = ::recv(tcpSocket, chunk, sizeof(chunk), 0);
            if (bytesRead > 0) {
                std::lock_guard<std::mutex> lock(recvMutex);
                recvBuffer.insert(recvBuffer.end(), chunk, chunk + bytesRead);
                processBuffer();
            } else if (bytesRead == 0) {
                log(LogLevel::Info, "Server disconnected");
                running = false;
                break;
            } else {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAECONNRESET || err == WSAECONNABORTED)
#else
                int err = errno;
                if (err == ECONNRESET || err == ECONNABORTED)
#endif
                {
                    log(LogLevel::Info, "Connection reset");
                } else {
                    handleError(err);
                }
                running = false;
                break;
            }
        }
    }
};

} // namespace knotlink

#endif // TCP_CLIENT_HPP
