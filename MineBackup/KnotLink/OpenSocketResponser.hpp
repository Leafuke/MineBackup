#ifndef OPEN_SOCKET_RESPONSER_HPP
#define OPEN_SOCKET_RESPONSER_HPP
#pragma once

#include <string>
#include <functional>
#include "TcpClient.hpp"

class OpenSocketResponser {
public:
	OpenSocketResponser(const std::string& APPID,
		const std::string& OpenSocketID)
	: appID(APPID), openSocketID(OpenSocketID), registered(false) {
		KLresponser = new TcpClient();
		KLresponser->connectToServer("127.0.0.1", 6378);
		KLresponser->setOnDataReceivedCallback(
			std::bind(&OpenSocketResponser::handleReceivedData, this, std::placeholders::_1));
		while (!KLresponser->running) { /* 等待连接完成 */ }
		
		registerChannel();
	}
	
	~OpenSocketResponser() {
		KLresponser->stopHeartbeat();
		delete KLresponser;
	}
	
	void setQuestionHandler(std::function<std::string(const std::string&)> handler) {
		onQuestionHandler = std::move(handler);
	}
	
private:
	TcpClient* KLresponser;
	std::string appID;
	std::string openSocketID;
	std::function<std::string(const std::string&)> onQuestionHandler;
	bool registered;          // 防止把注册回包当问题解析
	
	void registerChannel() {
		std::string key = appID + "-" + openSocketID;
		KLresponser->sendData(key);
	}
	
	void handleReceivedData(const std::string& data) {
		// 忽略心跳
		if (data == "heartbeat_response") return;
		
		// 忽略第一次注册回包（简单做法）
		if (!registered && data == (appID + "-" + openSocketID)) {
			registered = true;
			return;
		}
		
		// 正式问题格式：questionID&*&payload
		auto pos = data.find("&*&");
		if (pos == std::string::npos) return;
		
		std::string questionID = data.substr(0, pos);
		std::string payload    = data.substr(pos + 3);
		
		if (onQuestionHandler) {
			std::string reply = onQuestionHandler(payload);
			std::string response = questionID + "&*&" + reply;
			std::cout<<response<<std::endl;
			KLresponser->sendData(response);
		}
	}
};

#endif // OPEN_SOCKET_RESPONSER_HPP
