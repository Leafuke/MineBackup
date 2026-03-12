#ifndef OPEN_SOCKET_RESPONSER_POSIX_HPP
#define OPEN_SOCKET_RESPONSER_POSIX_HPP

// POSIX版本的 OpenSocketResponser - 用于 Linux 和 MacOS
// 当 KnotLink 服务端集成在 MineBackup 中时，直接注册处理器到服务端

#include <string>
#include <functional>
#include "KnotLinkServer.hpp"

class OpenSocketResponser {
public:
    OpenSocketResponser(const std::string& APPID, const std::string& OpenSocketID)
        : appID(APPID), openSocketID(OpenSocketID) {
        
        // 确保 KnotLink 服务端已启动
        if (!KnotLinkServer::getInstance().isRunning()) {
            KnotLinkServer::getInstance().start();
        }
        
        channelKey = appID + "-" + openSocketID;
        std::cout << "[OpenSocketResponser] Registered for channel: " << channelKey << std::endl;
    }

    ~OpenSocketResponser() {
        // 清理时可以移除处理器，但目前实现保持简单
    }

    void setQuestionHandler(std::function<std::string(const std::string&)> handler) {
        onQuestionHandler = std::move(handler);
        
        // 将处理器注册到 KnotLink 服务端
        KnotLinkServer::getInstance().setQuestionHandler(channelKey, 
            [this](const std::string& question) -> std::string {
                if (onQuestionHandler) {
                    return onQuestionHandler(question);
                }
                return "ERROR:NO_HANDLER";
            }
        );
    }

private:
    std::string appID;
    std::string openSocketID;
    std::string channelKey;
    std::function<std::string(const std::string&)> onQuestionHandler;
};

#endif // OPEN_SOCKET_RESPONSER_POSIX_HPP
