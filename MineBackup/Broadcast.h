#pragma once
#ifndef _BROADCAST_H
#define _BROADCAST_H

#include <iostream>
#ifdef _WIN32
#include "KnotLink/SignalSender.hpp"
#include "KnotLink/OpenSocketResponser.hpp"
extern SignalSender* g_signalSender;
extern OpenSocketResponser* g_commandResponser;
#endif
void BroadcastEvent(const std::string& eventPayload);

#endif // BROADCAST_H