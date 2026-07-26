/*
 * KnotLink SDK - C++
 * Copyright (c) 2024-2026 KnotLink Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SOCKET_RESPONSER_HPP
#define OPEN_SOCKET_RESPONSER_HPP

#include <string>
#include <functional>
#include "TcpClient.hpp"

namespace knotlink {

class OpenSocketResponser {
public:
    OpenSocketResponser(const std::string& APPID, const std::string& OpenSocketID)
        : appID(APPID), openSocketID(OpenSocketID) {
        KLresponser = new TcpClient();
        KLresponser->connectToServer("127.0.0.1", 6378);
        KLresponser->setOnDataReceivedCallback(
            std::bind(&OpenSocketResponser::handleReceivedData, this, std::placeholders::_1));
        while (!KLresponser->running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

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

    void registerChannel() {
        std::string key = appID + "-" + openSocketID;
        KLresponser->sendData(key);
    }

    void handleReceivedData(const std::string& data) {
        if (data == "heartbeat_response") return;

        auto pos = data.find("&*&");
        if (pos == std::string::npos) return;

        std::string questionID = data.substr(0, pos);
        std::string payload    = data.substr(pos + 3);

        if (onQuestionHandler) {
            std::string reply = onQuestionHandler(payload);
            std::string response = questionID + "&*&" + reply;
            KLresponser->sendData(response);
        }
    }
};

} // namespace knotlink

#endif // OPEN_SOCKET_RESPONSER_HPP
