/*
 * KnotLink SDK - C++
 * Copyright (c) 2024-2026 KnotLink Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SOCKET_QUERIER_HPP
#define OPEN_SOCKET_QUERIER_HPP

#include <string>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include "TcpClient.hpp"

namespace knotlink {

class OpenSocketQuerier {
public:
    OpenSocketQuerier() {
        KLquerier = new TcpClient();
        KLquerier->connectToServer("127.0.0.1", 6376);
        KLquerier->setOnDataReceivedCallback(
            std::bind(&OpenSocketQuerier::handleReceivedData, this, std::placeholders::_1));
        while (!KLquerier->running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~OpenSocketQuerier() {
        KLquerier->stopHeartbeat();
        delete KLquerier;
    }

    void setConfig(const std::string& APPID, const std::string& OpenSocketID) {
        appID        = APPID;
        openSocketID = OpenSocketID;
    }

    std::string query_l(const std::string& question, int timeoutMs = -1) {
        std::unique_lock<std::mutex> lock(mtx_);
        std::string qid = std::to_string(++questionCounter_);
        std::string packet = appID + "-" + openSocketID + "&*&" + question;
        KLquerier->sendData(packet);

        auto deadline = [&] {
            if (timeoutMs < 0) { cv_.wait(lock, [this, &qid] { return answers_.count(qid); }); return true; }
            return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                [this, &qid] { return answers_.count(qid); });
        };
        if (!deadline())
            throw std::runtime_error("query_l timed out after " + std::to_string(timeoutMs) + "ms");

        std::string ans = answers_[qid];
        answers_.erase(qid);
        return ans;
    }

private:
    TcpClient* KLquerier;
    std::string appID;
    std::string openSocketID;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::string> answers_;
    uint64_t questionCounter_ = 0;

    void handleReceivedData(const std::string& data) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string qid = std::to_string(questionCounter_);
        answers_[qid] = data;
        cv_.notify_all();
    }
};

} // namespace knotlink

#endif // OPEN_SOCKET_QUERIER_HPP
