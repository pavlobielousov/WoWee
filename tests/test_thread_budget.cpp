// The thread budget (VITA-8) must be invisible off the Vita: every role gets back exactly the
// count the caller computed, and entering a thread changes nothing.
#include <catch_amalgamated.hpp>

#include "core/thread_budget.hpp"
#include "core/thread_pool.hpp"

using wowee::core::ThreadRole;

TEST_CASE("platformWorkerCount returns the desktop value unchanged", "[thread_budget]") {
#ifndef __vita__  // the Vita answers from a fixed table (vita_threads.cpp)
    for (int r = 0; r <= static_cast<int>(ThreadRole::AsyncEquipmentLoad); ++r) {
        const auto role = static_cast<ThreadRole>(r);
        for (size_t v : {size_t{0}, size_t{1}, size_t{2}, size_t{7}, size_t{16}}) {
            REQUIRE(wowee::core::platformWorkerCount(role, v) == v);
        }
    }
    wowee::core::enterThread(ThreadRole::Main);  // must be a no-op, not a crash
#endif
}

TEST_CASE("ThreadPool still runs tasks with a role", "[thread_budget]") {
    wowee::core::ThreadPool pool(2, ThreadRole::IoWorker);
    REQUIRE(pool.threadCount() == 2);
    auto f = pool.submit([] { return 42; });
    REQUIRE(f.get() == 42);
}
