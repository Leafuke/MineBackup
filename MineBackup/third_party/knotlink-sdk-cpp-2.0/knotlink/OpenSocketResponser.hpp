/*
 * KnotLink SDK - C++
 * Copyright (c) 2024-2026 KnotLink Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SOCKET_RESPONSER_HPP
#define OPEN_SOCKET_RESPONSER_HPP

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include "TcpClient.hpp"

namespace knotlink {

class OpenSocketResponser {
public:
    OpenSocketResponser(std::string appID, std::string openSocketID)
        : client_(std::make_unique<TcpClient>()),
          appID_(std::move(appID)),
          openSocketID_(std::move(openSocketID)) {
        client_->setOnDataReceivedCallback(
            [this](const std::string& data) { handleReceivedData(data); });
        if (!client_->connectToServer("127.0.0.1", 6378)) {
            throw std::runtime_error("Unable to connect to KnotLink OpenSocket responder");
        }
        registerChannel();
    }

    ~OpenSocketResponser() {
        client_->stop();
    }

    void setQuestionHandler(std::function<std::string(const std::string&)> handler) {
        onQuestionHandler_ = std::move(handler);
    }

private:
    std::unique_ptr<TcpClient> client_;
    std::string appID_;
    std::string openSocketID_;
    std::function<std::string(const std::string&)> onQuestionHandler_;

    void registerChannel() {
        if (!client_->sendData(appID_ + "-" + openSocketID_)) {
            throw std::runtime_error("Unable to register KnotLink OpenSocket responder");
        }
    }

    void handleReceivedData(const std::string& data) {
        const auto separator = data.find("&*&");
        if (separator == std::string::npos || !onQuestionHandler_) {
            return;
        }
        const std::string questionID = data.substr(0, separator);
        const std::string payload = data.substr(separator + 3);
        client_->sendData(questionID + "&*&" + onQuestionHandler_(payload));
    }
};

} // namespace knotlink

#endif // OPEN_SOCKET_RESPONSER_HPP
