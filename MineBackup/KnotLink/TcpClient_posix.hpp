#ifndef TCP_CLIENT_POSIX_HPP
#define TCP_CLIENT_POSIX_HPP

// POSIX版本的TcpClient - 用于 Linux 和 MacOS
// 保持与Windows版本相同的接口

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <functional>
#include <atomic>
#include <mutex>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

class TcpClient {
public:
	TcpClient() : tcpSocket(-1), running(false), connected(false) {
	}
	
	~TcpClient() {
		stopHeartbeat();
		disconnect();
	}
	
	bool connectToServer(const std::string& ip, uint16_t port) {
		tcpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (tcpSocket < 0) {
			std::cerr << "[TcpClient] Failed to create socket: " << strerror(errno) << std::endl;
			return false;
		}
		
		sockaddr_in serverAddr{};
		serverAddr.sin_family = AF_INET;
		serverAddr.sin_port = htons(port);
		
		if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) <= 0) {
			std::cerr << "[TcpClient] Invalid address: " << ip << std::endl;
			close(tcpSocket);
			tcpSocket = -1;
			return false;
		}
		
		if (connect(tcpSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
			std::cerr << "[TcpClient] Failed to connect to server " << ip << ":" << port << " - " << strerror(errno) << std::endl;
			close(tcpSocket);
			tcpSocket = -1;
			return false;
		}
		
		connected = true;
		running = true;
		
		// 启动读取线程
		readThread = std::thread(&TcpClient::readData, this);
		
		// 启动心跳
		startHeartbeat();
		
		std::cout << "[TcpClient] Connected to " << ip << ":" << port << std::endl;
		return true;
	}
	
	void disconnect() {
		running = false;
		connected = false;
		
		if (tcpSocket >= 0) {
			// 关闭socket会打断recv()
			shutdown(tcpSocket, SHUT_RDWR);
			close(tcpSocket);
			tcpSocket = -1;
		}
		
		if (readThread.joinable()) {
			readThread.join();
		}
	}
	
	void sendData(const std::string& data) {
		std::lock_guard<std::mutex> lock(sendMutex);
		if (tcpSocket >= 0 && connected) {
			ssize_t bytesSent = send(tcpSocket, data.c_str(), data.size(), MSG_NOSIGNAL);
			if (bytesSent < 0) {
				handleError(errno);
			}
		} else {
			std::cerr << "[TcpClient] Socket is not connected." << std::endl;
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
	
	bool isConnected() const {
		return connected && tcpSocket >= 0;
	}
	
	std::atomic<bool> running;
	
private:
	int tcpSocket;
	std::atomic<bool> connected;
	std::thread heartBeatThread;
	std::thread readThread;
	std::mutex sendMutex;
	std::string heartbeatMessage = "heartbeat";
	std::string heartbeatResponse = "heartbeat_response";
	std::function<void(const std::string&)> onDataReceivedCallback;
	
	void sendHeartbeat() {
		// 等待3分钟后发送第一次心跳
		for (int i = 0; i < 180 && running; ++i) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		while (running) {
			sendData(heartbeatMessage);
			// 等待3分钟
			for (int i = 0; i < 180 && running; ++i) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
			}
		}
	}
	
	void handleError(int socketError) {
		std::cerr << "[TcpClient] Socket error: " << strerror(socketError) << " (" << socketError << ")" << std::endl;
	}
	
	void readData() {
		char buffer[1024];
		while (running && tcpSocket >= 0) {
			ssize_t bytesRead = recv(tcpSocket, buffer, sizeof(buffer) - 1, 0);
			if (bytesRead > 0) {
				buffer[bytesRead] = '\0';
				std::string receivedData(buffer, bytesRead);
				std::cout << "[TcpClient] Received data: " << receivedData << std::endl;
				if (receivedData == heartbeatResponse) {
					continue;
				}
				// 处理接收到的数据
				if (onDataReceivedCallback) {
					onDataReceivedCallback(receivedData);
				}
			} else if (bytesRead == 0) {
				std::cout << "[TcpClient] Server disconnected." << std::endl;
				connected = false;
				break;
			} else {
				if (errno == EINTR) continue;
				if (running) {
					handleError(errno);
				}
				break;
			}
		}
		running = false;
	}
};

#endif // TCP_CLIENT_POSIX_HPP
