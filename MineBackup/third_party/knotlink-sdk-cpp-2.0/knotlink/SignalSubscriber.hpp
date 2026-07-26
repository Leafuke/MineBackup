/*
 * KnotLink SDK - C++
 * Copyright (c) 2024-2026 KnotLink Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SIGNAL_SUBSCRIBER_HPP
#define SIGNAL_SUBSCRIBER_HPP

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include "TcpClient.hpp"

namespace knotlink {

class SignalSubscriber {
public:
    SignalSubscriber(std::string appID, std::string signalID)
        : client_(std::make_unique<TcpClient>()),
          appID_(std::move(appID)),
          signalID_(std::move(signalID)) {
        client_->setOnDataReceivedCallback(
            [this](const std::string& data) { handleReceivedData(data); });
        if (!client_->connectToServer("127.0.0.1", 6372)) {
            throw std::runtime_error("Unable to connect to KnotLink signal subscriber");
        }
        subscribe(appID_, signalID_);
    }

    ~SignalSubscriber() {
        client_->stop();
    }

    bool subscribe(const std::string& appID, const std::string& signalID) {
        appID_ = appID;
        signalID_ = signalID;
        return client_->sendData(appID_ + "-" + signalID_);
    }

    void start() {}

    void stop() {
        client_->stop();
    }

    void setOnDataReceivedCallback(
        std::function<void(const std::string&)> callback) {
        onDataReceivedCallback_ = std::move(callback);
    }

private:
    std::unique_ptr<TcpClient> client_;
    std::string appID_;
    std::string signalID_;
    std::function<void(const std::string&)> onDataReceivedCallback_;

    void handleReceivedData(const std::string& data) {
        if (onDataReceivedCallback_) {
            onDataReceivedCallback_(data);
        }
    }
};

} // namespace knotlink

#endif // SIGNAL_SUBSCRIBER_HPP
