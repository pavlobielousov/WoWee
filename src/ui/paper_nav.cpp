// See paper_nav.hpp (VITA-17).
#include "ui/paper_nav.hpp"

#ifndef PAPER_NAV_GEOMETRY_ONLY  // the host test builds only the pure geometry
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>

namespace wowee::ui {

std::vector<NavRect> normaliseNavRects(const std::vector<NavRect>& rects) {
    std::vector<NavRect> out;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const NavRect& r = rects[i];
        if (r.x1 - r.x0 < 1.0f || r.y1 - r.y0 < 1.0f) continue;
        if (std::find(out.begin(), out.end(), r) != out.end()) continue;
        bool holdsAnother = false;
        for (std::size_t j = 0; j < rects.size() && !holdsAnother; ++j) {
            if (j == i) continue;
            const NavRect& s = rects[j];
            holdsAnother = s != r && s.area() < r.area() && s.x0 >= r.x0 && s.y0 >= r.y0 && s.x1 <= r.x1 && s.y1 <= r.y1;
        }
        if (!holdsAnother) out.push_back(r);
    }
    return out;
}

int navCurrent(const std::vector<NavRect>& rects, float x, float y) {
    int best = -1;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        if (!rects[i].contains(x, y)) continue;
        if (best < 0 || rects[i].area() < rects[static_cast<std::size_t>(best)].area()) best = static_cast<int>(i);
    }
    return best;
}

int pickNext(const std::vector<NavRect>& rects, int current, NavDir dir) {
    if (rects.empty()) return -1;
    if (current < 0 || current >= static_cast<int>(rects.size())) {
        int first = 0;
        for (std::size_t i = 1; i < rects.size(); ++i) {
            const NavRect& a = rects[i];
            const NavRect& b = rects[static_cast<std::size_t>(first)];
            if (a.cy() < b.cy() - 0.5f || (std::fabs(a.cy() - b.cy()) <= 0.5f && a.cx() < b.cx())) first = static_cast<int>(i);
        }
        return first;
    }
    const NavRect& cur = rects[static_cast<std::size_t>(current)];
    int best = current;
    float bestScore = 1.0e30f;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        if (static_cast<int>(i) == current) continue;
        const NavRect& r = rects[i];
        // forward: how far past the current control's facing edge the candidate starts; side: how far off the axis.
        float forward = 0, side = 0;
        switch (dir) {
            case NavDir::Up: forward = cur.cy() - r.cy(); side = std::fabs(r.cx() - cur.cx()); break;
            case NavDir::Down: forward = r.cy() - cur.cy(); side = std::fabs(r.cx() - cur.cx()); break;
            case NavDir::Left: forward = cur.cx() - r.cx(); side = std::fabs(r.cy() - cur.cy()); break;
            case NavDir::Right: forward = r.cx() - cur.cx(); side = std::fabs(r.cy() - cur.cy()); break;
        }
        if (forward < 1.0f) continue;
        // Sideways moves stay in their row (a narrow cone): "right" from a button is the button beside it, not the link
        // below and a little to the right. Up and down take any control further on, since rows span the card.
        const bool horizontal = dir == NavDir::Left || dir == NavDir::Right;
        const bool inCone = !horizontal || side <= 0.6f * forward;
        if (!inCone) continue;
        // Distance, not a weighted sum: a control two rows down and in line must not beat the nearer row that is a
        // little off to the side (the password field's "down" is the buttons, not the link under them).
        const float score = forward * forward + side * side;
        if (score < bestScore) {
            bestScore = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

#ifndef PAPER_NAV_GEOMETRY_ONLY
void PaperNav::beginFrame() { rects_.clear(); }

void PaperNav::addRect(float x0, float y0, float x1, float y1) { rects_.push_back({x0, y0, x1, y1}); }

void PaperNav::endFrame(ImDrawList* overlay, const std::vector<NavRect>* popupRows) {
    ImGuiIO& io = ImGui::GetIO();
    if (!(io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad)) return;

    // The press started last frame ends now; the click needs a frame in between to be seen as a click.
    if (releasePending_) {
        io.AddMouseButtonEvent(0, false);
        releasePending_ = false;
    }

    const std::vector<NavRect> targets = normaliseNavRects(popupRows ? *popupRows : rects_);
    const auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };

    bool moved = false;
    NavDir dir = NavDir::Down;
    if (key(ImGuiKey_GamepadDpadUp) || key(ImGuiKey_GamepadLStickUp)) { dir = NavDir::Up; moved = true; }
    else if (key(ImGuiKey_GamepadDpadDown) || key(ImGuiKey_GamepadLStickDown)) { dir = NavDir::Down; moved = true; }
    else if (key(ImGuiKey_GamepadDpadLeft) || key(ImGuiKey_GamepadLStickLeft)) { dir = NavDir::Left; moved = true; }
    else if (key(ImGuiKey_GamepadDpadRight) || key(ImGuiKey_GamepadLStickRight)) { dir = NavDir::Right; moved = true; }

    int current = navCurrent(targets, io.MousePos.x, io.MousePos.y);
    if (moved) {
        padActive_ = true;
        const int next = pickNext(targets, current, dir);
        if (next >= 0 && next != current) {
            io.AddMousePosEvent(targets[static_cast<std::size_t>(next)].cx(), targets[static_cast<std::size_t>(next)].cy());
            current = next;
        } else if (next >= 0 && current < 0) {
            io.AddMousePosEvent(targets[static_cast<std::size_t>(next)].cx(), targets[static_cast<std::size_t>(next)].cy());
            current = next;
        }
    }
    if (padActive_ && key(ImGuiKey_GamepadFaceDown) && current >= 0) {
        const NavRect& r = targets[static_cast<std::size_t>(current)];
        io.AddMousePosEvent(r.cx(), r.cy());
        io.AddMouseButtonEvent(0, true);
        releasePending_ = true;
    }
    if (key(ImGuiKey_GamepadFaceRight)) {
        padActive_ = true;
        io.AddKeyEvent(ImGuiKey_Escape, true);
        io.AddKeyEvent(ImGuiKey_Escape, false);
    }
    if (key(ImGuiKey_GamepadStart)) {
        padActive_ = true;
        io.AddKeyEvent(ImGuiKey_Enter, true);
        io.AddKeyEvent(ImGuiKey_Enter, false);
    }

    // The ring marks the control the next press lands on, while the pad is the one driving.
    if (padActive_ && current >= 0 && overlay) {
        const NavRect& r = targets[static_cast<std::size_t>(current)];
        overlay->AddRect(ImVec2(r.x0 - 3, r.y0 - 3), ImVec2(r.x1 + 3, r.y1 + 3), IM_COL32(199, 156, 33, 230), 4.0f, 0, 2.5f);
    }
}

#endif  // PAPER_NAV_GEOMETRY_ONLY

}  // namespace wowee::ui
