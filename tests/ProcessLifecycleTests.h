#pragma once
#include <optional>

// Helper modes share the existing data-test executable, but each terminal
// shutdown scenario runs in its own process.
std::optional<int> RunProcessLifecycleMode(int argc, char** argv);
