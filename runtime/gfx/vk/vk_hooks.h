// vk_hooks.h — what the Android shell gives the Vulkan backend, so the backend needs neither SDL nor the shell's code.
//
// The shell owns the window (SDL), the touch controls, the display plan, the frame cap and [PERF]. It fills these
// hooks before the game starts (android/native/src/game_host.cpp) and feeds input with bo2vk_input_push. With no
// hooks (the PC tests), the backend runs headless: no window, no swapchain, frames rendered off screen.
#pragma once
#include <cstdint>

extern "C" {

struct Bo2VkShellHooks {
    // The ANativeWindow* to present to (Android), or null for no window.
    void* (*native_window)(void);
    // The window in pixels, and where the game's image goes in it (the display plan: fill, or 16:9 with bars).
    void (*panel_size)(int* w, int* h);
    void (*dest_rect)(int panel_w, int panel_h, int* x, int* y, int* w, int* h);
    // Frames per second the presentation is paced to (0 = only v-sync).
    int (*frame_cap)(void);
    // Called after every present (the shell's [PERF] counts frames here).
    void (*frame_presented)(void);
    // Called once per gfx_pump_once before anything else: the shell drains SDL events, runs the touch pad and the
    // controller bridge, and pushes keyboard and mouse input. Returns 0 when the app is closing.
    int (*pump)(void);
    // 1 while the app is in the foreground (focus messages go to the game on every change).
    int (*foreground)(void);
    // The display's refresh rate in Hz (gfx_refresh_hz, which the runtime reports to the game).
    double (*refresh_hz)(void);
};

// Before gfx_window_open. The struct is copied.
void bo2vk_set_hooks(const struct Bo2VkShellHooks* hooks);

// A Win32 message for the game (WM_KEYDOWN, WM_MOUSEMOVE, ...), as the macOS backend's Cocoa translation makes them.
void bo2vk_input_push(uint32_t msg, uint32_t wparam, uint32_t lparam);
// Moves the game's pointer by (dx, dy) mouse counts: mouse look turns the view by that much (touch drag look).
void bo2vk_mouse_rel(int dx, int dy);

// The window's surface changed (Android destroys it in the background and makes a new one on return): the next
// pump makes a new Vulkan surface from native_window() and a new swapchain.
void bo2vk_surface_changed(void);

// How the presentation scales the back buffer up to the display plan's rectangle (Graphics -> Upscaler): 0 bilinear,
// 1 SGSR (Snapdragon Game Super Resolution 1, the default). Any thread, any time: read at each present. The same size
// is always a plain copy. BO2_UPSCALER sets the value at start.
void bo2vk_set_upscaler(int mode);

// The BO2_FX_AB measurement (graphics_apply.cpp): 1 drops the particle-like draws (vk_capture.cpp, [FX-DRAWS]) before
// any copy, 0 draws them again. Any thread, any time.
void bo2vk_set_fx_hidden(int on);

// Tests: the last rendered back buffer, as RGBA8 (converted from its format), after a gfx_pump_once.
int bo2vk_read_backbuffer(uint8_t* rgba, uint32_t capacity, uint32_t* w, uint32_t* h);
}

namespace bo2vk {
struct ShellHooks : Bo2VkShellHooks {};
}
