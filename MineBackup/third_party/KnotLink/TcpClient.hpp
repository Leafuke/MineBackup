#ifndef TCP_CLIENT_HPP
#define TCP_CLIENT_HPP

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <functional>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

class TcpClient {
public:
	TcpClient() : tcpSocket(INVALID_SOCKET), running(false) {
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
			std::cerr << "WSAStartup failed." << std::endl;
			exit(1);
		}
	}
	
	~TcpClient() {
		stopHeartbeat();
		if (readThread.joinable()&&tcpSocket != INVALID_SOCKET) {
			closesocket(tcpSocket); // 打断 recv()
			readThread.join();
		}
		WSACleanup();
	}
	
	bool connectToServer(const std::string& ip, uint16_t port) {
		tcpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (tcpSocket == INVALID_SOCKET) {
			std::cerr << "Failed to create socket." << std::endl;
			return false;
		}
		
		sockaddr_in serverAddr;
		serverAddr.sin_family = AF_INET;
		serverAddr.sin_port = htons(port);
		inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr);
		
		if (connect(tcpSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
			std::cerr << "Failed to connect to server." << std::endl;
			closesocket(tcpSocket);
			return false;
		}
		
		readThread = std::thread(&TcpClient::readData, this);
		readThread.detach();
		
		startHeartbeat();
		return true;
	}
	
	void sendData(const std::string& data) {
		if (tcpSocket != INVALID_SOCKET) {
			int bytesSent = send(tcpSocket, data.c_str(), data.size(), 0);
			if (bytesSent == SOCKET_ERROR) {
				handleError(WSAGetLastError());
			}
		} else {
			std::cerr << "Socket is not connected." << std::endl;
		}
	}
	
	void startHeartbeat() {
		running = true;
		heartBeatThread = std::thread(&TcpClient::sendHeartbeat, this);
	}
	
	void stopHeartbeat() {
		running = false;
		if (heartBeatThread.joinable()) {
			heartBeatThread.join();
		}
	}
	
	void setOnDataReceivedCallback(const std::function<void(const std::string&)>& callback) {
		onDataReceivedCallback = callback;
	}
	
	bool running;
	
private:
	SOCKET tcpSocket;
	std::thread heartBeatThread;
	std::thread readThread;
	std::string heartbeatMessage = "heartbeat";
	std::string heartbeatResponse = "heartbeat_response";
	std::function<void(const std::string&)> onDataReceivedCallback;
	
	void sendHeartbeat() {
		std::this_thread::sleep_for(std::chrono::seconds(180)); // 3 minutes
		while (running) {
			sendData(heartbeatMessage);
			std::this_thread::sleep_for(std::chrono::seconds(180)); // 3 minutes
		}
	}
	
	void handleError(int socketError) {
		std::cerr << "Socket error: " << socketError << std::endl;
	}
	
	void readData() {
		char buffer[1024];
		while (true) {
			int bytesRead = recv(tcpSocket, buffer, sizeof(buffer), 0);
			if (bytesRead > 0) {
				buffer[bytesRead] = '\0';
				std::string receivedData(buffer);
				std::cout << "Received data: " << receivedData << std::endl;
				if (receivedData == heartbeatResponse) {
					continue;
				}
				// Handle received data
				if (onDataReceivedCallback) {
					onDataReceivedCallback(receivedData);
				}
			} else if (bytesRead == 0) {
				std::cout << "Server disconnected." << std::endl;
				break;
			} else {
				handleError(WSAGetLastError());
				break;
			}
		}
	}
};

#endif // TCP_CLIENT_HPP
