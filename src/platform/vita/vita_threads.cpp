// Thread budget for the Vita (VITA-8): how many threads each role gets, which cores they may run
// on, their priority, and a stack big enough to use. See docs/vita/DEV_SETUP.md section 13.
#include "core/thread_budget.hpp"

#include "core/logger.hpp"

#include <psp2/kernel/threadmgr.h>

#include <pthread.h>

#include <atomic>
#include <cstring>

namespace wowee::platform::vita {

namespace {

using core::ThreadRole;

// Priorities: lower number = runs first. The main thread keeps the system default (160); the
// network pump sits above it so a packet is never waiting for a frame to end, everything else
// below it so background work cannot make a frame late. User range is 64..191.
struct RoleLayout {
    ThreadRole role;
    const char* name;
    size_t count;  // threads (or tasks in flight) the role gets
    int cpuMask;   // SCE_KERNEL_CPU_MASK_USER_*; never the system core
    int priority;  // 0 = leave as is
};

constexpr int kCore0 = SCE_KERNEL_CPU_MASK_USER_0;
constexpr int kCore1 = SCE_KERNEL_CPU_MASK_USER_1;
constexpr int kCore2 = SCE_KERNEL_CPU_MASK_USER_2;

constexpr RoleLayout kLayout[] = {
    {ThreadRole::Main,               "main",           1, kCore0,          0},
    {ThreadRole::NetworkPump,        "network-pump",   1, kCore1,          112},
    {ThreadRole::FrameWorker,        "frame-worker",   1, kCore1 | kCore2, 176},
    {ThreadRole::IoWorker,           "io-worker",      1, kCore1 | kCore2, 184},
    {ThreadRole::TerrainWorker,      "terrain-worker", 1, kCore2,          184},
    {ThreadRole::WorldPreload,       "world-preload",  1, kCore2,          188},
    {ThreadRole::Watchdog,           "watchdog",       0, kCore2,          188},  // not started
    {ThreadRole::UpdateCheck,        "update-check",   1, kCore2,          188},
    {ThreadRole::AsyncCreatureLoad,  "async-creature", 1, kCore2,          184},
    {ThreadRole::AsyncObjectLoad,    "async-object",   1, kCore2,          184},
    {ThreadRole::AsyncEquipmentLoad, "async-equip",    1, kCore2,          186},
};

const RoleLayout* find(ThreadRole role) {
    for (const RoleLayout& l : kLayout) {
        if (l.role == role) return &l;
    }
    return nullptr;
}

constexpr int kRoleCount = sizeof(kLayout) / sizeof(kLayout[0]);
std::atomic<bool> g_logged[kRoleCount];

}  // namespace

size_t workerCount(ThreadRole role, size_t /*desktopValue*/) {
    const RoleLayout* l = find(role);
    return l ? l->count : 1;
}

void enterThread(ThreadRole role) {
    const RoleLayout* l = find(role);
    if (!l) return;

    const SceUID tid = sceKernelGetThreadId();
    const int affinityResult = sceKernelChangeThreadCpuAffinityMask(tid, l->cpuMask);
    int priorityResult = 0;
    if (l->priority != 0) priorityResult = sceKernelChangeThreadPriority(tid, l->priority);

    // One line per role, not per thread: the pools start several threads of one role, and the
    // line is for the startup layout, not for tracing.
    const int index = static_cast<int>(l - kLayout);
    if (g_logged[index].exchange(true, std::memory_order_acq_rel)) return;

    SceKernelThreadInfo info;
    std::memset(&info, 0, sizeof info);
    info.size = sizeof info;
    sceKernelGetThreadInfo(tid, &info);
    LOG_WARNING("Vita thread: ", l->name, " tid=", tid, " cores=0x", std::hex,
                static_cast<unsigned>(info.currentCpuAffinityMask), std::dec,
                " priority=", info.currentPriority, " stack=", info.stackSize / 1024, "KB",
                (affinityResult < 0 || priorityResult < 0) ? " (REFUSED, see log)" : "");
    if (affinityResult < 0 || priorityResult < 0) {
        LOG_WARNING("Vita thread: ", l->name, " affinity result 0x", std::hex,
                    static_cast<unsigned>(affinityResult), " priority result 0x",
                    static_cast<unsigned>(priorityResult), std::dec);
    }
}

}  // namespace wowee::platform::vita

// ---------------------------------------------------------------------------------------------
// Thread stacks. pthread-embedded gives a std::thread a 32 KB stack unless the creator asks for
// more, and libstdc++ never asks (measured: tools/vita/threadcheck). The ADT / M2 / WMO / BLP
// parsers run on worker threads and need far more, so the link wraps pthread_create
// (-Wl,--wrap=pthread_create in cmake/vita/Vita.cmake) and raises any stack below
// WOWEE_VITA_THREAD_STACK_KB (512) to that. A bigger request is left alone.
#ifndef WOWEE_VITA_THREAD_STACK_KB
#define WOWEE_VITA_THREAD_STACK_KB 512
#endif

extern "C" {
int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*),
                          void* arg) {
    constexpr size_t kWanted = static_cast<size_t>(WOWEE_VITA_THREAD_STACK_KB) * 1024;
    pthread_attr_t local;
    if (attr) {
        local = *attr;
    } else {
        pthread_attr_init(&local);
    }
    size_t current = 0;
    if (pthread_attr_getstacksize(&local, &current) != 0 || current < kWanted) {
        pthread_attr_setstacksize(&local, kWanted);
    }
    return __real_pthread_create(thread, &local, start, arg);
}
}
