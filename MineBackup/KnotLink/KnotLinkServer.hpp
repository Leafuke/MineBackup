#ifndef KNOTLINK_SERVER_HPP
#define KNOTLINK_SERVER_HPP

// KnotLink 集成服务端 - 用于 Linux 和 MacOS
// 在这些平台上，MineBackup 同时作为 KnotLink 服务端和客户端运行
// 这样不需要单独的 KnotLink 服务端程序 —— 今后考虑windows平台也这么做

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <queue>
#include <chrono>
#include <cstring>
#include <memory>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

class KnotLinkServer;

// 客户端连接类
class KLClientConnection {
public:
    int socket;
    std::string channelKey;  // appID-socketID 或 appID-signalID
    std::string receiveBuffer;
    bool registered;
    std::chrono::steady_clock::time_point lastActivity;

    KLClientConnection(int sock) 
        : socket(sock), registered(false), 
          lastActivity(std::chrono::steady_clock::now()) {}

    ~KLClientConnection() {
        if (socket >= 0) {
            close(socket);
        }
    }
};

// KnotLink 服务端类
class KnotLinkServer {
public:
    // 单例模式
    static KnotLinkServer& getInstance() {
        static KnotLinkServer instance;
        return instance;
    }

    KnotLinkServer(const KnotLinkServer&) = delete;
    KnotLinkServer& operator=(const KnotLinkServer&) = delete;

    // 启动所有服务端端口
    bool start() {
        if (running) return true;
        
        running = true;

        // 启动信号端口 (6370) - 用于接收信号
        signalThread = std::thread(&KnotLinkServer::runSignalServer, this);
        
        // 启动订阅端口 (6372) - 用于订阅者注册和接收广播
        subscribeThread = std::thread(&KnotLinkServer::runSubscribeServer, this);
        
        // 启动查询端口 (6376) - 用于同步查询
        queryThread = std::thread(&KnotLinkServer::runQueryServer, this);
        
        // 启动响应者端口 (6378) - 用于响应者注册
        responserThread = std::thread(&KnotLinkServer::runResponserServer, this);

        std::cout << "[KnotLinkServer] All servers started" << std::endl;
        return true;
    }

    void stop() {
        if (!running) return;
        
        running = false;

        // 关闭所有监听socket (IPv4 和 IPv6)
        for (auto& [port, socks] : portSockets) {
            if (socks.first >= 0) { close(socks.first); socks.first = -1; }
            if (socks.second >= 0) { close(socks.second); socks.second = -1; }
        }
        portSockets.clear();
        
        signalSocket = -1;
        subscribeSocket = -1;
        querySocket = -1;
        responserSocket = -1;

        // 等待所有线程结束
        if (signalThread.joinable()) signalThread.join();
        if (subscribeThread.joinable()) subscribeThread.join();
        if (queryThread.joinable()) queryThread.join();
        if (responserThread.joinable()) responserThread.join();

        // 清理所有客户端连接
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            subscribers.clear();
            responsers.clear();
        }

        std::cout << "[KnotLinkServer] All servers stopped" << std::endl;
    }

    bool isRunning() const { return running; }

    // 发送信号到订阅者 (内部调用)
    void broadcastSignal(const std::string& channelKey, const std::string& data) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        
        auto it = subscribers.find(channelKey);
        if (it != subscribers.end()) {
            for (auto& client : it->second) {
                if (client && client->socket >= 0) {
                    send(client->socket, data.c_str(), data.size(), MSG_NOSIGNAL);
                }
            }
        }
    }

    // 设置问题处理器 (用于处理来自 OpenSocketQuerier 的查询)
    void setQuestionHandler(const std::string& channelKey, 
                           std::function<std::string(const std::string&)> handler) {
        std::lock_guard<std::mutex> lock(handlersMutex);
        questionHandlers[channelKey] = handler;
    }

private:
    KnotLinkServer() : running(false), 
                       signalSocket(-1), subscribeSocket(-1),
                       querySocket(-1), responserSocket(-1) {}

    ~KnotLinkServer() {
        stop();
    }

    std::atomic<bool> running;
    
    // 服务端sockets
    int signalSocket;      // 端口 6370 - 信号输入
    int subscribeSocket;   // 端口 6372 - 订阅者
    int querySocket;       // 端口 6376 - 查询
    int responserSocket;   // 端口 6378 - 响应者

    // 线程
    std::thread signalThread;
    std::thread subscribeThread;
    std::thread queryThread;
    std::thread responserThread;

    // 客户端管理
    std::mutex clientsMutex;
    std::map<std::string, std::vector<std::shared_ptr<KLClientConnection>>> subscribers;
    std::map<std::string, std::shared_ptr<KLClientConnection>> responsers;

    // 问题处理器
    std::mutex handlersMutex;
    std::map<std::string, std::function<std::string(const std::string&)>> questionHandlers;

    // 待处理的查询 (用于跨连接的请求-响应匹配)
    std::mutex pendingMutex;
    std::map<std::string, int> pendingQueries; // questionID -> querierSocket

    // 存储每个端口的 IPv4 和 IPv6 sockets
    std::map<uint16_t, std::pair<int, int>> portSockets; // port -> (ipv4_sock, ipv6_sock)

    // 创建 IPv4 监听 socket
    int createIPv4ListenSocket(uint16_t port) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            std::cerr << "[KnotLinkServer] Failed to create IPv4 socket for port " << port << std::endl;
            return -1;
        }

        // 允许地址重用
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 只监听 127.0.0.1
        addr.sin_port = htons(port);

        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[KnotLinkServer] Failed to bind IPv4 to port " << port << ": " << strerror(errno) << std::endl;
            close(sock);
            return -1;
        }

        if (listen(sock, 10) < 0) {
            std::cerr << "[KnotLinkServer] Failed to listen on IPv4 port " << port << std::endl;
            close(sock);
            return -1;
        }

        // 设置非阻塞
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        return sock;
    }

    // 创建 IPv6 监听 socket (macOS 上 Java 可能使用 IPv6 连接 localhost)
    int createIPv6ListenSocket(uint16_t port) {
        int sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            std::cerr << "[KnotLinkServer] Failed to create IPv6 socket for port " << port << std::endl;
            return -1;
        }

        // 允许地址重用
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        // 仅限 IPv6，不使用双栈
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_loopback; // 只监听 ::1
        addr.sin6_port = htons(port);

        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[KnotLinkServer] Failed to bind IPv6 to port " << port << ": " << strerror(errno) << std::endl;
            close(sock);
            return -1;
        }

        if (listen(sock, 10) < 0) {
            std::cerr << "[KnotLinkServer] Failed to listen on IPv6 port " << port << std::endl;
            close(sock);
            return -1;
        }

        // 设置非阻塞
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        return sock;
    }

    // 创建双栈监听 socket (IPv4 + IPv6)
    // 返回 IPv4 socket fd，同时创建并存储 IPv6 socket
    int createListenSocket(uint16_t port) {
        int ipv4Sock = createIPv4ListenSocket(port);
        int ipv6Sock = createIPv6ListenSocket(port);
        
        int socketCount = 0;
        if (ipv4Sock >= 0) socketCount++;
        if (ipv6Sock >= 0) socketCount++;
        
        if (socketCount == 0) {
            std::cerr << "[KnotLinkServer] Failed to create any socket for port " << port << std::endl;
            return -1;
        }

        // 存储 socket 对
        portSockets[port] = {ipv4Sock, ipv6Sock};

        std::cout << "[KnotLinkServer] Listening on port " << port 
                  << " (" << socketCount << " socket(s))" << std::endl;
        
        // 返回 IPv4 socket (或者 IPv6 如果 IPv4 创建失败)
        return ipv4Sock >= 0 ? ipv4Sock : ipv6Sock;
    }

    // 辅助函数：在两个 socket 上执行 poll 并接受连接
    // 返回新接受的 client socket，如果没有连接则返回 -1
    int acceptFromDualStack(uint16_t port, int timeoutMs = 100) {
        auto it = portSockets.find(port);
        if (it == portSockets.end()) return -1;
        
        int ipv4Sock = it->second.first;
        int ipv6Sock = it->second.second;
        
        // 构建 pollfd 数组
        std::vector<pollfd> pfds;
        if (ipv4Sock >= 0) {
            pfds.push_back({ipv4Sock, POLLIN, 0});
        }
        if (ipv6Sock >= 0) {
            pfds.push_back({ipv6Sock, POLLIN, 0});
        }
        
        if (pfds.empty()) return -1;
        
        if (poll(pfds.data(), pfds.size(), timeoutMs) > 0) {
            for (auto& pfd : pfds) {
                if (pfd.revents & POLLIN) {
                    sockaddr_storage clientAddr{};
                    socklen_t addrLen = sizeof(clientAddr);
                    int clientSock = accept(pfd.fd, (sockaddr*)&clientAddr, &addrLen);
                    if (clientSock >= 0) {
                        return clientSock;
                    }
                }
            }
        }
        return -1;
    }

    // 信号服务器 - 端口 6370
    // 接收来自 SignalSender 的信号并转发给订阅者
    void runSignalServer() {
        signalSocket = createListenSocket(6370);
        if (signalSocket < 0) return;

        while (running) {
            int clientSock = acceptFromDualStack(6370);
            if (clientSock >= 0) {
                // 处理信号连接 (短连接)
                std::thread([this, clientSock]() {
                    handleSignalConnection(clientSock);
                }).detach();
            }
        }
    }

    void handleSignalConnection(int clientSock) {
        char buffer[4096];
        std::string data;
        
        // 设置超时
        timeval tv{5, 0};
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (running) {
            ssize_t bytesRead = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                data += buffer;
                
                // 处理心跳
                if (data == "heartbeat") {
                    send(clientSock, "heartbeat_response", 18, MSG_NOSIGNAL);
                    data.clear();
                    continue;
                }
                
                // 解析信号: channelKey&*&payload
                size_t pos = data.find("&*&");
                if (pos != std::string::npos) {
                    std::string channelKey = data.substr(0, pos);
                    std::string payload = data.substr(pos + 3);
                    
                    std::cout << "[KnotLinkServer] Signal received for channel: " << channelKey << std::endl;
                    
                    // 广播给订阅者 (只发送 payload 部分)
                    broadcastSignal(channelKey, payload);
                    data.clear();
                }
            } else if (bytesRead == 0) {
                break; // 连接关闭
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
        }
        
        close(clientSock);
    }

    // 订阅服务器 - 端口 6372
    // 处理订阅者注册和长连接
    void runSubscribeServer() {
        subscribeSocket = createListenSocket(6372);
        if (subscribeSocket < 0) return;

        std::vector<std::shared_ptr<KLClientConnection>> connections;

        while (running) {
            // 检查新连接 (IPv4 + IPv6)
            int clientSock = acceptFromDualStack(6372, 10);
            if (clientSock >= 0) {
                auto conn = std::make_shared<KLClientConnection>(clientSock);
                connections.push_back(conn);
                std::cout << "[KnotLinkServer] New subscriber connection" << std::endl;
            }

            // 处理现有连接
            for (auto it = connections.begin(); it != connections.end();) {
                auto& conn = *it;
                
                char buffer[1024];
                
                // 非阻塞读取
                int flags = fcntl(conn->socket, F_GETFL, 0);
                fcntl(conn->socket, F_SETFL, flags | O_NONBLOCK);
                
                ssize_t bytesRead = recv(conn->socket, buffer, sizeof(buffer) - 1, 0);
                
                if (bytesRead > 0) {
                    buffer[bytesRead] = '\0';
                    conn->receiveBuffer += buffer;
                    conn->lastActivity = std::chrono::steady_clock::now();
                    
                    // 处理心跳
                    if (conn->receiveBuffer == "heartbeat") {
                        send(conn->socket, "heartbeat_response", 18, MSG_NOSIGNAL);
                        conn->receiveBuffer.clear();
                    }
                    // 处理注册
                    else if (!conn->registered && conn->receiveBuffer.find("&*&") == std::string::npos) {
                        conn->channelKey = conn->receiveBuffer;
                        conn->registered = true;
                        conn->receiveBuffer.clear();
                        
                        std::lock_guard<std::mutex> lock(clientsMutex);
                        subscribers[conn->channelKey].push_back(conn);
                        std::cout << "[KnotLinkServer] Subscriber registered: " << conn->channelKey << std::endl;
                    }
                } else if (bytesRead == 0) {
                    // 连接关闭
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    if (!conn->channelKey.empty()) {
                        auto& vec = subscribers[conn->channelKey];
                        vec.erase(std::remove(vec.begin(), vec.end(), conn), vec.end());
                    }
                    it = connections.erase(it);
                    continue;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    // 错误
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    if (!conn->channelKey.empty()) {
                        auto& vec = subscribers[conn->channelKey];
                        vec.erase(std::remove(vec.begin(), vec.end(), conn), vec.end());
                    }
                    it = connections.erase(it);
                    continue;
                }
                
                ++it;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // 查询服务器 - 端口 6376
    // 处理同步查询请求
    void runQueryServer() {
        querySocket = createListenSocket(6376);
        if (querySocket < 0) return;

        while (running) {
            int clientSock = acceptFromDualStack(6376);
            if (clientSock >= 0) {
                // 每个查询在独立线程中处理
                std::thread([this, clientSock]() {
                    handleQueryConnection(clientSock);
                }).detach();
            }
        }
    }

    void handleQueryConnection(int clientSock) {
        char buffer[4096];
        std::string data;
        
        // 将 socket 设置为阻塞模式（accept 继承了监听 socket 的非阻塞属性）
        int flags = fcntl(clientSock, F_GETFL, 0);
        fcntl(clientSock, F_SETFL, flags & ~O_NONBLOCK);
        
        // 设置超时
        timeval tv{5, 0};
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ssize_t bytesRead = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
        
        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            data = buffer;
            
            // 解析查询: channelKey&*&question
            size_t pos = data.find("&*&");
            if (pos != std::string::npos) {
                std::string channelKey = data.substr(0, pos);
                std::string question = data.substr(pos + 3);
                
                std::cout << "[KnotLinkServer] Query received for channel: " << channelKey << std::endl;
                
                std::string response = "ERROR:NO_HANDLER";
                
                // 首先检查本地处理器
                {
                    std::lock_guard<std::mutex> lock(handlersMutex);
                    auto it = questionHandlers.find(channelKey);
                    if (it != questionHandlers.end()) {
                        try {
                            response = it->second(question);
                        } catch (const std::exception& e) {
                            response = std::string("ERROR:") + e.what();
                        }
                    }
                }
                
                // 如果没有本地处理器，检查是否有远程响应者
                if (response == "ERROR:NO_HANDLER") {
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    auto it = responsers.find(channelKey);
                    if (it != responsers.end() && it->second && it->second->socket >= 0) {
                        // 生成问题ID
                        static int questionCounter = 0;
                        std::string questionID = "Q" + std::to_string(++questionCounter);
                        
                        // 发送给响应者
                        std::string packet = questionID + "&*&" + question;
                        send(it->second->socket, packet.c_str(), packet.size(), MSG_NOSIGNAL);
                        
                        // 等待响应 (简化实现，实际应该用更复杂的机制)
                        char respBuffer[4096];
                        timeval respTv{5, 0};
                        setsockopt(it->second->socket, SOL_SOCKET, SO_RCVTIMEO, &respTv, sizeof(respTv));
                        
                        ssize_t respRead = recv(it->second->socket, respBuffer, sizeof(respBuffer) - 1, 0);
                        if (respRead > 0) {
                            respBuffer[respRead] = '\0';
                            std::string respData(respBuffer);
                            size_t respPos = respData.find("&*&");
                            if (respPos != std::string::npos) {
                                response = respData.substr(respPos + 3);
                            }
                        }
                    }
                }
                
                // 发送响应
                send(clientSock, response.c_str(), response.size(), MSG_NOSIGNAL);
            }
        }
        
        // 优雅关闭连接：先 shutdown 写端，让客户端有机会读取响应
        shutdown(clientSock, SHUT_WR);
        
        // 等待客户端关闭（或超时），避免 TIME_WAIT 状态堆积
        char discard[64];
        while (recv(clientSock, discard, sizeof(discard), 0) > 0) {}
        
        close(clientSock);
    }

    // 响应者服务器 - 端口 6378
    // 处理响应者注册
    void runResponserServer() {
        responserSocket = createListenSocket(6378);
        if (responserSocket < 0) return;

        std::vector<std::shared_ptr<KLClientConnection>> connections;

        while (running) {
            // 检查新连接 (IPv4 + IPv6)
            int clientSock = acceptFromDualStack(6378, 10);
            if (clientSock >= 0) {
                auto conn = std::make_shared<KLClientConnection>(clientSock);
                connections.push_back(conn);
                std::cout << "[KnotLinkServer] New responser connection" << std::endl;
            }

            // 处理现有连接
            for (auto it = connections.begin(); it != connections.end();) {
                auto& conn = *it;
                
                char buffer[4096];
                
                // 非阻塞读取
                int flags = fcntl(conn->socket, F_GETFL, 0);
                fcntl(conn->socket, F_SETFL, flags | O_NONBLOCK);
                
                ssize_t bytesRead = recv(conn->socket, buffer, sizeof(buffer) - 1, 0);
                
                if (bytesRead > 0) {
                    buffer[bytesRead] = '\0';
                    conn->receiveBuffer += buffer;
                    conn->lastActivity = std::chrono::steady_clock::now();
                    
                    // 处理心跳
                    if (conn->receiveBuffer == "heartbeat") {
                        send(conn->socket, "heartbeat_response", 18, MSG_NOSIGNAL);
                        conn->receiveBuffer.clear();
                    }
                    // 处理注册 (格式: appID-socketID)
                    else if (!conn->registered && conn->receiveBuffer.find("&*&") == std::string::npos) {
                        conn->channelKey = conn->receiveBuffer;
                        conn->registered = true;
                        conn->receiveBuffer.clear();
                        
                        std::lock_guard<std::mutex> lock(clientsMutex);
                        responsers[conn->channelKey] = conn;
                        
                        // 发送确认
                        send(conn->socket, conn->channelKey.c_str(), conn->channelKey.size(), MSG_NOSIGNAL);
                        
                        std::cout << "[KnotLinkServer] Responser registered: " << conn->channelKey << std::endl;
                    }
                    // 处理响应 (格式: questionID&*&reply)
                    else if (conn->registered) {
                        // 响应会在 handleQueryConnection 中被接收
                        conn->receiveBuffer.clear();
                    }
                } else if (bytesRead == 0) {
                    // 连接关闭
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    if (!conn->channelKey.empty()) {
                        responsers.erase(conn->channelKey);
                    }
                    it = connections.erase(it);
                    continue;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    // 错误
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    if (!conn->channelKey.empty()) {
                        responsers.erase(conn->channelKey);
                    }
                    it = connections.erase(it);
                    continue;
                }
                
                ++it;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

#endif // KNOTLINK_SERVER_HPP
