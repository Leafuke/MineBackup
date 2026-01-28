#include "Broadcast.h"

SignalSender* g_signalSender = nullptr;
OpenSocketResponser* g_commandResponser = nullptr;

void BroadcastEvent(const std::string& eventPayload) {
    if (g_signalSender) {
        g_signalSender->emitt(eventPayload);
    }
}

#ifndef _WIN32
// Linux 和 MacOS 专用的 KnotLink 初始化和清理函数
#include "KnotLink/KnotLinkServer.hpp"

void InitKnotLink() {
    KnotLinkServer::getInstance().start();
}

void CleanupKnotLink() {
    KnotLinkServer::getInstance().stop();
    
    if (g_commandResponser) {
        delete g_commandResponser;
        g_commandResponser = nullptr;
    }
    if (g_signalSender) {
        delete g_signalSender;
        g_signalSender = nullptr;
    }
}
#else
void InitKnotLink() {
    // Windows 上不需要启动服务端，使用外部 KnotLink 服务端
}

void CleanupKnotLink() {
    if (g_commandResponser) {
        g_commandResponser = nullptr;
    }
    if (g_signalSender) {
        g_signalSender = nullptr;
    }
}
#endif