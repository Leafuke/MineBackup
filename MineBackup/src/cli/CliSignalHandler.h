#pragma once

#include <stop_token>
#include <thread>

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
};
