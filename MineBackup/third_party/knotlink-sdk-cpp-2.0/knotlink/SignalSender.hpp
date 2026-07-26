/*
 * KnotLink SDK - C++
 * Copyright (c) 2024-2026 KnotLink Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SIGNALSENDER_HPP
#define SIGNALSENDER_HPP

#include <string>
#include "TcpClient.hpp"

namespace knotlink {

class SignalSender {
public:
    SignalSender() {
        init();
    }

    SignalSender(std::string APPID, std::string SignalID)
        : appID(std::move(APPID)), signalID(std::move(SignalID)) {
        init();
    }

    ~SignalSender() {
        if (KLsender) {
            KLsender->stopHeartbeat();
            delete KLsender;
            KLsender = nullptr;
        }
    }

    void setConfig(std::string APPID, std::string SignalID) {
        appID = std::move(APPID);
        signalID = std::move(SignalID);
    }

    void emitt(std::string data) {
        emitt(appID, signalID, std::move(data));
    }

    void emitt(std::string APPID, std::string SignalID, std::string data) {
        std::string s_data = APPID + "-" + SignalID + "&*&" + data;
        KLsender->sendData(s_data);
    }

private:
    TcpClient* KLsender = nullptr;
    std::string appID;
    std::string signalID;

    void init() {
        KLsender = new TcpClient();
        KLsender->connectToServer("127.0.0.1", 6370);
    }
};

} // namespace knotlink

#endif // SIGNALSENDER_HPP
