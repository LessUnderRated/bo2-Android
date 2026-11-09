// main.cpp — SDL_main of libmain.so, the library SDLActivity loads.
//
// STATUS: the recompiled game is not linked into this library yet (README.md, "Status"). Until it
// is, this runs the shell's diagnostics screen, which exercises everything the game will stand on:
//   - the touch controls and any connected controller, read back as the 16 XINPUT_STATE bytes the
//     game's XInputGetState will receive (runtime/input/pad_sdl.cpp) — the whole input path;
//   - the GPU and driver (the S26's Adreno or Xclipse question), from Vulkan;
//   - the imported game data under the app's external files directory;
//   - frame pacing, logged as [PERF] lines every five seconds with the battery's power draw.
// When the game core loads (core_loader.cpp, game_linked()), the game replaces the screen.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "game_host.hpp"
#include "device_report.hpp"
#include "display_plan.hpp"
#include "graphics.hpp"
#include "test_card.hpp"
#include "log.hpp"
#include "perf.hpp"
#include "touch_pad.hpp"

extern "C" void bo2_pad_pump();
extern "C" void bo2_pad_rumble_pump();
extern "C" uint32_t bo2_pad_etat(uint32_t idx, uint8_t out[16]);
extern "C" void bo2_graphics_apply(const char* game_dir);

namespace {

std::string data_dir() {
    const char* external = SDL_GetAndroidExternalStoragePath();
    return std::string(external ? external : ".") + "/game";
}

bool exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

// What the setup screen imported, as a line for the screen and the log.
std::string data_summary(const std::string& dir) {
    std::string out;
    for (const char* entry : {"zone", "sound", "video", "players", "t6zm.exe", "t6mp.exe", "t6sp.exe"}) {
        out += entry;
        out += exists(dir + "/" + entry) ? " yes  " : " -  ";
    }
    return out;
}

void text(SDL_Renderer* r, float x, float y, float scale, const std::string& s) {
    SDL_SetRenderScale(r, scale, scale);
    SDL_RenderDebugText(r, x / scale, y / scale, s.c_str());
    SDL_SetRenderScale(r, 1, 1);
}

void bar(SDL_Renderer* r, float x, float y, float w, float h, float value /* -1..1 */) {
    SDL_FRect back{x, y, w, h};
    SDL_SetRenderDrawColor(r, 60, 60, 70, 255);
    SDL_RenderFillRect(r, &back);
    const float mid = x + w / 2;
    SDL_FRect fill{value >= 0 ? mid : mid + value * w / 2, y, std::abs(value) * w / 2, h};
    SDL_SetRenderDrawColor(r, 255, 140, 40, 255);
    SDL_RenderFillRect(r, &fill);
}

int diagnostics() {
    SDL_Window* window = SDL_CreateWindow("Black Ops II", 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    std::fprintf(stderr, "[SHELL] renderer %s\n", SDL_GetRendererName(renderer));

    bo2device::Gpu gpu;
    const std::string gpu_lines = bo2device::gpu_report(&gpu);
    std::fprintf(stderr, "[SHELL] %s[SHELL] S26 variant: %s\n", gpu_lines.c_str(), bo2device::s26_variant(gpu.vendor_id));
    const std::string dir = data_dir();
    const std::string data = data_summary(dir);
    std::fprintf(stderr, "[SHELL] game data in %s: %s\n", dir.c_str(), data.c_str());
    // What the graphics settings would tell the game, checked against the imported config files.
    bo2_graphics_apply(dir.c_str());
    int applied_cap = -2;
    double next_frame_ms = 0;

    SDL_Texture* card = nullptr;
    bo2display::Plan shown{};
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING) running = false;
        }
        bo2touch::pad_pump();
        bo2_pad_pump();
        bo2_pad_rumble_pump();

        uint8_t xi[16] = {};
        const bool connected = bo2_pad_etat(0, xi) == 0;
        uint32_t packet;
        uint16_t buttons;
        int16_t lx, ly, rx, ry;
        std::memcpy(&packet, xi, 4);
        std::memcpy(&buttons, xi + 4, 2);
        std::memcpy(&lx, xi + 8, 2);
        std::memcpy(&ly, xi + 10, 2);
        std::memcpy(&rx, xi + 12, 2);
        std::memcpy(&ry, xi + 14, 2);

        int w = 0, h = 0;
        SDL_GetRenderOutputSize(renderer, &w, &h);
        const bo2display::Plan plan = bo2display::plan(w, h, bo2display::settings());
        if (!card || plan.guest_w != shown.guest_w || plan.guest_h != shown.guest_h || plan.dest.w != shown.dest.w ||
            plan.dest.x != shown.dest.x || plan.panel_w != shown.panel_w) {
            SDL_Rect safe{};
            SDL_GetWindowSafeArea(window, &safe);
            std::fprintf(stderr,
                         "[DISPLAY] panel %dx%d, safe area %d,%d %dx%d (window units); game renders %dx%d (%.3f:1), "
                         "drawn at %d,%d %dx%d\n",
                         plan.panel_w, plan.panel_h, safe.x, safe.y, safe.w, safe.h, plan.guest_w, plan.guest_h,
                         plan.aspect, plan.dest.x, plan.dest.y, plan.dest.w, plan.dest.h);
            if (card) SDL_DestroyTexture(card);
            card = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, plan.guest_w,
                                     plan.guest_h);
            shown = plan;
        }
        if (card) {
            SDL_SetRenderTarget(renderer, card);
            bo2display::draw_test_card(renderer, plan);
            SDL_SetRenderTarget(renderer, nullptr);
        }
        const float s = h / 360.0f;  // the debug font is 8 px; scale with the screen
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);  // the bars, when there are any
        SDL_RenderClear(renderer);
        if (card) {
            // Rendered landscape; SDL's output is landscape too (the activity is sensorLandscape).
            SDL_FRect dest{static_cast<float>(plan.dest.x), static_cast<float>(plan.dest.y),
                           static_cast<float>(plan.dest.w), static_cast<float>(plan.dest.h)};
            SDL_RenderTexture(renderer, card, nullptr, &dest);
        }
        SDL_SetRenderDrawColor(renderer, 240, 236, 228, 255);
        float y = h * 0.18f;
        const float x = w * 0.30f, line = 12 * s;
        text(renderer, x, y, s, "BO2-APK shell: the game core is not linked yet");
        y += line * 1.5f;
        text(renderer, x, y, s * 0.6f, gpu.name + " / " + gpu.driver);
        y += line;
        text(renderer, x, y, s * 0.6f, std::string("S26: ") + bo2device::s26_variant(gpu.vendor_id));
        y += line;
        text(renderer, x, y, s * 0.6f, "data: " + data);
        y += line;
        char status[160];
        std::snprintf(status, sizeof status, "screen %dx%d, game %dx%d (%s), drawn %dx%d", plan.panel_w, plan.panel_h,
                      plan.guest_w, plan.guest_h,
                      bo2display::settings().aspect == bo2display::Aspect::Fill ? "fill" : "16:9", plan.dest.w,
                      plan.dest.h);
        text(renderer, x, y, s * 0.6f, status);
        y += line * 1.5f;
        std::snprintf(status, sizeof status, "XInput pad 0: %s  packet %u  buttons %04x  LT %u RT %u",
                      connected ? "connected" : "absent", packet, buttons, xi[6], xi[7]);
        text(renderer, x, y, s * 0.6f, status);
        y += line;
        bar(renderer, x, y, w * 0.18f, line * 0.6f, lx / 32767.0f);
        bar(renderer, x + w * 0.20f, y, w * 0.18f, line * 0.6f, ly / 32767.0f);
        bar(renderer, x, y + line, w * 0.18f, line * 0.6f, rx / 32767.0f);
        bar(renderer, x + w * 0.20f, y + line, w * 0.18f, line * 0.6f, ry / 32767.0f);
        y += line * 2.5f;
        const bo2perf::Summary perf = bo2perf::last();
        std::snprintf(status, sizeof status, "%.1f fps (cap %d)  p50 %.1f ms  p99 %.1f ms  max %.1f ms  %.1f W  thermal %d",
                      perf.fps, bo2gfx::settings().frame_cap, perf.p50_ms, perf.p99_ms, perf.max_ms, perf.watts,
                      perf.thermal_status);
        SDL_SetRenderDrawColor(renderer, 240, 236, 228, 255);
        text(renderer, x, y, s * 0.6f, status);
        SDL_RenderPresent(renderer);
        // The frame cap: the next present waits for its slot. V-sync (the panel's rate, up to 120 Hz on an
        // S26) still bounds it from above; an uncapped setting leaves only v-sync.
        const int cap = bo2gfx::settings().frame_cap;
        if (cap != applied_cap) {
            std::fprintf(stderr, "[GFX] frame cap %d\n", cap);
            applied_cap = cap;
            next_frame_ms = 0;
        }
        if (cap > 0) {
            const double period = 1000.0 / cap, now = bo2touch::monotonic_ms();
            if (next_frame_ms == 0 || now - next_frame_ms > period) next_frame_ms = now;  // late: start again
            next_frame_ms += period;
            const double wait = next_frame_ms - bo2touch::monotonic_ms();
            if (wait > 0) SDL_DelayPrecise(static_cast<Uint64>(wait * 1e6));
        }
        bo2perf::frame_presented();
    }
    if (card) SDL_DestroyTexture(card);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Back pauses (Start) rather than leaving; Bo2Activity opens its own menu for the phone's Back.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    bo2touch::pad_init();
    int status;
    // The game when its core loads (game_linked: core_loader.cpp), the diagnostics screen otherwise.
    if (game_linked()) {
        status = run_game(data_dir(), argc, argv);
    } else
    {
        status = diagnostics();
    }
    SDL_Quit();
    return status;
}
