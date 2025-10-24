#ifndef SIGNALSENDER_HPP
#define SIGNALSENDER_HPP
#pragma once

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

#endif // SIGNALSENDER_HPP
