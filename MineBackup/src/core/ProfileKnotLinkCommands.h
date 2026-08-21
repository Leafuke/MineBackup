#pragma once

#include "KnotLinkProtocol.h"

#include <cstddef>
#include <memory>
#include <string>

class HeadlessKnotLinkBridge;
class ProfileRuntime;

// Adapts the shared KnotLink command dispatcher to a headless ProfileRuntime.
// Long-running commands are acknowledged synchronously and completed on
// managed, cancellable worker threads.
class ProfileKnotLinkCommands {
public:
	ProfileKnotLinkCommands(
		ProfileRuntime& runtime,
		std::shared_ptr<HeadlessKnotLinkBridge> bridge);
	~ProfileKnotLinkCommands();

	ProfileKnotLinkCommands(const ProfileKnotLinkCommands&) = delete;
	ProfileKnotLinkCommands& operator=(const ProfileKnotLinkCommands&) = delete;

	void SetBridge(std::shared_ptr<HeadlessKnotLinkBridge> bridge);
	void Stop();
	std::string Handle(
		const std::shared_ptr<minebackup::knotlink::KnotLinkCommandContext>& context);
	std::size_t ActiveOperationCount();

private:
	struct Implementation;
	std::unique_ptr<Implementation> implementation_;
};
