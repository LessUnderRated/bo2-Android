// game_host.cpp — the recompiled game inside the shell: the window, the Vulkan backend's hooks, input, and the
// environment bo2-mac's runtime reads. main.cpp calls run_game() when the game core is linked (bo2_game_main).
//
// Two threads. SDL's own (SDLThread, where SDL_main runs and where SDL wants its events polled) owns the window, the
// events, the touch controls and the controller bridge. The game runs on a thread of its own with a 64 MiB stack:
// SDLThread is a Java thread (about 1 MiB), and lifted code makes one host call per guest call, where bo2-mac runs on
// macOS's 8 MiB main thread. The game's gfx_pump_once calls hook_pump(), which only reads what SDLThread published;
// input reaches the game through the backend's locked queue (bo2vk_input_push), the pad through pad_sdl.cpp's mutex.
#include "game_host.hpp"

#include <SDL3/SDL.h>
#include <android/native_window.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

#include "display_plan.hpp"
#include "log.hpp"
#include "graphics.hpp"
#include "perf.hpp"
#include "prof.hpp"
#include "touch_pad.hpp"
#include "vk_hooks.h"
#include "core_loader.hpp"

extern "C" void bo2_pad_pump();
extern "C" void bo2_pad_rumble_pump();
extern "C" void bo2_graphics_apply(const char* game_dir);
extern "C" void bo2_pad_top();   // runtime/input/pad_sdl.cpp: gpad_enabled 1 again if a pad is connected
void bo2_adpf_frame();   // adpf.cpp: Android performance hints for the frame's threads
extern "C" void bo2_view_distance_tick(int64_t now_ms);   // graphics_apply.cpp: r_zfar kept in step with the map
extern "C" void bo2_fx_ab_tick(int64_t now_ms);   // graphics_apply.cpp: the BO2_FX_AB effects measurement

namespace {

SDL_Window* g_window = nullptr;
std::atomic<bool> g_foreground{true};
std::atomic<bool> g_quit{false};
std::atomic<bool> g_game_done{false};
bo2display::Plan g_plan{};

// SDL scancode -> Win32 virtual-key code, for a keyboard (Bluetooth or USB) on the phone.
uint32_t vk_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s == SDL_SCANCODE_0) return '0';
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return 0x61 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
        case SDL_SCANCODE_ESCAPE: return 0x1B;
        case SDL_SCANCODE_BACKSPACE: return 0x08;
        case SDL_SCANCODE_TAB: return 0x09;
        case SDL_SCANCODE_SPACE: return 0x20;
        case SDL_SCANCODE_MINUS: return 0xBD;
        case SDL_SCANCODE_EQUALS: return 0xBB;
        case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
        case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
        case SDL_SCANCODE_BACKSLASH: return 0xDC;
        case SDL_SCANCODE_SEMICOLON: return 0xBA;
        case SDL_SCANCODE_APOSTROPHE: return 0xDE;
        case SDL_SCANCODE_GRAVE: return 0xC0;
        case SDL_SCANCODE_COMMA: return 0xBC;
        case SDL_SCANCODE_PERIOD: return 0xBE;
        case SDL_SCANCODE_SLASH: return 0xBF;
        case SDL_SCANCODE_CAPSLOCK: return 0x14;
        case SDL_SCANCODE_RIGHT: return 0x27;
        case SDL_SCANCODE_LEFT: return 0x25;
        case SDL_SCANCODE_DOWN: return 0x28;
        case SDL_SCANCODE_UP: return 0x26;
        case SDL_SCANCODE_INSERT: return 0x2D;
        case SDL_SCANCODE_HOME: return 0x24;
        case SDL_SCANCODE_PAGEUP: return 0x21;
        case SDL_SCANCODE_DELETE: return 0x2E;
        case SDL_SCANCODE_END: return 0x23;
        case SDL_SCANCODE_PAGEDOWN: return 0x22;
        case SDL_SCANCODE_KP_0: return 0x60;
        case SDL_SCANCODE_LCTRL: return 0xA2;
        case SDL_SCANCODE_RCTRL: return 0xA3;
        case SDL_SCANCODE_LSHIFT: return 0xA0;
        case SDL_SCANCODE_RSHIFT: return 0xA1;
        case SDL_SCANCODE_LALT: return 0xA4;
        case SDL_SCANCODE_RALT: return 0xA5;
        default: return 0;
    }
}
// The generic VK the game also checks for a left/right modifier (VK_SHIFT, VK_CONTROL, VK_MENU).
uint32_t vk_generic(uint32_t vk) {
    switch (vk) {
        case 0xA0: case 0xA1: return 0x10;
        case 0xA2: case 0xA3: return 0x11;
        case 0xA4: case 0xA5: return 0x12;
        default: return 0;
    }
}

// Window pixels -> the guest monitor's pixels, through the display plan's rectangle.
uint32_t mouse_lparam(float x, float y) {
    const bo2display::Plan& p = g_plan;
    float gx = x, gy = y;
    if (p.dest.w > 0 && p.dest.h > 0) {
        gx = (x - p.dest.x) * p.guest_w / p.dest.w;
        gy = (y - p.dest.y) * p.guest_h / p.dest.h;
    }
    const int ix = (int)SDL_clamp(gx, 0.f, (float)(p.guest_w > 0 ? p.guest_w - 1 : 0));
    const int iy = (int)SDL_clamp(gy, 0.f, (float)(p.guest_h > 0 ? p.guest_h - 1 : 0));
    return (uint32_t)(ix & 0xFFFF) | ((uint32_t)(iy & 0xFFFF) << 16);
}

void key(uint32_t vk, uint32_t scancode, bool down, bool repeat) {
    if (!vk) return;
    const uint32_t lp = 1u | ((scancode & 0xFF) << 16) | (repeat ? (1u << 30) : 0u) | (down ? 0u : (3u << 30));
    bo2vk_input_push(down ? 0x0100 : 0x0101, vk, lp);   // WM_KEYDOWN / WM_KEYUP
    if (const uint32_t g = vk_generic(vk)) bo2vk_input_push(down ? 0x0100 : 0x0101, g, lp);
}

// ---- Hooks ------------------------------------------------------------------------------------------------------------
// The display's refresh to the frame cap (60 Hz for a 60 fps cap): each game frame then lands on one refresh of its
// own. On the S26's 120 Hz panel a 60 fps stream otherwise sits on 2 refreshes, or 1 or 3 when it drifts.
void set_display_rate(ANativeWindow* win) {
    if (!win) return;
    const int cap = bo2gfx::settings().frame_cap;
    const int r = ANativeWindow_setFrameRate(win, cap > 0 ? (float)cap : 0.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
    std::fprintf(stderr, "[GAME] display refresh asked for %d fps content: %s\n", cap, r == 0 ? "ok" : "refused");
}

void* hook_native_window() {
    if (!g_window) return nullptr;
    void* win = SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
    set_display_rate(static_cast<ANativeWindow*>(win));   // a new surface (start, back from the background)
    return win;
}
void hook_panel_size(int* w, int* h) {
    int pw = 0, ph = 0;
    if (g_window) SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    *w = pw; *h = ph;
}
void hook_dest_rect(int pw, int ph, int* x, int* y, int* w, int* h) {
    g_plan = bo2display::plan(pw, ph, bo2display::settings());
    *x = g_plan.dest.x; *y = g_plan.dest.y; *w = g_plan.dest.w; *h = g_plan.dest.h;
}
int hook_frame_cap() { return bo2gfx::settings().frame_cap; }
void hook_frame_presented() {
    bo2perf::frame_presented();
    bo2_adpf_frame();   // adpf.cpp: the frame's work, for the CPU governor
    bo2_view_distance_tick((int64_t)SDL_GetTicks());   // every 2 s: the view distance against the map's own
    bo2_fx_ab_tick((int64_t)SDL_GetTicks());           // off unless BO2_FX_AB is set
}
int hook_foreground() { return g_foreground.load() ? 1 : 0; }
double hook_refresh_hz() {
    const SDL_DisplayMode* m = g_window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window)) : nullptr;
    return m && m->refresh_rate > 0 ? m->refresh_rate : 60.0;
}

// SDLThread: one event, translated.
void sdl_event(const SDL_Event& e) {
    {
        {
            switch (e.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_TERMINATING:
                    g_quit = true;
                    break;
                case SDL_EVENT_WILL_ENTER_BACKGROUND:
                case SDL_EVENT_DID_ENTER_BACKGROUND:
                    g_foreground = false;
                    break;
                case SDL_EVENT_DID_ENTER_FOREGROUND:
                    if (!g_foreground.load()) bo2vk_surface_changed();   // Android made a new surface
                    g_foreground = true;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST: g_foreground = false; break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED: g_foreground = true; break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP:
                    // The phone's Back key is the app's menu (Bo2Activity), never the game's.
                    if (e.key.scancode != SDL_SCANCODE_AC_BACK)
                        key(vk_of(e.key.scancode), (uint32_t)e.key.scancode, e.type == SDL_EVENT_KEY_DOWN, e.key.repeat);
                    break;
                case SDL_EVENT_TEXT_INPUT:
                    for (const unsigned char* c = (const unsigned char*)e.text.text; c && *c; c++)
                        if (*c < 0x80) bo2vk_input_push(0x0102, *c, 1);   // WM_CHAR (ASCII; the game's text is ASCII)
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (e.motion.which != SDL_TOUCH_MOUSEID) bo2vk_input_push(0x0200, 0, mouse_lparam(e.motion.x, e.motion.y));
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP: {
                    if (e.button.which == SDL_TOUCH_MOUSEID) break;   // touches are the touch pad's
                    const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    const uint32_t msg = e.button.button == SDL_BUTTON_RIGHT ? (down ? 0x0204 : 0x0205)
                                       : e.button.button == SDL_BUTTON_MIDDLE ? (down ? 0x0207 : 0x0208)
                                                                               : (down ? 0x0201 : 0x0202);
                    bo2vk_input_push(msg, 0, mouse_lparam(e.button.x, e.button.y));
                    break;
                }
                case SDL_EVENT_MOUSE_WHEEL:
                    bo2vk_input_push(0x020A, (uint32_t)((int16_t)(e.wheel.y * 120)) << 16, 0);
                    break;
                default: break;
            }
        }
    }
}

// The game thread, once per gfx_pump_once: 0 when the app is closing; in the background, wait (Android took the
// surface away; the game resumes where it was).
int hook_pump() {
    while (!g_quit.load() && !g_foreground.load()) usleep(50000);
    return g_quit.load() ? 0 : 1;
}

struct GameArgs { int argc; char** argv; int status; };
void* game_thread(void* p) {
    GameArgs* a = static_cast<GameArgs*>(p);
    a->status = bo2core::api().game_main(a->argc, a->argv);
    g_game_done.store(true);
    return nullptr;
}

std::string internal_dir() {
    const char* p = SDL_GetAndroidInternalStoragePath();
    return p ? p : ".";
}

// ---- Graphics settings to the game (docs/GRAPHICS.md) ----
// The game's own settings are sent once it has run its config (the main menu reached), again as each map starts to
// load (a profile load may have set them back; texture detail only takes effect for textures loaded afterwards), and
// when the player changes them. On a worker thread: the observer runs on the log thread, which must not block.
std::string g_gfx_game_dir;
std::atomic<bool> g_menu_seen{false};
std::atomic<int64_t> g_gfx_map_ms{0};

void apply_graphics_async(const char* why) {
    std::thread([why] {
        std::fprintf(stderr, "[GFX] applying the graphics settings (%s)\n", why);
        // The model LOD-distance scales are cheat-protected in the game: the runtime lifts that flag on those two
        // dvars before their `set` lines are queued (bo2-mac win32_runtime.cpp). Absent without the game core.
        if (auto unlock = bo2core::api().dvar_unlock) unlock();
        bo2_graphics_apply(g_gfx_game_dir.c_str());
    }).detach();
}

void on_game_line(const char* line) {
    if (!g_menu_seen.load()) {
        if (std::strstr(line, "Can play online") || std::strstr(line, "Content discovery taking too long")) {
            g_menu_seen.store(true);
            bo2_pad_top();   // the game's own settings are loaded now: a pad connected before the start is enabled again
            apply_graphics_async("main menu");
        }
        return;
    }
    if (std::strstr(line, "Loading fastfile 'zm_")) {   // a map's zones load in a burst: once per burst
        const int64_t now = (int64_t)SDL_GetTicks();
        if (now - g_gfx_map_ms.exchange(now) > 20000) { bo2_pad_top(); apply_graphics_async("map loading"); }
    }
}

}  // namespace

// ☰ → Graphics changed while the game runs (jni.cpp): send the new settings now.
extern "C" void bo2_graphics_changed() {
    bo2vk_set_upscaler(bo2gfx::settings().upscaler);   // the presentation's, read at the next present
    if (g_window)
        set_display_rate(static_cast<ANativeWindow*>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr)));
    if (g_menu_seen.load()) apply_graphics_async("settings changed");
}

// The game core, loaded once from the app's copy of it (Bo2App sets BO2_CORE; core_loader.cpp).
bool game_linked() { return bo2core::load(std::getenv("BO2_CORE"), nullptr); }

int run_game(const std::string& game_dir, int argc, char** argv) {
    // What the recompiled Zombies cannot start without: say which, rather than fail inside the runtime.
    std::string missing;
    for (const char* need : {"pe_sections.bin", "zone", "sound", "t6zm.exe"}) {
        struct stat st {};
        if (::stat((game_dir + "/" + need).c_str(), &st) != 0) missing += std::string(missing.empty() ? "" : ", ") + need;
    }
    if (!missing.empty()) {
        const std::string why = "Missing from " + game_dir + ": " + missing +
            ".\n\nCopy your game folder and pe_sections.bin (from the --lift build) with scripts/install-phone.ps1 "
            "or build-android.sh --push-data.";
        std::fprintf(stderr, "[GAME] %s\n", why.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Black Ops II", why.c_str(), nullptr);
        return 1;
    }
    // The window, fullscreen under the cutout (as the diagnostics screen's).
    g_window = SDL_CreateWindow("Black Ops II", 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN);
    if (!g_window) {
        std::fprintf(stderr, "[GAME] SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    g_plan = bo2display::plan(pw, ph, bo2display::settings());

    // What the runtime reads. The guest monitor is the display plan's (bo2-mac guest_monitor.h on Android).
    const std::string internal = internal_dir();
    const std::string cache = internal + "/shader_cache";
    ::mkdir(cache.c_str(), 0700);
    setenv("BO2_GAME_DIR", game_dir.c_str(), 1);
    setenv("BO2_PE_SECTIONS", (game_dir + "/pe_sections.bin").c_str(), 1);
    setenv("BO2_SHADER_CACHE_DIR", cache.c_str(), 1);
    setenv("BO2_REFUSAL_FILE", (internal + "/refusal.txt").c_str(), 1);
    setenv("HOME", internal.c_str(), 1);
    setenv("BO2_GUEST_W", std::to_string(g_plan.guest_w).c_str(), 1);
    setenv("BO2_GUEST_H", std::to_string(g_plan.guest_h).c_str(), 1);
    int nloc = 0;
    if (SDL_Locale** loc = SDL_GetPreferredLocales(&nloc)) {
        if (nloc > 0 && loc[0] && loc[0]->language) setenv("BO2_LANG", loc[0]->language, 1);
        SDL_free(loc);
    }
    // The game's working directory is its folder, as the exe's is on Windows.
    if (::chdir(game_dir.c_str()) != 0) std::fprintf(stderr, "[GAME] chdir %s failed\n", game_dir.c_str());
    std::fprintf(stderr, "[GAME] panel %dx%d, the game sees a %dx%d monitor, drawn at %d,%d %dx%d; data in %s\n", pw, ph,
                 g_plan.guest_w, g_plan.guest_h, g_plan.dest.x, g_plan.dest.y, g_plan.dest.w, g_plan.dest.h,
                 game_dir.c_str());

    g_gfx_game_dir = game_dir;
    bo2log::set_line_observer(on_game_line);

    Bo2VkShellHooks h{};
    h.native_window = hook_native_window;
    h.panel_size = hook_panel_size;
    h.dest_rect = hook_dest_rect;
    h.frame_cap = hook_frame_cap;
    h.frame_presented = hook_frame_presented;
    h.pump = hook_pump;
    h.foreground = hook_foreground;
    h.refresh_hz = hook_refresh_hz;
    bo2vk_set_hooks(&h);
    bo2vk_set_upscaler(bo2gfx::settings().upscaler);
    SDL_StartTextInput(g_window);

    bo2prof::start();   // per-thread CPU, GPU/clocks/battery and a sampling profile, as log lines (prof.cpp)

    GameArgs args{argc, argv, 1};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t game;
    if (pthread_create(&game, &attr, game_thread, &args) != 0) {
        std::fprintf(stderr, "[GAME] the game thread could not start\n");
        return 1;
    }
    pthread_attr_destroy(&attr);
    pthread_setname_np(game, "bo2_game");
    // SDLThread from here: events, touch, the controller bridge, until the game returns.
    // A fixed 120 Hz tick, sleeping between turns. SDL_WaitEventTimeout(…, 4) did not sleep on Android: simpleperf on
    // the S26 (in a match) had this thread at 82% of a core, a third of it in SDL's timed mutex wait and the joystick
    // lock (each turn pushes and reads ~40 pad values under it). The game reads the pad once per frame; 8 ms is enough.
    using clock = std::chrono::steady_clock;
    constexpr auto kTick = std::chrono::microseconds(8333);
    auto next = clock::now();
    while (!g_game_done.load()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) sdl_event(e);
        bo2touch::pad_pump();
        bo2_pad_pump();
        bo2_pad_rumble_pump();
        next += kTick;
        const auto now = clock::now();
        if (next < now) next = now;   // fell behind (a long event): no catch-up burst
        else std::this_thread::sleep_until(next);
    }
    pthread_join(game, nullptr);
    const int status = args.status;
    std::fprintf(stderr, "[GAME] bo2_game_main returned %d\n", status);
    if (FILE* f = std::fopen((internal + "/refusal.txt").c_str(), "r")) {   // the Steam layer refused to start
        char why[512] = {};
        std::fread(why, 1, sizeof why - 1, f);
        std::fclose(f);
        std::fprintf(stderr, "[GAME] refused: %s\n", why);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Black Ops II", why, g_window);
        std::remove((internal + "/refusal.txt").c_str());
    }
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
    return status;
}
