// The memory card, as the main thread sees it (VITA-20): a priority gate for the worker threads, and a trace of slow calls.
//
// THE GATE. The card is one queue and the kernel serves it first come, first served. With three or four worker threads streaming
// models and textures, a main-thread call (a log line, a stat, the read of a small file) waited behind all of their reads: 100 to
// 1000 ms, which is where the frame stalls of VITA-20 came from. Every sceIo call of a thread other than the main thread now takes a
// gate first, and a read is cut into 64 KB pieces with the gate released between them; the main thread never takes it. So at most one
// worker piece (a few milliseconds) is ever in the card's queue ahead of the main thread.
//
// THE TRACE.
// The card is one queue: while worker threads stream reads, any sceIo call from the main thread (a log line, an fopen, a stat, a
// file read inside a widget) can wait hundreds of milliseconds, and the frame stalls in whatever stage made the call. The log's
// stage timers say WHERE a frame stalled, not what it waited for. Linked with -Wl,--wrap=sceIoOpen,... (cmake/vita/Vita.cmake,
// wowee_vita_executable) every sceIo call of the program, from newlib, the logger and std::filesystem alike, passes through here.
// With WOWEE_IO_TRACE_MS=<n> in env.txt, a call from the main thread that takes longer than n milliseconds is logged with its path and
// size; unset, the wrappers only call through (a thread-id test and an integer compare).
#include "core/logger.hpp"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstdio>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

extern "C" {
SceUID __real_sceIoOpen(const char* file, int flags, SceMode mode);
int __real_sceIoClose(SceUID fd);
int __real_sceIoRead(SceUID fd, void* data, SceSize size);
int __real_sceIoWrite(SceUID fd, const void* data, SceSize size);
int __real_sceIoGetstat(const char* file, SceIoStat* stat);
SceUID __real_sceIoDopen(const char* dirname);
int __real_sceIoRemove(const char* file);
int __real_sceIoMkdir(const char* dir, SceMode mode);
int __real_sceIoRename(const char* oldname, const char* newname);
}

namespace {

std::atomic<int> g_thresholdMs{-1};  // -1 = off (or not set yet)
std::atomic<SceUID> g_mainThread{0};
std::mutex g_pathMutex;
std::unordered_map<SceUID, std::string> g_paths;  // files the main thread opened, for the read and write lines

// The workers' gate, and the size of one piece of a worker's read.
std::mutex g_gate;
constexpr SceSize kPiece = 64 * 1024;

bool isMain() {
    const SceUID main = g_mainThread.load(std::memory_order_relaxed);
    return main == 0 || sceKernelGetThreadId() == main;
}

struct GateHold {  // held by a worker around one card call; the main thread holds nothing
    bool held = false;
    GateHold() {
        if (!isMain()) {
            g_gate.lock();
            held = true;
        }
    }
    ~GateHold() {
        if (held) g_gate.unlock();
    }
};

bool tracing() {
    int threshold = g_thresholdMs.load(std::memory_order_relaxed);
    if (g_mainThread.load(std::memory_order_relaxed) == 0) g_mainThread.store(sceKernelGetThreadId());  // the first call is the main thread's
    // The first sceIo call of the program reads env.txt, which is what sets WOWEE_IO_TRACE_MS: ask again, every 64th call, until it is there.
    static std::atomic<unsigned> calls{0};
    if (threshold < 0 && (calls.fetch_add(1, std::memory_order_relaxed) & 63u) == 0) {
        const char* v = std::getenv("WOWEE_IO_TRACE_MS");
        if (v) {
            threshold = std::atoi(v);
            g_thresholdMs.store(threshold);
        }
    }
    return threshold >= 0 && sceKernelGetThreadId() == g_mainThread.load(std::memory_order_relaxed);
}

struct Timer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    [[nodiscard]] double ms() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
};

// The report is queued, never logged here: a slow call can be the logger's own write, made with the logger's lock held, and logging
// from inside it would take the same lock again (the first version did, and froze the main thread for good). A background thread
// drains the queue (vitaIoTraceDrain, called by the renderer's poll thread).
std::mutex g_queueMutex;
std::vector<std::string> g_queue;

void report(const char* op, const std::string& what, long bytes, double ms) {
    if (ms < g_thresholdMs.load(std::memory_order_relaxed)) return;
    char line[512];
    std::snprintf(line, sizeof line, "IO %s '%.300s' %ld bytes took %.1f ms (main thread)", op, what.c_str(), bytes, ms);
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.size() < 2000) g_queue.emplace_back(line);
}

std::string pathOf(SceUID fd) {
    std::lock_guard<std::mutex> lock(g_pathMutex);
    auto it = g_paths.find(fd);
    return it == g_paths.end() ? std::string("fd ") + std::to_string(fd) : it->second;
}

}  // namespace

extern "C" {

SceUID __wrap_sceIoOpen(const char* file, int flags, SceMode mode) {
    const bool traced = tracing();
    GateHold gate;
    if (!traced) return __real_sceIoOpen(file, flags, mode);
    Timer t;
    const SceUID fd = __real_sceIoOpen(file, flags, mode);
    if (fd >= 0) {
        std::lock_guard<std::mutex> lock(g_pathMutex);
        g_paths[fd] = file ? file : "";
    }
    report("open", file ? file : "", 0, t.ms());
    return fd;
}

int __wrap_sceIoClose(SceUID fd) {
    const bool traced = tracing();
    GateHold gate;
    if (!traced) return __real_sceIoClose(fd);
    Timer t;
    const std::string p = pathOf(fd);
    const int r = __real_sceIoClose(fd);
    {
        std::lock_guard<std::mutex> lock(g_pathMutex);
        g_paths.erase(fd);
    }
    report("close", p, 0, t.ms());
    return r;
}

int __wrap_sceIoRead(SceUID fd, void* data, SceSize size) {
    const bool traced = tracing();
    if (!isMain() && size > kPiece) {
        // A worker's big read goes in pieces, the gate released between them: the main thread's calls slip in at the boundaries.
        char* out = static_cast<char*>(data);
        SceSize done = 0;
        while (done < size) {
            const SceSize want = size - done < kPiece ? size - done : kPiece;
            int n;
            {
                GateHold gate;
                n = __real_sceIoRead(fd, out + done, want);
            }
            if (n < 0) return done ? static_cast<int>(done) : n;
            done += static_cast<SceSize>(n);
            if (static_cast<SceSize>(n) < want) break;  // end of file
        }
        return static_cast<int>(done);
    }
    GateHold gate;
    if (!traced) return __real_sceIoRead(fd, data, size);
    Timer t;
    const int r = __real_sceIoRead(fd, data, size);
    report("read", pathOf(fd), r, t.ms());
    return r;
}

int __wrap_sceIoWrite(SceUID fd, const void* data, SceSize size) {
    const bool traced = tracing();
    GateHold gate;
    if (!traced) return __real_sceIoWrite(fd, data, size);
    Timer t;
    const int r = __real_sceIoWrite(fd, data, size);
    report("write", pathOf(fd), r, t.ms());
    return r;
}

int __wrap_sceIoGetstat(const char* file, SceIoStat* stat) {
    const bool traced = tracing();
    GateHold gate;
    if (!traced) return __real_sceIoGetstat(file, stat);
    Timer t;
    const int r = __real_sceIoGetstat(file, stat);
    report("stat", file ? file : "", 0, t.ms());
    return r;
}

SceUID __wrap_sceIoDopen(const char* dirname) {
    const bool traced = tracing();
    GateHold gate;
    if (!traced) return __real_sceIoDopen(dirname);
    Timer t;
    const SceUID r = __real_sceIoDopen(dirname);
    report("opendir", dirname ? dirname : "", 0, t.ms());
    return r;
}

int __wrap_sceIoRemove(const char* file) {
    const bool traced = tracing();
    GateHold gate;
    if (!traced) return __real_sceIoRemove(file);
    Timer t;
    const int r = __real_sceIoRemove(file);
    report("remove", file ? file : "", 0, t.ms());
    return r;
}

int __wrap_sceIoMkdir(const char* dir, SceMode mode) {
    const bool traced = tracing();
    GateHold gate;
    if (!traced) return __real_sceIoMkdir(dir, mode);
    Timer t;
    const int r = __real_sceIoMkdir(dir, mode);
    report("mkdir", dir ? dir : "", 0, t.ms());
    return r;
}

int __wrap_sceIoRename(const char* oldname, const char* newname) {
    const bool traced = tracing();
    GateHold gate;
    if (!traced) return __real_sceIoRename(oldname, newname);
    Timer t;
    const int r = __real_sceIoRename(oldname, newname);
    report("rename", std::string(oldname ? oldname : "") + " -> " + (newname ? newname : ""), 0, t.ms());
    return r;
}

}  // extern "C"

namespace wowee::platform::vita {

// Writes the queued slow-call lines to the log; call it from a thread that is not the main thread.
void vitaIoTraceDrain() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        lines.swap(g_queue);
    }
    for (const std::string& l : lines) LOG_WARNING(l);
}

}  // namespace wowee::platform::vita
