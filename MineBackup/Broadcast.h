#pragma once
#include "KnotLink/SignalSender.hpp"
#include "KnotLink/OpenSocketResponser.hpp"
#include <iostream>
extern SignalSender* g_signalSender;
extern OpenSocketResponser* g_commandResponser;
void BroadcastEvent(const std::string& eventPayload);