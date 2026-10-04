#pragma once
// Gamepad navigation for the pre-game screens' own controls (VITA-17).
//
// PaperUI's controls are hit-tested against the mouse pointer, so ImGui's gamepad navigation (which walks ImGui widgets)
// has nothing to move through. This moves the pointer instead: every control announces its rectangle when it asks "is the
// pointer over me?" (PaperUI::hovered), the D-pad / left stick move the pointer to the nearest control in that direction,
// A presses where it stands (a left click, so a field raises the keyboard and a dropdown opens), B is Escape and Start is
// Enter. The control under the pointer already draws as hovered, and a ring marks it while the pad is in use.
//
// The geometry is plain C++ with no ImGui types (pickNext, normalise), so it is unit-tested on the host
// (tools/vita/depcheck/papernav_test.cpp).

#include <cstddef>
#include <vector>

struct ImDrawList;

namespace wowee::ui {

struct NavRect {
    float x0, y0, x1, y1;
    [[nodiscard]] float cx() const { return (x0 + x1) * 0.5f; }
    [[nodiscard]] float cy() const { return (y0 + y1) * 0.5f; }
    [[nodiscard]] float area() const { return (x1 - x0) * (y1 - y0); }
    [[nodiscard]] bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    [[nodiscard]] bool operator==(const NavRect&) const = default;
};

enum class NavDir { Up, Down, Left, Right };

/// Drop what is not a target: rectangles with a smaller one inside them (a panel, a margin claiming the pointer), exact
/// duplicates, and empty ones. Order of the survivors is kept.
std::vector<NavRect> normaliseNavRects(const std::vector<NavRect>& rects);

/// The control a pointer at (x, y) is on: the smallest rectangle containing it, or -1.
int navCurrent(const std::vector<NavRect>& rects, float x, float y);

/// The control to move to from `current` (-1 = none yet) in a direction, or `current` if nothing lies that way. With no
/// current control, the topmost-leftmost one.
int pickNext(const std::vector<NavRect>& rects, int current, NavDir dir);

class PaperNav {
public:
    void beginFrame();
    void addRect(float x0, float y0, float x1, float y1);
    /// Reads the pad, moves the pointer, clicks. `popupRows` replaces the targets while a dropdown is open.
    void endFrame(ImDrawList* overlay, const std::vector<NavRect>* popupRows);

private:
    std::vector<NavRect> rects_;
    bool releasePending_ = false;
    bool padActive_ = false;
};

}  // namespace wowee::ui
