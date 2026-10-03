#pragma once

// QueryTimer, moved verbatim from vk_frame_data.hpp (VITA-51): the collision queries of the WMO and M2
// renderers time themselves with it, and a build with no Vulkan has to be able to include it.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace wowee {
namespace rendering {

// Timer utility for performance profiling queries.
// Uses atomics because floor-height queries are dispatched on async threads
// from CameraController while the main thread may read the counters.
struct QueryTimer {
    std::atomic<double>* totalMs = nullptr;
    std::atomic<uint32_t>* callCount = nullptr;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    QueryTimer(std::atomic<double>* total, std::atomic<uint32_t>* calls)
        : totalMs(total), callCount(calls) {}
    ~QueryTimer() {
        if (callCount) {
            callCount->fetch_add(1, std::memory_order_relaxed);
        }
        if (totalMs) {
            auto end = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            // Relaxed is fine for diagnostics - exact ordering doesn't matter.
            double old = totalMs->load(std::memory_order_relaxed);
            while (!totalMs->compare_exchange_weak(old, old + ms, std::memory_order_relaxed)) {}
        }
    }
};

} // namespace rendering
} // namespace wowee
