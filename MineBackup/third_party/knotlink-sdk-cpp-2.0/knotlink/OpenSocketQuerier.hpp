/*
 * KnotLink SDK - C++
 * Copyright (c) 2024-2026 KnotLink Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SOCKET_QUERIER_HPP
#define OPEN_SOCKET_QUERIER_HPP

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include "TcpClient.hpp"

namespace knotlink {

class OpenSocketQuerier {
public:
    OpenSocketQuerier() : client_(std::make_unique<TcpClient>()) {
        client_->setOnDataReceivedCallback(
            [this](const std::string& data) { handleReceivedData(data); });
        if (!client_->connectToServer("127.0.0.1", 6376)) {
            throw std::runtime_error("Unable to connect to KnotLink OpenSocket querier");
        }
    }

    ~OpenSocketQuerier() {
        client_->stop();
    }

    void setConfig(const std::string& appID, const std::string& openSocketID) {
        appID_ = appID;
        openSocketID_ = openSocketID;
    }

    std::string query_l(const std::string& question, int timeoutMs = -1) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            answerReady_ = false;
            answer_.clear();
        }
        if (!client_->sendData(appID_ + "-" + openSocketID_ + "&*&" + question)) {
            throw std::runtime_error("Unable to send KnotLink OpenSocket query");
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (timeoutMs < 0) {
            condition_.wait(lock, [this] { return answerReady_ || !client_->isRunning(); });
        } else if (!condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
                       return answerReady_ || !client_->isRunning();
                   })) {
            throw std::runtime_error(
                "query_l timed out after " + std::to_string(timeoutMs) + "ms");
        }
        if (!answerReady_) {
            throw std::runtime_error("KnotLink OpenSocket connection closed");
        }
        return answer_;
    }

private:
    std::unique_ptr<TcpClient> client_;
    std::string appID_;
    std::string openSocketID_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::string answer_;
    bool answerReady_ = false;

    void handleReceivedData(const std::string& data) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            answer_ = data;
            answerReady_ = true;
        }
        condition_.notify_all();
    }
};

} // namespace knotlink

#endif // OPEN_SOCKET_QUERIER_HPP
