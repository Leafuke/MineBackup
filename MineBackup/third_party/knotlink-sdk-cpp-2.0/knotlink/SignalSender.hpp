/*
 * KnotLink SDK - C++
 * Copyright (c) 2024-2026 KnotLink Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SIGNALSENDER_HPP
#define SIGNALSENDER_HPP

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include "TcpClient.hpp"

namespace knotlink {

class SignalSender {
public:
    SignalSender() {
        init();
    }

    SignalSender(std::string appID, std::string signalID)
        : appID_(std::move(appID)), signalID_(std::move(signalID)) {
        init();
    }

    ~SignalSender() = default;

    void setConfig(std::string appID, std::string signalID) {
        appID_ = std::move(appID);
        signalID_ = std::move(signalID);
    }

    bool emitt(std::string data) {
        return emitt(appID_, signalID_, std::move(data));
    }

    bool emitt(const std::string& appID, const std::string& signalID, std::string data) {
        return client_->sendData(appID + "-" + signalID + "&*&" + data);
    }

private:
    std::unique_ptr<TcpClient> client_;
    std::string appID_;
    std::string signalID_;

    void init() {
        client_ = std::make_unique<TcpClient>();
        if (!client_->connectToServer("127.0.0.1", 6370)) {
            throw std::runtime_error("Unable to connect to KnotLink signal service");
        }
    }
};

} // namespace knotlink

#endif // SIGNALSENDER_HPP
