#include "PlatformCompat.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <iostream>

int main() {
#ifdef _WIN32
    return 0;
#else
    const int originalStdin = dup(STDIN_FILENO);
    const int nullInput = open("/dev/null", O_RDONLY);
    if (originalStdin < 0 || nullInput < 0) {
        std::cerr << "Unable to prepare non-TTY input test\n";
        return 1;
    }
    if (dup2(nullInput, STDIN_FILENO) < 0) {
        std::cerr << "Unable to redirect stdin\n";
        return 1;
    }
    close(nullInput);

    const int result = _kbhit();
    const bool remainedNonTty = isatty(STDIN_FILENO) == 0;
    (void)dup2(originalStdin, STDIN_FILENO);
    close(originalStdin);
    if (result != 0 || !remainedNonTty) {
        std::cerr << "Non-TTY input must be ignored without terminal mutation\n";
        return 1;
    }
    return 0;
#endif
}
