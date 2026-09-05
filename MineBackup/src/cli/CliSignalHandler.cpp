#include "CliSignalHandler.h"
#include "ProcessRunner.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

using namespace std;

namespace {
atomic<unsigned long> g_signalCount{0};
static_assert(atomic<unsigned long>::is_always_lock_free,
    "Signal notifications require lock-free atomics");

bool NotifySignal() noexcept {
    unsigned long expected = 0;
    if (g_signalCount.compare_exchange_strong(expected, 1, memory_order_relaxed)) return false;
    g_signalCount.store(2, memory_order_relaxed);
    return true;
}

#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT
        && type != CTRL_CLOSE_EVENT && type != CTRL_SHUTDOWN_EVENT) return FALSE;
    if (NotifySignal()) TerminateProcess(GetCurrentProcess(), 9);
    return TRUE;
}
#else
void PosixHandler(int) { NotifySignal(); }
#endif

unsigned long SignalCount() { return g_signalCount.load(memory_order_relaxed); }
} // namespace

CliSignalHandler::CliSignalHandler() {
    g_signalCount.store(0, memory_order_relaxed);
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE))
        throw system_error(GetLastError(), system_category(), "Could not install console signal handler");
#else
    struct sigaction action {};
    action.sa_handler = PosixHandler;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    if (sigaction(SIGINT, &action, &previousInt_) != 0)
        throw system_error(errno, generic_category(), "Could not install SIGINT handler");
    if (sigaction(SIGTERM, &action, &previousTerm_) != 0) {
        const int error = errno;
        sigaction(SIGINT, &previousInt_, nullptr);
        throw system_error(error, generic_category(), "Could not install SIGTERM handler");
    }
#endif
    try {
#ifndef _WIN32
        forceMonitor_ = jthread([](stop_token token) {
            while (!token.stop_requested()) {
                if (SignalCount() >= 2) {
                    ProcessRunner::TerminateActiveProcess();
                    _exit(9);
                }
                this_thread::sleep_for(chrono::milliseconds(10));
            }
        });
#endif
        monitor_ = jthread([this](stop_token token) {
            while (!token.stop_requested() && SignalCount() == 0) {
                this_thread::sleep_for(chrono::milliseconds(10));
            }
            // request_stop runs callbacks synchronously; the force monitor
            // must remain independent of this thread and those callbacks.
            if (SignalCount() != 0) stopSource_.request_stop();
        });
    }
    catch (...) {
#ifdef _WIN32
        SetConsoleCtrlHandler(ConsoleHandler, FALSE);
#else
        forceMonitor_.request_stop();
        if (forceMonitor_.joinable()) forceMonitor_.join();
        sigaction(SIGINT, &previousInt_, nullptr);
        sigaction(SIGTERM, &previousTerm_, nullptr);
#endif
        throw;
    }
}

CliSignalHandler::~CliSignalHandler() {
    monitor_.request_stop();
    if (monitor_.joinable()) monitor_.join();
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
#else
    forceMonitor_.request_stop();
    if (forceMonitor_.joinable()) forceMonitor_.join();
    sigaction(SIGINT, &previousInt_, nullptr);
    sigaction(SIGTERM, &previousTerm_, nullptr);
#endif
}

bool CliSignalHandler::WasInterrupted() const { return SignalCount() != 0; }
