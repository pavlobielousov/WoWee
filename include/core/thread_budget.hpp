#pragma once

#include <cstddef>

namespace wowee {
namespace core {

// What a thread is for. The thread budget (VITA-8) is decided per role, so a platform with few
// cores can give each role its own count, core and priority in one place.
enum class ThreadRole {
    Main,
    NetworkPump,
    FrameWorker,
    IoWorker,
    TerrainWorker,
    WorldPreload,
    Watchdog,
    UpdateCheck,
    AsyncCreatureLoad,   // in-flight std::async creature model loads
    AsyncObjectLoad,     // in-flight std::async game object model loads
    AsyncEquipmentLoad,  // in-flight std::async equipment texture pre-decodes
};

}  // namespace core

#ifdef __vita__
namespace platform::vita {
// Implemented in src/platform/vita/vita_threads.cpp.
size_t workerCount(core::ThreadRole role, size_t desktopValue);
void enterThread(core::ThreadRole role);
}  // namespace platform::vita
#endif

namespace core {

// How many threads (or tasks in flight) a role gets. `desktopValue` is what the caller computes
// for the desktop; every platform except the Vita returns it unchanged. The Vita has three usable
// cores (core 3 belongs to the system) and no headroom, so it ignores `desktopValue` and answers
// from a fixed table.
inline size_t platformWorkerCount(ThreadRole role, size_t desktopValue) {
#ifdef __vita__
    return platform::vita::workerCount(role, desktopValue);
#else
    (void)role;
    return desktopValue;
#endif
}

// Call first thing on a thread. Desktop: does nothing. Vita: pins the thread to its cores and
// sets its priority (the layout is in vita_threads.cpp), and logs the result once per role.
inline void enterThread(ThreadRole role) {
#ifdef __vita__
    platform::vita::enterThread(role);
#else
    (void)role;
#endif
}

}  // namespace core
}  // namespace wowee
