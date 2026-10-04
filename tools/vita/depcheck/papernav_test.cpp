// Host test for the geometry of src/ui/paper_nav.cpp (VITA-17). No ImGui, no Vita SDK:
//   c++ -std=c++20 -Wall -Wextra -Werror -DPAPER_NAV_GEOMETRY_ONLY -Iinclude tools/vita/depcheck/papernav_test.cpp src/ui/paper_nav.cpp -o /tmp/papernav_test && /tmp/papernav_test
#include "ui/paper_nav.hpp"

#include <cstdio>

using namespace wowee::ui;
static int g_fail = 0;
static void check(const char* name, bool ok) {
    if (!ok) ++g_fail;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

int main() {
    // A login card: account field, password field, then two buttons side by side, a link under them.
    const std::vector<NavRect> card = {
        {100, 100, 400, 140},  // 0 account
        {100, 160, 400, 200},  // 1 password
        {100, 230, 240, 270},  // 2 Connect
        {260, 230, 400, 270},  // 3 Quit
        {150, 300, 350, 320},  // 4 link
    };
    check("down from the account field is the password field", pickNext(card, 0, NavDir::Down) == 1);
    check("down from the password field is the button below it (left one: same column)", pickNext(card, 1, NavDir::Down) == 2);
    check("right from Connect is Quit", pickNext(card, 2, NavDir::Right) == 3);
    check("left from Quit is Connect", pickNext(card, 3, NavDir::Left) == 2);
    check("down from Quit is the link", pickNext(card, 3, NavDir::Down) == 4);
    check("up from the link is a button", pickNext(card, 4, NavDir::Up) == 2 || pickNext(card, 4, NavDir::Up) == 3);
    check("up from the first field stays", pickNext(card, 0, NavDir::Up) == 0);
    check("left from the left button stays", pickNext(card, 2, NavDir::Left) == 2);
    check("nothing selected: the topmost-leftmost", pickNext(card, -1, NavDir::Down) == 0);
    check("no rectangles: nothing", pickNext({}, -1, NavDir::Down) == -1);

    // a column of equal rows (a list) and a button far to the side that must not steal "down"
    const std::vector<NavRect> list = {{0, 0, 200, 30}, {0, 30, 200, 60}, {0, 60, 200, 90}, {500, 50, 600, 80}};
    check("down walks a list in order", pickNext(list, 0, NavDir::Down) == 1 && pickNext(list, 1, NavDir::Down) == 2);
    check("a control far to the side does not steal down", pickNext(list, 1, NavDir::Down) == 2);
    check("right from a row reaches the control at the side", pickNext(list, 1, NavDir::Right) == 3);

    // normalising
    const std::vector<NavRect> messy = {{0, 0, 960, 544}, {100, 100, 400, 140}, {100, 100, 400, 140}, {10, 10, 10, 50}, {120, 110, 200, 130}};
    const auto norm = normaliseNavRects(messy);
    check("normalise drops the panel that holds others, duplicates and empties",
          norm.size() == 1 && norm[0] == NavRect{120, 110, 200, 130});
    check("normalise keeps unrelated rectangles", normaliseNavRects(card).size() == card.size());

    // current control under a pointer: the smallest that holds it
    const std::vector<NavRect> nested = {{0, 0, 100, 100}, {20, 20, 60, 60}};
    check("current is the smallest rectangle holding the pointer", navCurrent(nested, 30, 30) == 1 && navCurrent(nested, 80, 80) == 0);
    check("no control under the pointer", navCurrent(nested, 500, 500) == -1);

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
