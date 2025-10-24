#include "Broadcast.h"
SignalSender* g_signalSender = nullptr;
OpenSocketResponser* g_commandResponser = nullptr;
void BroadcastEvent(const std::string& eventPayload) {
	if (g_signalSender) {
		g_signalSender->emitt(eventPayload);
	}
}