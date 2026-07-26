#pragma once

#include <memory>

class NetworkBackend;

std::shared_ptr<NetworkBackend> CreatePlatformNetworkBackend();
