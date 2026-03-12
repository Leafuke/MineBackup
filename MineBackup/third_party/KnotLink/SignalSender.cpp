#include "SignalSender.hpp"
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