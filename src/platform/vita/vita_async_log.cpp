// The log file's writer thread (VITA-20); see asyncLogWrite in platform/vita/vita_platform.hpp.
#include "core/thread_budget.hpp"
#include "platform/vita/vita_platform.hpp"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wowee::platform::vita {

namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<std::string> g_pending;
std::ofstream* g_stream = nullptr;
bool g_started = false;

void writerLoop() {
    core::enterThread(core::ThreadRole::UpdateCheck);  // lowest priority, off the main thread's core
    std::vector<std::string> batch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return !g_pending.empty(); });
            batch.swap(g_pending);
        }
        for (const std::string& line : batch) *g_stream << line;
        g_stream->flush();  // the card may take its time: lines gather meanwhile and go out together
        batch.clear();
    }
}

}  // namespace

void asyncLogWrite(std::ofstream& stream, std::string line) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_started) {
        g_started = true;
        g_stream = &stream;
        std::thread(writerLoop).detach();
    }
    if (g_pending.size() < 20000) g_pending.push_back(std::move(line));
    g_cv.notify_one();
}

}  // namespace wowee::platform::vita
