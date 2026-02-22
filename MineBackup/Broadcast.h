#pragma once
#ifndef _BROADCAST_H
#define _BROADCAST_H

#include <iostream>
#include <string>
#include <chrono>
#ifdef _WIN32
#include "KnotLink/SignalSender.hpp"
#include "KnotLink/OpenSocketResponser.hpp"
#else
#include "KnotLink/SignalSender_posix.hpp"
#include "KnotLink/OpenSocketResponser_posix.hpp"
#endif

extern SignalSender* g_signalSender;
extern OpenSocketResponser* g_commandResponser;

void BroadcastEvent(const std::string& eventPayload);
void InitKnotLink();
void CleanupKnotLink();

// KnotLink 联动模组握手检测
// 向模组发送握手消息，等待回应以判断模组是否存在及版本是否兼容
// action: "backup" 或 "restore"，worldName: 当前操作的世界名
// timeoutMs: 等待超时，默认100ms
// 返回: true 表示模组已检测到且版本兼容
bool PerformModHandshake(const std::string& action, const std::string& worldName, int timeoutMs = 100);

#endif // BROADCAST_H