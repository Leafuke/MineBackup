#ifndef SIGNAL_SENDER_POSIX_HPP
#define SIGNAL_SENDER_POSIX_HPP

// POSIX版本的 SignalSender - 用于 Linux 和 MacOS
// 当 KnotLink 服务端集成在 MineBackup 中时，可以直接调用服务端的广播功能

#include <string>
#include "KnotLinkServer.hpp"

class SignalSender {
public:
    SignalSender() {
        init();
    }

    SignalSender(std::string APPID, std::string SignalID) 
        : appID(APPID), signalID(SignalID) {
        init();
    }

    ~SignalSender() {
        // 不需要特别清理，因为使用的是服务端的内部功能
    }

    void setConfig(std::string APPID, std::string SignalID) {
        appID = APPID;
        signalID = SignalID;
    }

    void emitt(std::string data) {
        emitt(appID, signalID, data);
    }

    void emitt(std::string APPID, std::string SignalID, std::string data) {
        std::string channelKey = APPID + "-" + SignalID;
        
        // 直接调用集成服务端的广播功能
        KnotLinkServer::getInstance().broadcastSignal(channelKey, data);
        
        std::cout << "[SignalSender] Emitted signal to " << channelKey << ": " << data << std::endl;
    }

private:
    std::string appID;
    std::string signalID;

    void init() {
        // 确保 KnotLink 服务端已启动
        if (!KnotLinkServer::getInstance().isRunning()) {
            KnotLinkServer::getInstance().start();
        }
    }
};

#endif // SIGNAL_SENDER_POSIX_HPP
