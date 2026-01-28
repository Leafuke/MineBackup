#pragma once
#ifndef _BROADCAST_H
#define _BROADCAST_H

#include <iostream>
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

#endif // BROADCAST_H