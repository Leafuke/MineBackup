#include "knotlink/TcpClient.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (condition) {
        return;
    }
    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

class ListeningSocket {
public:
    ListeningSocket() {
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == SOCK_INVALID) {
            throw std::runtime_error("test socket() failed");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(socket_, 1) != 0) {
            sock_close(socket_);
            throw std::runtime_error("test bind/listen failed");
        }

#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            sock_close(socket_);
            throw std::runtime_error("test getsockname failed");
        }
        port_ = ntohs(address.sin_port);
    }

    ~ListeningSocket() {
        if (socket_ != SOCK_INVALID) {
            sock_close(socket_);
        }
    }

    ListeningSocket(const ListeningSocket&) = delete;
    ListeningSocket& operator=(const ListeningSocket&) = delete;

    uint16_t port() const { return port_; }

    sock_t acceptOne() const {
        return accept(socket_, nullptr, nullptr);
    }

    void close() {
        if (socket_ != SOCK_INVALID) {
            sock_close(socket_);
            socket_ = SOCK_INVALID;
        }
    }

private:
    sock_t socket_ = SOCK_INVALID;
    uint16_t port_ = 0;
};

std::string Frame(const std::string& payload) {
    constexpr char magic[4] = {0x4B, 0x4B, 0x00, 0x02};
    const uint32_t length = htonl(static_cast<uint32_t>(payload.size()));
    std::string frame(magic, sizeof(magic));
    frame.append(reinterpret_cast<const char*>(&length), sizeof(length));
    frame.append(payload);
    return frame;
}

bool SendRaw(sock_t socket, const char* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        const int result = ::send(socket, data + sent,
                                  static_cast<int>(length - sent), SOCK_SEND_FLAGS);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

void TestFramingAndStop() {
    ListeningSocket server;
    std::atomic<bool> requestReceived{false};
    std::thread serverThread([&] {
        const sock_t peer = server.acceptOne();
        if (peer == SOCK_INVALID) {
            return;
        }

        std::vector<char> request;
        char chunk[7];
        while (request.size() < 8) {
            const int count = recv(peer, chunk, sizeof(chunk), 0);
            if (count <= 0) {
                sock_close(peer);
                return;
            }
            request.insert(request.end(), chunk, chunk + count);
        }
        uint32_t netLength = 0;
        std::memcpy(&netLength, request.data() + 4, sizeof(netLength));
        const std::size_t expected = 8 + ntohl(netLength);
        while (request.size() < expected) {
            const int count = recv(peer, chunk, sizeof(chunk), 0);
            if (count <= 0) {
                sock_close(peer);
                return;
            }
            request.insert(request.end(), chunk, chunk + count);
        }
        requestReceived.store(
            std::string(request.data() + 8, expected - 8) == "client_request");

        const std::string first = Frame("split_response");
        const std::string second = Frame("coalesced_response");
        SendRaw(peer, first.data(), 3);
        std::this_thread::sleep_for(5ms);
        SendRaw(peer, first.data() + 3, first.size() - 3);
        SendRaw(peer, second.data(), second.size());
        std::this_thread::sleep_for(20ms);
        sock_shutdown(peer);
        sock_close(peer);
    });

    std::mutex responseMutex;
    std::condition_variable responseCondition;
    std::vector<std::string> responses;
    const auto started = std::chrono::steady_clock::now();
    {
        knotlink::TcpClient client;
        client.setOnDataReceivedCallback([&](const std::string& response) {
            {
                std::lock_guard<std::mutex> lock(responseMutex);
                responses.push_back(response);
            }
            responseCondition.notify_all();
        });
        Check(client.connectToServer("127.0.0.1", server.port()),
              "client should connect to loopback server");
        Check(client.sendData("client_request"), "sendData should send a complete frame");

        std::unique_lock<std::mutex> lock(responseMutex);
        Check(responseCondition.wait_for(lock, 2s, [&] { return responses.size() == 2; }),
              "split and coalesced frames should both be decoded");
        lock.unlock();
        client.stop();
        client.stop();
    }
    serverThread.join();

    Check(requestReceived.load(), "server should receive the complete client frame");
    Check(responses == std::vector<std::string>({"split_response", "coalesced_response"}),
          "decoded response order should be preserved");
    Check(std::chrono::steady_clock::now() - started < 2s,
          "repeated stop and destruction should complete promptly");
}

void TestConnectionRefusalIsBounded() {
    ListeningSocket unusedPort;
    const uint16_t port = unusedPort.port();
    unusedPort.close();

    const auto started = std::chrono::steady_clock::now();
    {
        knotlink::TcpClient client;
        Check(!client.connectToServer("127.0.0.1", port),
              "connection refusal should be reported");
        Check(!client.isRunning(), "refused connection must not enter running state");
        client.stop();
    }
    Check(std::chrono::steady_clock::now() - started < 2s,
          "connection refusal and destruction should be bounded");
}

void TestRemoteDisconnectStopsClient() {
    ListeningSocket server;
    std::thread serverThread([&] {
        const sock_t peer = server.acceptOne();
        if (peer != SOCK_INVALID) {
            sock_shutdown(peer);
            sock_close(peer);
        }
    });

    knotlink::TcpClient client;
    Check(client.connectToServer("127.0.0.1", server.port()),
          "disconnect test should connect");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (client.isRunning() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    Check(!client.isRunning(), "remote disconnect should clear running state");
    client.stop();
    serverThread.join();
}

} // namespace

int main() {
    sock_init();
    try {
        TestFramingAndStop();
        TestConnectionRefusalIsBounded();
        TestRemoteDisconnectStopsClient();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "[FAIL] unexpected exception: " << error.what() << '\n';
    }
    sock_cleanup();

    if (failures == 0) {
        std::cout << "KnotLink SDK lifecycle tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
