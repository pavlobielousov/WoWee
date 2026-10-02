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
