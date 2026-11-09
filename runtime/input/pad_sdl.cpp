// pad_sdl.cpp — the controller as XInput sees it, from SDL3 gamepads. The Android (and Linux) build's
// replacement for bo2-mac's runtime/input/pad_gc.mm (GameController.framework), with the same four
// entry points, so the runtime's XInput shim (remaining_dlls_impl.cpp, xinput1_3 ordinals 2/3/4) is
// unchanged. Not a drop-in on its own: the platform loop must call bo2_pad_pump() and
// bo2_pad_rumble_pump() once per frame on the SDL thread, which bo2-mac's runtime does not do.
// Adapted from pad_gc.mm (adrbn/bo2-mac, GPL-3.0) for Bo2-APK, modified 2026-11-07;
// see THIRD_PARTY_NOTICES.md.
//
// The pad is whichever SDL gamepad is open: a real controller (Bluetooth, USB, a phone grip), or,
// with none connected, the touch screen, which android/native/src/touch_pad.cpp attaches as an SDL
// virtual gamepad. The touch pad steps aside when a real one connects, so index 0 is always the one
// the player is holding.
//
//   XINPUT_STATE   = { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; }            16 bytes
//   XINPUT_GAMEPAD = { WORD wButtons; BYTE bLeftTrigger, bRightTrigger;
//                      SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; }            12 bytes
//   dwPacketNumber changes only when the state does (the game skips a read when it has not).
//
// Mapping by position, as Windows' Xbox driver and Steam Input do: south = A, east = B, west = X,
// north = Y. Sticks -32768..32767 with Y up positive (SDL's Y is down: negated here). Dead zones are
// the game's own, applied by it: the raw value is passed through.
//
// Threads: SDL's gamepad state may only be read on the thread that pumps SDL events. That thread
// calls bo2_pad_pump() once per frame, which takes a snapshot under a mutex; the guest's input
// thread reads the snapshot in XInputGetState and never calls into SDL.
#include <SDL3/SDL.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include "core_loader.hpp"   // the game core's console command queue (null until the core loads)

namespace {

enum : uint16_t {
    XI_UP = 0x0001, XI_DOWN = 0x0002, XI_LEFT = 0x0004, XI_RIGHT = 0x0008,
    XI_START = 0x0010, XI_BACK = 0x0020, XI_LTHUMB = 0x0040, XI_RTHUMB = 0x0080,
    XI_LSHOULDER = 0x0100, XI_RSHOULDER = 0x0200,
    XI_A = 0x1000, XI_B = 0x2000, XI_X = 0x4000, XI_Y = 0x8000,
};
constexpr uint32_t kSuccess = 0, kNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED

struct Gamepad {  // XINPUT_GAMEPAD exactly (12 bytes, no padding)
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
static_assert(sizeof(Gamepad) == 12, "XINPUT_GAMEPAD is 12 bytes");

std::mutex g_mu;
Gamepad g_state;
bool g_connected = false;
uint32_t g_packet = 0;
SDL_Gamepad* g_pad = nullptr;  // SDL thread only
// XInputSetState's rumble: recorded under g_mu, played by bo2_pad_rumble_pump on the SDL thread.
uint16_t g_rumble_low = 0, g_rumble_high = 0;
bool g_rumble_changed = false;
std::atomic<bool> g_disabled{false};
// A physical pad's state straight from Android's input events (bo2_pad_direct, PadDirect.java, on the UI thread as
// each event arrives), instead of the SDL thread's 120 Hz copy: the game reads it as soon as it changes, whatever the
// game's own threads are doing. The SDL thread still opens and closes the pad, and plays rumble.
std::atomic<bool> g_direct{false};
int64_t g_direct_ns = 0;   // when the direct state last changed (under g_mu), for the comparison with SDL's

int64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// [PAD-LAT] (every 10 s, from the game's reads): how old a new pad state is when the game first reads it, how often the
// game reads, and from how many threads. Under g_mu.
int64_t g_unread_ns = 0;   // when the oldest state change the game has not read yet arrived; 0: none
uint64_t g_reads = 0, g_fresh = 0, g_fresh_sum_us = 0, g_fresh_max_us = 0, g_gap_max_us = 0;
int64_t g_last_read_ns = 0, g_window_ns = 0;
int g_readers[4] = {0, 0, 0, 0};
// [PAD-CHECK]: the direct state against SDL's own reading of the same pad (bo2_pad_pump), when both have settled.
uint64_t g_checks = 0, g_mismatches = 0;

// `gpad_enabled` is archived and 0 by default: the game ignores the pad until the player turns it on
// in Options (bo2-mac, pad_gc.mm). A pad connecting turns it on, as there.
void enable_gpad() {
    if (const auto q = bo2core::api().cmd_queue) q("set gpad_enabled 1");
}

int16_t thumb_y(Sint16 sdl_y) {  // SDL down-positive to XInput up-positive, without overflow
    return sdl_y == -32768 ? 32767 : static_cast<int16_t>(-sdl_y);
}

uint8_t trigger(Sint16 v) {  // SDL gives 0..32767 for a trigger
    return v <= 0 ? 0 : static_cast<uint8_t>((v * 255 + 16383) / 32767);
}

void publish(const Gamepad& e) {  // under g_mu
    if (std::memcmp(&e, &g_state, sizeof e) != 0) {
        g_state = e;
        ++g_packet;
        if (!g_unread_ns) g_unread_ns = mono_ns();
    }
}

// Direct and SDL readings of one pad agree when the buttons are the same and every axis is close (the two read the
// same events, but not at the same instant).
bool agree(const Gamepad& a, const Gamepad& b) {
    auto close = [](int x, int y, int tol) { return (x > y ? x - y : y - x) <= tol; };
    return a.buttons == b.buttons && close(a.lt, b.lt, 24) && close(a.rt, b.rt, 24) && close(a.lx, b.lx, 3000) &&
           close(a.ly, b.ly, 3000) && close(a.rx, b.rx, 3000) && close(a.ry, b.ry, 3000);
}

}  // namespace

// Once per frame on the SDL thread, after the events are pumped.
extern "C" void bo2_pad_pump() {
    if (g_disabled) return;
    if (g_pad && !SDL_GamepadConnected(g_pad)) {
        SDL_CloseGamepad(g_pad);
        g_pad = nullptr;
        std::lock_guard<std::mutex> lock(g_mu);
        g_connected = false;
        g_state = Gamepad{};
        ++g_packet;
        g_direct = false;
        std::fprintf(stderr, "[PAD] pad disconnected\n");
    }
    if (!g_pad) {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        // A real pad first: the touch pad is detached while one is connected, but take no chances.
        for (int pass = 0; pass < 2 && !g_pad && ids; pass++)
            for (int i = 0; i < count && !g_pad; i++)
                if ((pass == 0) != SDL_IsJoystickVirtual(ids[i])) g_pad = SDL_OpenGamepad(ids[i]);
        SDL_free(ids);
        if (!g_pad) return;
        std::fprintf(stderr, "[PAD] pad on index 0: %s\n", SDL_GetGamepadName(g_pad));
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_connected = true;
            ++g_packet;
            g_rumble_changed = true;  // a new pad gets the rumble the game asked for already
        }
        enable_gpad();
    }
    Gamepad e;
    auto b = [&](SDL_GamepadButton button, uint16_t bit) {
        if (SDL_GetGamepadButton(g_pad, button)) e.buttons |= bit;
    };
    b(SDL_GAMEPAD_BUTTON_DPAD_UP, XI_UP);
    b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, XI_DOWN);
    b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, XI_LEFT);
    b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XI_RIGHT);
    b(SDL_GAMEPAD_BUTTON_START, XI_START);
    b(SDL_GAMEPAD_BUTTON_BACK, XI_BACK);
    b(SDL_GAMEPAD_BUTTON_LEFT_STICK, XI_LTHUMB);
    b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, XI_RTHUMB);
    b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XI_LSHOULDER);
    b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XI_RSHOULDER);
    b(SDL_GAMEPAD_BUTTON_SOUTH, XI_A);
    b(SDL_GAMEPAD_BUTTON_EAST, XI_B);
    b(SDL_GAMEPAD_BUTTON_WEST, XI_X);
    b(SDL_GAMEPAD_BUTTON_NORTH, XI_Y);
    e.lt = trigger(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    e.rt = trigger(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    e.lx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX);
    e.ly = thumb_y(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY));
    e.rx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX);
    e.ry = thumb_y(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY));
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_direct.load(std::memory_order_relaxed) && !SDL_IsJoystickVirtual(SDL_GetGamepadID(g_pad))) {
        // A physical pad's state comes from Android's events directly (the copy would be older): only check that SDL,
        // reading the same pad its own way, agrees, once the direct state has been still for 150 ms.
        if (mono_ns() - g_direct_ns > 150000000LL) {
            ++g_checks;
            if (!agree(e, g_state) && (++g_mismatches & (g_mismatches - 1)) == 0)
                std::fprintf(stderr, "[PAD-CHECK] direct and SDL differ (%llu of %llu checks): buttons %04x / %04x, "
                                     "triggers %u,%u / %u,%u, left %d,%d / %d,%d, right %d,%d / %d,%d\n",
                             (unsigned long long)g_mismatches, (unsigned long long)g_checks, g_state.buttons, e.buttons,
                             g_state.lt, g_state.rt, e.lt, e.rt, g_state.lx, g_state.ly, e.lx, e.ly, g_state.rx,
                             g_state.ry, e.rx, e.ry);
        }
        return;
    }
    publish(e);
}

extern "C" void bo2_pad_disable() { g_disabled = true; }

// The physical pad's whole state, from Android's input events as they arrive (PadDirect.java, the UI thread): XInput's
// layout and ranges already (sticks up-positive, triggers 0..255). From the first call on, the SDL thread's copy steps
// aside for physical pads (bo2_pad_pump); the touch pad stays on it.
extern "C" void bo2_pad_direct(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    Gamepad e;
    e.buttons = buttons;
    e.lt = lt;
    e.rt = rt;
    e.lx = lx;
    e.ly = ly;
    e.rx = rx;
    e.ry = ry;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_direct.exchange(true))
        std::fprintf(stderr, "[PAD] controller state straight from Android's input events (not the 120 Hz copy)\n");
    g_direct_ns = mono_ns();
    publish(e);
}

// XInputGetState: the 16 bytes of XINPUT_STATE in `out`. ERROR_SUCCESS or 0x48F.
extern "C" uint32_t bo2_pad_etat(uint32_t idx, uint8_t out[16]) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (idx != 0 || !g_connected) return kNotConnected;
    // [PAD-LAT]: the game's reads of pad 0.
    const int64_t now = mono_ns();
    if (!g_window_ns) g_window_ns = now;
    ++g_reads;
    if (g_last_read_ns) g_gap_max_us = std::max<uint64_t>(g_gap_max_us, (uint64_t)(now - g_last_read_ns) / 1000);
    g_last_read_ns = now;
    if (g_unread_ns) {
        const uint64_t us = (uint64_t)(now - g_unread_ns) / 1000;
        ++g_fresh;
        g_fresh_sum_us += us;
        g_fresh_max_us = std::max(g_fresh_max_us, us);
        g_unread_ns = 0;
    }
    const int tid = gettid();
    for (int& r : g_readers) {
        if (r == tid) break;
        if (!r) { r = tid; break; }
    }
    if (now - g_window_ns >= 10000000000LL) {
        const double secs = (now - g_window_ns) / 1e9;
        int nthreads = 0;
        for (int r : g_readers) nthreads += r != 0;
        std::fprintf(stderr, "[PAD-LAT] the game reads the pad %.0f times a second (longest gap %.1f ms, %d thread%s); "
                             "a new state waited %.1f ms on average, %.1f at most, before its first read (%llu states)"
                             "%s; direct and SDL checks %llu, mismatches %llu\n",
                     g_reads / secs, g_gap_max_us / 1000.0, nthreads, nthreads == 1 ? "" : "s",
                     g_fresh ? g_fresh_sum_us / 1000.0 / g_fresh : 0.0, g_fresh_max_us / 1000.0,
                     (unsigned long long)g_fresh, g_direct.load() ? ", from Android's events" : ", from SDL's copy",
                     (unsigned long long)g_checks, (unsigned long long)g_mismatches);
        g_reads = g_fresh = g_fresh_sum_us = g_fresh_max_us = g_gap_max_us = 0;
        g_window_ns = now;
        for (int& r : g_readers) r = 0;
    }
    std::memcpy(out, &g_packet, 4);
    std::memcpy(out + 4, &g_state, 12);
    return kSuccess;
}

// XInputGetCapabilities: the 20 bytes of XINPUT_CAPABILITIES (Type=GAMEPAD, SubType=GAMEPAD).
extern "C" uint32_t bo2_pad_capacites(uint32_t idx, uint8_t out[20]) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (idx != 0 || !g_connected) return kNotConnected;
    std::memset(out, 0, 20);
    out[0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
    out[1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
    const uint16_t all = 0xF3FF;
    std::memcpy(out + 4, &all, 2);
    out[6] = out[7] = 0xFF;
    const int16_t full = 0x7FC0;
    for (int k = 0; k < 4; k++) std::memcpy(out + 8 + 2 * k, &full, 2);
    std::memset(out + 16, 0xFF, 4);
    return kSuccess;
}


extern "C" uint32_t bo2_pad_vibrer(uint32_t idx, uint16_t low, uint16_t high) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (idx != 0 || !g_connected) return kNotConnected;
    if (low != g_rumble_low || high != g_rumble_high) {
        g_rumble_low = low;
        g_rumble_high = high;
        g_rumble_changed = true;
    }
    return kSuccess;
}

// On the SDL thread, after bo2_pad_pump: hands the game's rumble to the pad. XInputSetState holds
// until the game changes it, but an SDL rumble ends after its duration, so a held level is sent again
// every half second with a one-second duration.
extern "C" void bo2_pad_rumble_pump() {
    static uint64_t sent_at = 0;
    uint16_t low, high;
    bool changed;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        changed = g_rumble_changed;
        g_rumble_changed = false;
        low = g_rumble_low;
        high = g_rumble_high;
    }
    const uint64_t now = SDL_GetTicks();
    const bool held = (low || high) && now - sent_at >= 500;
    if (!g_pad || (!changed && !held)) return;
    SDL_RumbleGamepad(g_pad, low, high, (low || high) ? 1000 : 0);
    sent_at = now;
}

// bo2-mac's runtime calls this when the measured level's fast file loads (BO2_PAD_SCRIPT's start).
// Scripted pads are not carried over yet; it only re-enables the pad, as pad_gc.mm does.
extern "C" void bo2_pad_top() {
    bool connected;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        connected = g_connected;
    }
    if (connected) enable_gpad();
}
