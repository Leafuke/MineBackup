#pragma once
#include "PlatformCompat.h"
#if defined(_WIN32)
#include "Platform_win.h"
#elif defined(__APPLE__)
#include "Platform_macos.h"
#else
#include "Platform_linux.h"
#endif
