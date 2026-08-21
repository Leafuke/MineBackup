#include "CliSignalHandler.h"

#include <atomic>
#include <chrono>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

using namespace std;

namespace {

#ifdef _WIN32
atomic<unsigned long> g_signalCount{0};

BOOL WINAPI ConsoleHandler(DWORD type) {
	if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT
		&& type != CTRL_CLOSE_EVENT && type != CTRL_SHUTDOWN_EVENT) return FALSE;
	if (g_signalCount.fetch_add(1, memory_order_relaxed) >= 1) {
		TerminateProcess(GetCurrentProcess(), 9);
	}
	return TRUE;
}
#else
volatile sig_atomic_t g_signalCount = 0;

void PosixHandler(int) {
	if (g_signalCount >= 1) _exit(9);
	g_signalCount = 1;
}
#endif

unsigned long SignalCount() {
#ifdef _WIN32
	return g_signalCount.load(memory_order_relaxed);
#else
	return static_cast<unsigned long>(g_signalCount);
#endif
}

} // namespace

CliSignalHandler::CliSignalHandler() {
#ifdef _WIN32
	g_signalCount.store(0, memory_order_relaxed);
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
	g_signalCount = 0;
	struct sigaction action {};
	action.sa_handler = PosixHandler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGINT, &action, nullptr);
	sigaction(SIGTERM, &action, nullptr);
#endif
	monitor_ = jthread([this](stop_token token) {
		while (!token.stop_requested() && SignalCount() == 0) {
			this_thread::sleep_for(chrono::milliseconds(10));
		}
		if (SignalCount() != 0) stopSource_.request_stop();
	});
}

CliSignalHandler::~CliSignalHandler() {
	monitor_.request_stop();
	if (monitor_.joinable()) monitor_.join();
#ifdef _WIN32
	SetConsoleCtrlHandler(ConsoleHandler, FALSE);
#else
	struct sigaction action {};
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, nullptr);
	sigaction(SIGTERM, &action, nullptr);
#endif
}

bool CliSignalHandler::WasInterrupted() const {
	return SignalCount() != 0;
}
