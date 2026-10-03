#pragma once

// Resource numbers for a long headless run (VITA-7, VITA-10): game-update time per second, and on
// the Vita the heap in use and the CPU share of each thread. Fork-only, header-only.
//
// CPU share: SceKernelThreadInfo::runClocks is the time a thread has run; the share is its growth
// over the report window divided by the window. Threads are the main thread and every thread that
// called core::enterThread (src/platform/vita/vita_threads.cpp). Percent of ONE core. (The
// documented sceKernelGetThreadRunStatus data-aborts on a real Vita, so it is not used.)

#include "core/logger.hpp"

#include <chrono>
#include <cstdint>

#ifdef __vita__
#include "platform/vita/vita_platform.hpp"

#include <malloc.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <cstring>
#include <string>
#include <vector>
#endif

namespace wowee::headless {

class RunStats {
public:
    using Clock = std::chrono::steady_clock;

    explicit RunStats(int reportEverySeconds) : every_(reportEverySeconds), last_(Clock::now()) {}

    // Around the game and auth update calls.
    void tickBegin() { tickStart_ = Clock::now(); }
    void tickEnd() {
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - tickStart_).count();
        tickMsTotal_ += ms;
        if (ms > tickMsMax_) tickMsMax_ = ms;
        ++ticks_;
    }

    // Once per loop iteration; reports every `every_` seconds.
    void sample() {
#ifdef __vita__
        // A headless app takes no input, so without this the system turns the screen off and then
        // suspends the app (the first 5-minute run on hardware stopped after 1.5 minutes).
        const auto t = Clock::now();
        if (t - lastPowerTick_ >= std::chrono::seconds(1)) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
            lastPowerTick_ = t;
        }
#endif
        const auto now = Clock::now();
        if (now - last_ < std::chrono::seconds(every_)) return;
        report(std::chrono::duration<double>(now - last_).count());
        last_ = now;
    }

private:
    void report(double seconds) {
        LOG_WARNING("[stats] game update ", ticks_ ? tickMsTotal_ / static_cast<double>(ticks_) : 0.0,
                    " ms avg, ", tickMsMax_, " ms max, ", tickMsTotal_ / seconds / 10.0,
                    " % of wall time");
#ifdef __vita__
        const struct mallinfo mi = mallinfo();
        LOG_WARNING("[stats] heap in use ", static_cast<unsigned>(mi.uordblks) / 1024, " KB of ",
                    static_cast<unsigned>(mi.arena) / 1024, " KB arena");
        reportThreads(seconds);
#endif
        tickMsTotal_ = 0;
        tickMsMax_ = 0;
        ticks_ = 0;
    }

#ifdef __vita__
    struct ThreadCpu {
        int tid;
        std::string name;
        uint64_t lastRunUs;
    };

    void reportThreads(double seconds) {
        // The main thread, then everything that registered; a thread seen for the first time
        // starts counting at this report.
        platform::vita::RegisteredThread reg[32];
        const size_t n = platform::vita::registeredThreads(reg, 32);
        std::vector<std::pair<int, std::string>> now{{sceKernelGetThreadId(), "main"}};
        for (size_t i = 0; i < n; ++i) now.push_back({reg[i].tid, reg[i].roleName});

        std::string line;
        double total = 0;
        for (const auto& [tid, name] : now) {
            SceKernelThreadInfo info;
            std::memset(&info, 0, sizeof info);
            info.size = sizeof info;
            if (sceKernelGetThreadInfo(tid, &info) < 0) continue;
            const uint64_t run = info.runClocks;
            ThreadCpu* seen = nullptr;
            for (auto& t : threads_) {
                if (t.tid == tid) seen = &t;
            }
            if (!seen) {
                threads_.push_back({tid, name, run});
                continue;
            }
            const double pct = 100.0 * static_cast<double>(run - seen->lastRunUs) / (seconds * 1e6);
            seen->lastRunUs = run;
            total += pct;
            line += " " + name + "#" + std::to_string(tid & 0xffff) + " " + std::to_string(pct).substr(0, 5);
        }
        LOG_WARNING("[stats] cpu (percent of one core, total ", total, "):", line);
    }

    std::vector<ThreadCpu> threads_;
#endif

    int every_;
    Clock::time_point last_;
    Clock::time_point tickStart_{};
    Clock::time_point lastPowerTick_{};
    double tickMsTotal_ = 0;
    double tickMsMax_ = 0;
    uint64_t ticks_ = 0;
};

}  // namespace wowee::headless
