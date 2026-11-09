// display_plan.cpp — see display_plan.hpp.
#include "display_plan.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace bo2display {

namespace {

// Render sizes stay multiples of 8: D3D11 block-compressed targets and most GPUs' tiles prefer it,
// and an odd width would make the game's half-resolution passes round differently per axis.
int round8(double v) { return std::max(8, static_cast<int>(std::lround(v / 8.0)) * 8); }

std::atomic<int> g_aspect{0};
std::atomic<int> g_height{1080};   // Bo2Activity.DEFAULT_HEIGHT

}  // namespace

Plan plan(int panel_w, int panel_h, const Settings& s) {
    Plan p;
    // The game is landscape whatever the window reports first (SDL can report portrait while rotating).
    p.panel_w = std::max(panel_w, panel_h);
    p.panel_h = std::min(panel_w, panel_h);
    if (p.panel_w <= 0 || p.panel_h <= 0) {
        p.panel_w = 1920;
        p.panel_h = 1080;
    }

    // The shape the game renders in, and the largest rectangle of that shape on the panel.
    const double shape = s.aspect == Aspect::Wide16x9 ? 16.0 / 9.0 : static_cast<double>(p.panel_w) / p.panel_h;
    int dw = p.panel_w, dh = p.panel_h;
    if (static_cast<double>(p.panel_w) / p.panel_h > shape) {
        dw = static_cast<int>(std::lround(p.panel_h * shape));
    } else {
        dh = static_cast<int>(std::lround(p.panel_w / shape));
    }
    p.dest = Rect{(p.panel_w - dw) / 2, (p.panel_h - dh) / 2, dw, dh};

    // Render height: as asked, never above what lands on the panel (more would be thrown away).
    const int height = s.render_height <= 0 ? dh : std::min(s.render_height, dh);
    p.guest_h = round8(height);
    p.guest_w = round8(p.guest_h * shape);
    if (p.guest_h > dh) p.guest_h = dh;  // rounding may not push past the panel
    if (p.guest_w > dw) p.guest_w = dw;
    p.aspect = static_cast<float>(p.guest_w) / p.guest_h;
    return p;
}

void set_settings(const Settings& s) {
    g_aspect.store(static_cast<int>(s.aspect));
    g_height.store(s.render_height);
}

Settings settings() {
    Settings s;
    s.aspect = static_cast<Aspect>(g_aspect.load());
    s.render_height = g_height.load();
    return s;
}

}  // namespace bo2display
