// ThreadCheck (VITA-8): what the Vita kernel really does with std::thread, so the thread budget
// in src/platform/vita/vita_threads.cpp rests on measurements. Writes
// ux0:data/wowee/threadcheck.log, one "key=value" per line. Run: see docs/vita/DEV_SETUP.md
// section 13.
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <pthread.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "core/logger.hpp"
#include "platform/vita/vita_platform.hpp"
#include "core/thread_budget.hpp"

static void out(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    // Open, append, close every time: a hang or crash must not lose the lines before it.
    SceUID fd = sceIoOpen("ux0:data/wowee/threadcheck.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, static_cast<SceSize>(n));
        sceIoClose(fd);
    }
}

static void describe(const char* who) {
    SceKernelThreadInfo info;
    std::memset(&info, 0, sizeof info);
    info.size = sizeof info;
    int r = sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    out("%s.info_result=%#x\n", who, static_cast<unsigned>(r));
    out("%s.stack_size=%d\n", who, info.stackSize);
    out("%s.init_priority=%d\n", who, info.initPriority);
    out("%s.priority=%d\n", who, info.currentPriority);
    out("%s.init_affinity=%#x\n", who, static_cast<unsigned>(info.initCpuAffinityMask));
    out("%s.affinity=%#x\n", who, static_cast<unsigned>(info.currentCpuAffinityMask));
    out("%s.cpu_now=%d\n", who, sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()));
}

// Burns stack until it dies; reports how deep it got, to measure the real usable stack.
[[gnu::noinline]] static void recurse(int d) {
    volatile char pad[1024];
    pad[0] = static_cast<char>(d);
    if (d % 32 == 0) out("recurse.depth_kb=%d\n", d);
    if (d < 100000) recurse(d + 1);
    asm volatile("" :: "r"(pad) : "memory");
}

static void setAffinity(int mask) {
    int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
    out("set_affinity.%#x.result=%#x now=%#x\n", static_cast<unsigned>(mask),
        static_cast<unsigned>(r), static_cast<unsigned>(sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId())));
}


// Core placement. The kernel does not say which core a thread runs on, so measure it: busy
// threads count loop iterations for a fixed time. Two threads sharing a core get about one core's
// worth between them; threads on different cores add up. `cores` is the affinity mask per thread
// (0 = leave unrestricted). Prints each phase's total as a percentage of one core's baseline.
static unsigned long long g_baseline = 0;

static void placementPhase(const char* name, const std::vector<int>& cores) {
    std::atomic<bool> go{false}, stop{false};
    std::vector<unsigned long long> counts(cores.size(), 0);
    std::vector<std::thread> ts;
    for (size_t i = 0; i < cores.size(); ++i) {
        ts.emplace_back([&, i] {
            if (cores[i] != 0) sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), cores[i]);
            while (!go.load(std::memory_order_acquire)) sceKernelDelayThread(1000);
            unsigned long long n = 0;
            unsigned x = 1;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int k = 0; k < 1000; ++k) x = x * 1664525u + 1013904223u;
                asm volatile("" :: "r"(x));
                ++n;
            }
            counts[i] = n;
        });
    }
    sceKernelDelayThread(200 * 1000);  // let them start and pin
    go.store(true, std::memory_order_release);
    sceKernelDelayThread(1500 * 1000);
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : ts) t.join();
    unsigned long long total = 0;
    for (auto c : counts) total += c;
    if (g_baseline == 0) g_baseline = total;
    out("placement.%s.threads=%d total_iterations=%llu percent_of_one_core=%llu\n", name,
        static_cast<int>(cores.size()), total, total * 100 / g_baseline);
}

static void placementTest() {
    // The busy threads run at the creator's priority (159), which beats main's 160: with every
    // user core busy, main would never wake to stop them (found on hardware, first run hung in
    // phase 4). Main goes above them for the test.
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 100);
    const int c0 = SCE_KERNEL_CPU_MASK_USER_0, c1 = SCE_KERNEL_CPU_MASK_USER_1,
              c2 = SCE_KERNEL_CPU_MASK_USER_2, all = SCE_KERNEL_CPU_MASK_USER_ALL;
    placementPhase("one_thread_core1", {c1});          // the baseline: 100
    placementPhase("two_threads_same_core1", {c1, c1}); // expect about 100
    placementPhase("two_threads_core1_core2", {c1, c2}); // expect about 200
    placementPhase("three_threads_core0_1_2", {c0, c1, c2}); // expect about 300
    placementPhase("four_threads_unrestricted", {all, all, all, all}); // about 300 if core 3 stays unused
    placementPhase("four_threads_no_affinity_call", {0, 0, 0, 0});     // same, no call at all
}

// Function-local statics (src/platform/vita/vita_cxa_guard.cpp, VITA-42): three threads race to a
// static with a slow initialiser. Expect one initialisation and 42 everywhere.
static std::atomic<int> g_guardInits{0};
static int guardedValue() {
    static const int v = [] {
        ++g_guardInits;
        sceKernelDelayThread(50000);
        return 42;
    }();
    return v;
}
static void guardRace() {
    std::atomic<int> good{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 3; ++i) ts.emplace_back([&] { if (guardedValue() == 42) ++good; });
    for (auto& t : ts) t.join();
    out("guard_race.inits=%d values_ok=%d\n", g_guardInits.load(), good.load());
}

// Optional crash-on-purpose test of the MAIN thread stack (4 MB requested in vita_main.cpp).
static void recurseMain() {
    out("recurse_main=start\n");
    recurse(0);
}

int main() {
    wowee::platform::vita::initProcess();  // heap, stack, sceNet, paths: the same start as the app
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/wowee", 0777);
    sceIoClose(sceIoOpen("ux0:data/wowee/threadcheck.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777));

#ifndef THREADCHECK_NO_LOGLEVEL
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);
#endif
    LOG_WARNING("threadcheck: logger started on the main thread");
    out("logger_main=ok\n");
    out("hardware_concurrency=%u\n", std::thread::hardware_concurrency());
    describe("main");
    guardRace();

    std::thread t([] { describe("std_thread"); });
    t.join();

    // Priority and affinity changes from inside a std::thread.
    std::thread t2([] {
        int r = sceKernelChangeThreadPriority(sceKernelGetThreadId(), 0x60);
        out("set_priority.0x60.result=%#x\n", static_cast<unsigned>(r));
        r = sceKernelChangeThreadPriority(sceKernelGetThreadId(), 0);
        out("set_priority.0_default.result=%#x\n", static_cast<unsigned>(r));
        setAffinity(SCE_KERNEL_CPU_MASK_USER_0);
        setAffinity(SCE_KERNEL_CPU_MASK_USER_1);
        setAffinity(SCE_KERNEL_CPU_MASK_USER_2);
        setAffinity(SCE_KERNEL_CPU_MASK_USER_ALL);
        setAffinity(SCE_KERNEL_CPU_MASK_SYSTEM);
        describe("std_thread_after");
    });
    t2.join();

    // Raw pthread with an explicit stack size.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t def = 0;
    pthread_attr_getstacksize(&attr, &def);
    out("pthread_attr_default_stacksize=%llu\n", static_cast<unsigned long long>(def));
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_t pt;
    int pr = pthread_create(&pt, &attr, [](void*) -> void* { describe("pthread_512k"); return nullptr; }, nullptr);
    out("pthread_create_512k.result=%d\n", pr);
    if (pr == 0) pthread_join(pt, nullptr);

    // The real thread budget (src/platform/vita/vita_threads.cpp) from inside std::threads.
    for (int r = 0; r <= static_cast<int>(wowee::core::ThreadRole::AsyncEquipmentLoad); ++r) {
        const auto role = static_cast<wowee::core::ThreadRole>(r);
        out("role.%d.count=%llu\n", r, static_cast<unsigned long long>(wowee::core::platformWorkerCount(role, 99)));
        std::thread rt([role, r] {
            wowee::core::enterThread(role);
            char who[24];
            snprintf(who, sizeof who, "role%d", r);
            describe(who);
        });
        rt.join();
    }

    placementTest();

    SceUID mainFd = sceIoOpen("ux0:data/wowee/threadcheck.recurse_main", SCE_O_RDONLY, 0);
    if (mainFd >= 0) {
        sceIoClose(mainFd);
        recurseMain();
    }

    // Optional, because it ends in a crash by design: with ux0:data/wowee/threadcheck.recurse
    // present, recurse in a std::thread until the stack is gone. The last "recurse.depth_kb"
    // line is how far it got (1 KB per frame).
    SceUID probeFd = sceIoOpen("ux0:data/wowee/threadcheck.recurse", SCE_O_RDONLY, 0);
    if (probeFd >= 0) {
        sceIoClose(probeFd);
        out("recurse_in_std_thread=start\n");
        std::thread t3([] { recurse(0); });
        t3.join();
    }
    sceKernelDelayThread(400 * 1000);  // past the logger's flush interval
    wowee::core::Logger::getInstance().flushIfStale();
    sceKernelExitProcess(0);
    return 0;
}
