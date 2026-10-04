#pragma once
// Per-renderer CPU cost and draw counts, logged every 300 frames (VITA-19): where the frame goes before anything is optimised.
#include "core/logger.hpp"

#include <chrono>

namespace wowee::rendering::gl {

struct FrameStats {
    const char* name;
    double ms = 0.0;
    long draws = 0, objects = 0;
    int frames = 0;
    std::chrono::steady_clock::time_point start;

    explicit FrameStats(const char* n) : name(n) {}
    void begin() { start = std::chrono::steady_clock::now(); }
    void end(long drawCalls, long objectsDrawn) {
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        draws += drawCalls;
        objects += objectsDrawn;
        if (++frames == 300) {
            LOG_WARNING("GL stats ", name, ": ", ms / frames, " ms CPU/frame, ", static_cast<double>(draws) / frames,
                        " draw calls, ", static_cast<double>(objects) / frames, " objects");
            ms = 0.0;
            draws = objects = 0;
            frames = 0;
        }
    }
};

}  // namespace wowee::rendering::gl
