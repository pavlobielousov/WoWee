// Vita process setup (VITA-6): newlib heap, main thread stack, clocks, sysmodules, sceNet,
// ux0:data/wowee, env.txt. initProcess() is the first call in the shared main().
#include <cstring>
#include "platform/vita/vita_platform.hpp"

#include "core/config_paths.hpp"
#include "core/env.hpp"
#include "core/logger.hpp"
#include "core/thread_budget.hpp"

#include <psp2/io/stat.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>

#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <new>
#include <psp2/kernel/clib.h>
#include <string>

// newlib reads these before main(), so they are compile-time values and env.txt cannot change
// them. Set them with -DWOWEE_VITA_HEAP_MB / -DWOWEE_VITA_STACK_MB (cmake/vita/Vita.cmake). The
// final heap budget comes from VITA-23.
#ifndef WOWEE_VITA_HEAP_MB
#define WOWEE_VITA_HEAP_MB 192
#endif
#ifndef WOWEE_VITA_STACK_MB
#define WOWEE_VITA_STACK_MB 4
#endif

extern "C" {
int _newlib_heap_size_user = WOWEE_VITA_HEAP_MB * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = WOWEE_VITA_STACK_MB * 1024 * 1024;
}

namespace wowee::platform::vita {

namespace {

// sceNetInit takes a buffer for the network stack's own use; 256 KB is what tools/vita/depcheck
// verified. Static so it is never part of the newlib heap budget.
char g_netMemory[256 * 1024];

struct InitReport {
    bool done = false;
    bool envFileFound = false;
    std::vector<std::string> envKeys;
    int clockArm = 0, clockBus = 0, clockGpu = 0, clockXbar = 0;
    int sysmoduleNet = 0, netInit = 0, netCtlInit = 0;
};
InitReport g_report;

}  // namespace

// Called by operator new when malloc fails, before the std::bad_alloc is thrown: says how full the heap was (VITA-23).
// Written with sceClibPrintf into a fixed buffer, because the logger allocates and this is exactly when nothing can.
void onAllocationFailed() {
    static bool reporting = false;
    if (!reporting) {
        reporting = true;
        const struct mallinfo heap = mallinfo();
        char line[160];
        std::snprintf(line, sizeof line, "wowee: ALLOCATION FAILED: heap in use %u MB, free in arena %u MB, arena %u MB of %d MB\n",
                      static_cast<unsigned>(heap.uordblks) / (1024 * 1024), static_cast<unsigned>(heap.fordblks) / (1024 * 1024),
                      static_cast<unsigned>(heap.arena) / (1024 * 1024), WOWEE_VITA_HEAP_MB);
        sceClibPrintf("%s", line);
        if (FILE* f = std::fopen("ux0:data/wowee/alloc_failed.txt", "a")) {
            std::fputs(line, f);
            std::fclose(f);
        }
        reporting = false;
    }
    throw std::bad_alloc();
}

void initProcess() {
    if (g_report.done) return;
    g_report.done = true;
    std::set_new_handler(&onAllocationFailed);

    // The stock maximum for each clock. Errors are kept for the report, not acted on: a Vita
    // that refuses a clock still runs, just slower.
    g_report.clockArm = scePowerSetArmClockFrequency(444);
    g_report.clockBus = scePowerSetBusClockFrequency(222);
    g_report.clockGpu = scePowerSetGpuClockFrequency(222);
    g_report.clockXbar = scePowerSetGpuXbarClockFrequency(166);

    g_report.sysmoduleNet = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam param = {g_netMemory, sizeof g_netMemory, 0};
    g_report.netInit = sceNetInit(&param);
    g_report.netCtlInit = sceNetCtlInit();

    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(kAppDir, 0777);

    g_report.envKeys = loadEnvFile(kEnvFile, g_report.envFileFound);

    // Where the data lives when env.txt does not say: the same default whether or not an
    // extraction is there yet (selectUserDataPath in main.cpp only sets this when one is).
    // env.txt can point it at uma0: (USB / SD2Vita) or anywhere else.
    core::setEnvVar("WOW_DATA_PATH", kDefaultDataRoot, /*overwrite=*/false);

    // HOME: the Warden handler (and anything else that follows the XDG convention) keeps its cache under $HOME, and the
    // fallback is a path relative to the working directory, which is app0: and read-only (VITA-23).
    core::setEnvVar("HOME", kAppDir, /*overwrite=*/false);
#ifdef WOWEE_VITA_CLIENT
    // The stock FrameXML interface (139 files, its Lua state and widget tree) needs more memory than a Vita has: it grew the
    // heap from 64 MB to the whole 288 MB before it finished loading (measured, VITA-23). Off until VITA-28 makes it fit;
    // WOWEE_LOAD_FRAMEXML=1 in env.txt turns it on again for the measurement.
    core::setEnvVar("WOWEE_LOAD_FRAMEXML", "0", /*overwrite=*/false);
#endif
}

void logStartupReport() {
    const InitReport& r = g_report;
    // WARNING on purpose: the default log level is WARNING, and this is the line a bug report needs.
    LOG_WARNING("Vita: heap ", _newlib_heap_size_user / (1024 * 1024), " MB, main stack ", WOWEE_VITA_STACK_MB, " MB");
    LOG_WARNING("Vita: clocks arm=", r.clockArm, " bus=", r.clockBus, " gpu=", r.clockGpu,
                " xbar=", r.clockXbar, " (0 = set; sysmodule net=", r.sysmoduleNet,
                " sceNetInit=", r.netInit, " sceNetCtlInit=", r.netCtlInit, ")");
    if (r.envFileFound) {
        std::string keys;
        for (const std::string& k : r.envKeys) keys += (keys.empty() ? "" : ", ") + k;
        LOG_WARNING("Vita: ", kEnvFile, " read, ", r.envKeys.size(), " variable(s): ", keys);
    } else {
        LOG_WARNING("Vita: ", kEnvFile, " not found (no overrides)");
    }
    const char* data = std::getenv("WOW_DATA_PATH");
    LOG_WARNING("Vita: data root   ", data ? data : "(unset)");
    if (data) {
        // sceIo refuses any path of more than 10 components below the device name (measured on the Vita, VITA-19): the
        // data root, "expansions", the expansion id, and up to 8 levels of the game's own tree (a few dozen files go
        // to 9).
        int depth = 0;
        bool inName = false;
        for (const char* c = std::strchr(data, ':') ? std::strchr(data, ':') + 1 : data; *c; ++c) {
            if (*c == '/') inName = false;
            else if (!inName) { inName = true; ++depth; }
        }
        if (depth + 2 + 8 > 10) {
            LOG_WARNING("Vita: the data root is ", depth, " levels deep: game files nested more than ", 10 - depth - 2,
                        " levels will not open (limit 10 path components); keep it at ux0: or one folder, see DEV_SETUP");
        }
    }
    LOG_WARNING("Vita: config root ", core::getConfigRoot());
    core::enterThread(core::ThreadRole::Main);
}

}  // namespace wowee::platform::vita
