// Thread-safe function-local statics on the Vita (VITA-6).
//
// libstdc++'s __cxa_guard_acquire / release use a pthread mutex and condition variable that are
// statically initialised. With threads enabled (the link needs -Wl,-u,pthread_cancel for
// std::thread to work, see tools/vita/depcheck/CMakeLists.txt) the first function-local static
// with a non-trivial initialiser crashes: pthread_mutex_lock reads address 0 in __cxa_guard_acquire,
// and __cxa_guard_release fails in pthread_cond_broadcast ("__concurrence_broadcast_error").
// Measured on Vita3K only; without that link option the guards work but std::thread throws.
// WoWee is full of Meyers singletons (Logger::getInstance is the first), so the guards are
// replaced here with a small lock-free implementation. Linked into the executable, it wins over
// the libstdc++.a member, which then is never pulled in.
//
// Guard layout (Itanium C++ ABI, 8 bytes): byte 0 is "initialised" (the compiler tests it inline
// before calling in); the second 32-bit word is ours: the id of the thread that is initialising
// right now, or 0.
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cxxabi.h>
#include <cstdint>
#include <cstdlib>

namespace {

struct Guard {
    std::atomic<std::uint8_t> done;
    std::uint8_t pad[3];
    std::atomic<std::uint32_t> owner;
};
static_assert(sizeof(Guard) == 8, "Itanium ABI guard is 64 bits");

}  // namespace

extern "C" int __cxa_guard_acquire(__cxxabiv1::__guard* guard) {
    Guard* g = reinterpret_cast<Guard*>(guard);
    if (g->done.load(std::memory_order_acquire)) return 0;
    const std::uint32_t me = static_cast<std::uint32_t>(sceKernelGetThreadId());
    for (;;) {
        std::uint32_t expected = 0;
        if (g->owner.compare_exchange_strong(expected, me, std::memory_order_acquire)) {
            // Someone may have finished between the first check and the exchange.
            if (g->done.load(std::memory_order_acquire)) {
                g->owner.store(0, std::memory_order_release);
                return 0;
            }
            return 1;  // this thread runs the initialiser
        }
        if (expected == me) std::abort();  // the initialiser needs its own static: libstdc++ throws here
        if (g->done.load(std::memory_order_acquire)) return 0;
        sceKernelDelayThread(200);  // another thread is initialising: wait (microseconds)
    }
}

extern "C" void __cxa_guard_release(__cxxabiv1::__guard* guard) {
    Guard* g = reinterpret_cast<Guard*>(guard);
    g->done.store(1, std::memory_order_release);
    g->owner.store(0, std::memory_order_release);
}

// The initialiser threw: let the next caller try again.
extern "C" void __cxa_guard_abort(__cxxabiv1::__guard* guard) {
    Guard* g = reinterpret_cast<Guard*>(guard);
    g->owner.store(0, std::memory_order_release);
}
