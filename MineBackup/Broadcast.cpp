#include "Broadcast.h"
#include "AppState.h"

extern std::string CURRENT_VERSION;

SignalSender* g_signalSender = nullptr;
OpenSocketResponser* g_commandResponser = nullptr;

void BroadcastEvent(const std::string& eventPayload) {
    if (g_signalSender) {
        g_signalSender->emitt(eventPayload);
    }
}

bool PerformModHandshake(const std::string& action, const std::string& worldName, int timeoutMs) {
    // 重置握手相关状态
    auto& mod = g_appState.knotLinkMod;
    mod.resetForOperation();
    mod.modDetected = false;
    mod.versionCompatible = false;
    mod.modVersion.clear();

    // 构建握手消息: 包含主程序版本号、操作类型、世界名称
    std::string handshakePayload = "event=handshake;version=" + CURRENT_VERSION +
        ";action=" + action +
        ";world=" + worldName +
        ";min_mod_version=" + std::string(KnotLinkModInfo::MIN_MOD_VERSION);

    BroadcastEvent(handshakePayload);

    // 等待模组响应
    bool received = mod.waitForFlag(&KnotLinkModInfo::handshakeReceived,
                                     std::chrono::milliseconds(timeoutMs));

    if (!received) {
        // 超时未收到响应 — 模组未安装或未运行
        mod.modDetected = false;
        mod.versionCompatible = false;
        return false;
    }

    // 收到响应，检查版本兼容性
    return mod.modDetected.load() && mod.versionCompatible.load();
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