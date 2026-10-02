// Host test for include/platform/vita/device_path.hpp (VITA-35). No Vita SDK needed:
//   c++ -std=c++20 -Wall -Wextra -Werror -I../../../include devpath_test.cpp -o /tmp/devpath_test && /tmp/devpath_test
#include "devpath_cases.hpp"

#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;
static int g_fail = 0;

static void check(const char* name, bool ok, const char* detail = "") {
    if (!ok) ++g_fail;
    std::printf("%s %s %s\n", ok ? "PASS" : "FAIL", name, detail);
}

int main() {
    devpath_cases::run(check);

    // removeTree on a real tree (host filesystem; nested dirs, files at every level).
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "wowee_devpath_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "a" / "b", ec);
    { std::ofstream(root / "x.bin") << "x"; }
    { std::ofstream(root / "a" / "y.bin") << "y"; }
    { std::ofstream(root / "a" / "b" / "z.bin") << "z"; }
    const auto n = wowee::platform::vita::removeTree(root, ec);
    check("removeTree removes the tree", !ec && !fs::exists(root), ec.message().c_str());
    check("removeTree counts 6 entries", n == 6);
    wowee::platform::vita::removeTree(root, ec);
    check("removeTree on a missing path is not an error", !ec, ec.message().c_str());

    std::printf("%d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
