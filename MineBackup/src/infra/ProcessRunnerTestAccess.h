#pragma once

// Deliberately absent from production builds and from ProcessSpec. Set hooks
// only in isolated test hosts, while no Run call is using the hook state.
#if defined(MINEBACKUP_PROCESS_TEST_HOOKS) && !defined(_WIN32)
#include <cstddef>
namespace ProcessRunner::Testing {
void SetCapacity(std::size_t capacity);
void SetAfterSpawn(void (*hook)(void*) noexcept, void* context);
void FailNextWait(int error);
bool IsClosing();
}
#endif
