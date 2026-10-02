// Vita process setup (VITA-6): newlib heap, main thread stack, clocks, sysmodules, sceNet,
// ux0:data/wowee, env.txt. initProcess() is the first call in the shared main().
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

#include <cstdlib>
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

void initProcess() {
    if (g_report.done) return;
    g_report.done = true;

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
}

void logStartupReport() {
    const InitReport& r = g_report;
    // WARNING on purpose: the default log level is WARNING, and this is the line a bug report needs.
    LOG_WARNING("Vita: heap ", WOWEE_VITA_HEAP_MB, " MB, main stack ", WOWEE_VITA_STACK_MB, " MB");
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
    LOG_WARNING("Vita: config root ", core::getConfigRoot());
    core::enterThread(core::ThreadRole::Main);
}

}  // namespace wowee::platform::vita
