#include "Broadcast.h"

#ifdef _WIN32
SignalSender* g_signalSender = nullptr;
OpenSocketResponser* g_commandResponser = nullptr;
void BroadcastEvent(const std::string& eventPayload) {
	if (g_signalSender) {
		g_signalSender->emitt(eventPayload);
	}
}
#else

void BroadcastEvent(const std::string& eventPayload) {
	// No-op on non-Windows platforms
}

#endif