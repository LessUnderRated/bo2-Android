// display_plan.hpp — what size the game renders at and where that lands on the phone's panel.
//
// bo2-mac shows the game one fake monitor, 1920x1080 (runtime/win32/guest_monitor.h), reports it
// identically through user32 (GetSystemMetrics, EnumDisplaySettings, GetMonitorInfo) and DXGI
// (GetDisplayModeList: the only mode it announces), renders in a 1280x720 window, and scales the image
// uniformly with bars. On a Galaxy S26 (2340x1080, 19.5:9) that would leave black bars down both sides.
//
// On Android the announced monitor comes from this plan instead: the panel's own aspect at the render
// height the player picks, so the game renders straight into the shape of the screen. Keeping bo2-mac's
// rule (P657), every API reports the one same monitor, and it is the one mode DXGI announces, so the
// game's own mode choice can only land on it.
//
// FILL (default): the render covers the whole panel, camera cutout included (SDL already lays the
//   window out under the cutout in fullscreen); the game's own HUD placement then decides what the
//   punch-hole covers. WIDE_16_9: bo2-mac's look, 16:9 centred with bars.
#pragma once

#include <cstdint>

namespace bo2display {

enum class Aspect : int { Fill = 0, Wide16x9 = 1 };

struct Settings {
    Aspect aspect = Aspect::Fill;
    // Render height in pixels; 0 or anything above the panel's means the panel's own height (native).
    int render_height = 720;
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Plan {
    int panel_w = 0, panel_h = 0;  // the window in pixels (the whole panel in fullscreen)
    int guest_w = 0, guest_h = 0;  // the monitor the game is shown, and renders at
    Rect dest;                     // where the game's image is drawn on the panel
    float aspect = 0;              // guest_w / guest_h
};

// The plan for a panel of panel_w x panel_h pixels (landscape or not; the game is landscape).
Plan plan(int panel_w, int panel_h, const Settings& settings);

// The process-wide settings the menu sets and the presentation reads (any thread).
void set_settings(const Settings& settings);
Settings settings();

}  // namespace bo2display
