#pragma once
#ifndef _BROADCAST_H
#define _BROADCAST_H
#include "KnotLink/SignalSender.hpp"
#include "KnotLink/OpenSocketResponser.hpp"
#include <iostream>
extern SignalSender* g_signalSender;
extern OpenSocketResponser* g_commandResponser;
void BroadcastEvent(const std::string& eventPayload);
#endif // BROADCAST_H