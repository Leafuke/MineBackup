#pragma once

#include <stop_token>
#include <thread>
#ifndef _WIN32
#include <csignal>
#endif

class CliSignalHandler {
public:
	CliSignalHandler();
	~CliSignalHandler();
	CliSignalHandler(const CliSignalHandler&) = delete;
	CliSignalHandler& operator=(const CliSignalHandler&) = delete;

	std::stop_token Token() const { return stopSource_.get_token(); }
	bool WasInterrupted() const;

private:
	std::stop_source stopSource_;
	std::jthread monitor_;
#ifndef _WIN32
	std::jthread forceMonitor_;
	struct sigaction previousInt_ {};
	struct sigaction previousTerm_ {};
#endif
};
