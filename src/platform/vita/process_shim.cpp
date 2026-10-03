// newlib on the Vita has no waitpid, and the shared include/platform/process.hpp (ffplay helpers of the
// audio managers) calls it. Nothing on the Vita ever spawns a process, so every handle is INVALID_PROCESS and the
// helpers return before this is reached; the symbol only has to exist (VITA-52).
#include <sys/types.h>
#include <cerrno>

extern "C" pid_t waitpid(pid_t, int*, int) {
    errno = ECHILD;
    return -1;
}
