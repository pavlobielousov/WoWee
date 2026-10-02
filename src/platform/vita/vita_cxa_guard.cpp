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
// Guard layout. On ARM EABI the compiler's guard variable is **32 bits** (`_ZGV...` is `.size 4`;
// the static_assert below checks it), not the 64 bits of the generic Itanium ABI. Byte 0 is "initialised"
// (the compiler tests it inline before calling in), so the whole state has to fit in this one word:
//   bit 0       initialised
//   bit 8       an initialiser is running
//   bits 16-31  low 16 bits of the id of the thread running it (to tell a recursive initialiser, which
//               libstdc++ reports by throwing, from another thread, which waits)
// Two threads whose ids share the low 16 bits would be taken for one thread: the second then aborts
// instead of waiting. Thread ids on the Vita are sequential uids, so that needs 65536 threads.
// (VITA-42: an earlier version kept the owner in a second word. That word was the *neighbouring
// variable*: the guard wrote a thread id into it, and spun forever if it was non-zero, which showed
// up only when the section layout changed.)
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cxxabi.h>
#include <cstdint>
#include <cstdlib>

static_assert(sizeof(__cxxabiv1::__guard) == 4, "ARM EABI guard variables are 32 bits");

namespace {

constexpr std::uint32_t kDone = 1u;
constexpr std::uint32_t kBusy = 1u << 8;

inline std::atomic<std::uint32_t>& word(__cxxabiv1::__guard* guard) {
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(__cxxabiv1::__guard));
    return *reinterpret_cast<std::atomic<std::uint32_t>*>(guard);
}

inline std::uint32_t me() { return static_cast<std::uint32_t>(sceKernelGetThreadId()) & 0xFFFFu; }

}  // namespace

extern "C" int __cxa_guard_acquire(__cxxabiv1::__guard* guard) {
    auto& w = word(guard);
    for (;;) {
        std::uint32_t v = w.load(std::memory_order_acquire);
        if (v & kDone) return 0;
        if (!(v & kBusy)) {
            // v is 0 here: take it.
            if (w.compare_exchange_strong(v, kBusy | (me() << 16), std::memory_order_acquire)) return 1;
            continue;  // lost the race: look again
        }
        if ((v >> 16) == me()) std::abort();  // the initialiser needs its own static: libstdc++ throws here
        sceKernelDelayThread(200);            // another thread is initialising: wait (microseconds)
    }
}

extern "C" void __cxa_guard_release(__cxxabiv1::__guard* guard) {
    word(guard).store(kDone, std::memory_order_release);
}

// The initialiser threw: let the next caller try again.
extern "C" void __cxa_guard_abort(__cxxabiv1::__guard* guard) {
    word(guard).store(0, std::memory_order_release);
}
