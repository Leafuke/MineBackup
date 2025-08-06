#ifndef SIGNALSENDER_HPP
#define SIGNALSENDER_HPP

#include <string>
#include "tcpclient.hpp"

class SignalSender {
public:
	SignalSender();
	~SignalSender();
	SignalSender(std::string APPID, std::string SignalID);
	void setConfig(std::string APPID, std::string SignalID);
	void emitt(std::string data);
	void emitt(std::string APPID, std::string SignalID, std::string data);
	
private:
	TcpClient* KLsender;
	std::string appID;
	std::string signalID;
	void init();
};

SignalSender::SignalSender() {
	init();
}

SignalSender::SignalSender(std::string APPID, std::string SignalID) : appID(APPID), signalID(SignalID) {
	init();
}


void SignalSender::init() {
	KLsender = new TcpClient();
	KLsender->connectToServer("127.0.0.1", 6370);
}

void SignalSender::setConfig(std::string APPID, std::string SignalID) {
	appID = APPID;
	signalID = SignalID;
}

void SignalSender::emitt(std::string data) {
	emitt(appID, signalID, data);
}

void SignalSender::emitt(std::string APPID, std::string SignalID, std::string data) {
	std::string s_key = APPID + "-" + SignalID;
	s_key += "&*&";
	// Create s_data by combining s_key and data
	std::string s_data = s_key + data;
	KLsender->sendData(s_data);
}

SignalSender::~SignalSender() {
	if (KLsender) {
		KLsender->stopHeartbeat();
		delete KLsender;
		KLsender = nullptr;
	}
}

#endif // SIGNALSENDER_HPP
