// touch_pad.cpp — see touch_pad.hpp.
#include "touch_pad.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>

extern "C" void bo2vk_mouse_rel(int dx, int dy) __attribute__((weak));   // runtime/gfx/vk, with the game core

namespace bo2touch {

// The enums differ in type, so compare their values (C++20 deprecates comparing them directly).
constexpr bool same(int a, int b) { return a == b; }

static_assert(same(kSouth, SDL_GAMEPAD_BUTTON_SOUTH) && same(kEast, SDL_GAMEPAD_BUTTON_EAST) &&
              same(kWest, SDL_GAMEPAD_BUTTON_WEST) && same(kNorth, SDL_GAMEPAD_BUTTON_NORTH) &&
              same(kBack, SDL_GAMEPAD_BUTTON_BACK) && same(kStart, SDL_GAMEPAD_BUTTON_START) &&
              same(kLeftStick, SDL_GAMEPAD_BUTTON_LEFT_STICK) && same(kRightStick, SDL_GAMEPAD_BUTTON_RIGHT_STICK) &&
              same(kLeftShoulder, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
              same(kRightShoulder, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) && same(kDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP) &&
              same(kDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN) && same(kDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT) &&
              same(kDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
              "touch_model.hpp follows SDL_GamepadButton");
static_assert(same(kLeftX, SDL_GAMEPAD_AXIS_LEFTX) && same(kLeftY, SDL_GAMEPAD_AXIS_LEFTY) &&
              same(kRightX, SDL_GAMEPAD_AXIS_RIGHTX) && same(kRightY, SDL_GAMEPAD_AXIS_RIGHTY) &&
              same(kLeftTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) && same(kRightTrigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER),
              "touch_model.hpp follows SDL_GamepadAxis");

namespace {

struct State {
    Model model;
    SDL_Joystick* joystick = nullptr;
    SDL_JoystickID instance = 0;
};

std::mutex g_mu;
State g_state;
std::mutex g_pads_mu;
std::vector<SDL_JoystickID> g_real_pads;
std::atomic<bool> g_want_attached{true};

Sint16 stick_axis(float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<Sint16>(v * SDL_JOYSTICK_AXIS_MAX);
}

// The virtual gamepad's mapping reads triggers as full-range axes, so at rest they sit at AXIS_MIN,
// not 0. Chronicle measured the alternative on a device: 0 normalises to a half-pressed trigger,
// which a game takes as held (Chronicle-rs touch.rs, Outputs::unpressed).
Sint16 trigger_axis(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return static_cast<Sint16>(SDL_JOYSTICK_AXIS_MIN + v * (SDL_JOYSTICK_AXIS_MAX - SDL_JOYSTICK_AXIS_MIN));
}

// SDL calls this from SDL_UpdateJoysticks, on the SDL thread, before it reads the pad's state.
// Only what changed is pushed: each setter takes SDL's joystick lock, and the pad is usually still.
void SDLCALL update(void*) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_state.joystick) return;
    static SDL_Joystick* last_joy = nullptr;
    static uint32_t last_buttons = 0;
    static Sint16 last_axes[SDL_GAMEPAD_AXIS_COUNT];
    const bool fresh = last_joy != g_state.joystick;   // a new virtual pad: push everything once
    last_joy = g_state.joystick;
    const Pad pad = g_state.model.sample(monotonic_ms());
    const uint32_t changed = fresh ? ~0u : (pad.buttons ^ last_buttons);
    if (changed)
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if ((changed >> b) & 1u) SDL_SetJoystickVirtualButton(g_state.joystick, b, (pad.buttons >> b) & 1u);
    last_buttons = pad.buttons;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) {
        const bool trigger = a == kLeftTrigger || a == kRightTrigger;
        const Sint16 v = trigger ? trigger_axis(pad.axes[a]) : stick_axis(pad.axes[a]);
        if (fresh || v != last_axes[a]) SDL_SetJoystickVirtualAxis(g_state.joystick, a, v);
        last_axes[a] = v;
    }
}

// Attaching and detaching push SDL events, which run the event watch; neither is called with g_mu
// held, or a finger arriving at that moment would wait on SDL while SDL waits on it.
bool attach(SDL_JoystickID* instance, SDL_Joystick** joystick) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Touch screen";
    desc.Update = update;
    *instance = SDL_AttachVirtualJoystick(&desc);
    if (!*instance) {
        std::fprintf(stderr, "[TOUCH] cannot attach the touch gamepad: %s\n", SDL_GetError());
        return false;
    }
    *joystick = SDL_OpenJoystick(*instance);
    std::fprintf(stderr, "[TOUCH] touch gamepad attached (instance %u)\n", static_cast<unsigned>(*instance));
    return true;
}

void detach(SDL_JoystickID instance, SDL_Joystick* joystick) {
    if (joystick) SDL_CloseJoystick(joystick);
    SDL_DetachVirtualJoystick(instance);
    std::fprintf(stderr, "[TOUCH] touch gamepad detached: a controller is connected\n");
}

bool SDLCALL watch(void*, SDL_Event* event) {
    switch (event->type) {
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED: {
            const SDL_JoystickID which = event->gdevice.which;
            std::lock_guard<std::mutex> lock(g_pads_mu);
            if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
                if (!SDL_IsJoystickVirtual(which) &&
                    std::find(g_real_pads.begin(), g_real_pads.end(), which) == g_real_pads.end())
                    g_real_pads.push_back(which);
            } else {
                g_real_pads.erase(std::remove(g_real_pads.begin(), g_real_pads.end(), which), g_real_pads.end());
            }
            g_want_attached.store(g_real_pads.empty());
            break;
        }
        default:
            break;
    }
    return true;
}

}  // namespace

double monotonic_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void pad_init() {
    // A finger on the controls must not also move the menus' mouse pointer.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) std::fprintf(stderr, "[TOUCH] SDL_InitSubSystem(GAMEPAD): %s\n", SDL_GetError());
    SDL_AddEventWatch(watch, nullptr);
    // Controllers connected before the watch existed: SDL_Init announced them already (its
    // GAMEPAD_ADDED events ran before this watch was added), so count them here.
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        std::lock_guard<std::mutex> lock(g_pads_mu);
        for (int i = 0; i < count; i++)
            if (!SDL_IsJoystickVirtual(ids[i]) &&
                std::find(g_real_pads.begin(), g_real_pads.end(), ids[i]) == g_real_pads.end())
                g_real_pads.push_back(ids[i]);
        g_want_attached.store(g_real_pads.empty());
        SDL_free(ids);
    }
    pad_pump();
}

void pad_pump() {
    const bool want = g_want_attached.load();
    SDL_JoystickID instance;
    SDL_Joystick* joystick;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        instance = g_state.instance;
        joystick = g_state.joystick;
    }
    if (want && !instance) {
        if (attach(&instance, &joystick)) {
            std::lock_guard<std::mutex> lock(g_mu);
            g_state.instance = instance;
            g_state.joystick = joystick;
        }
    } else if (!want && instance) {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_state.instance = 0;
            g_state.joystick = nullptr;
            g_state.model.release_all();
        }
        detach(instance, joystick);
    }
}

static void write_layout(const Model& m, float* out) {
    out[0] = m.stick_radius();
    out[1] = m.rest_x();
    out[2] = m.rest_y();
    for (int i = 0; i < kElementCount; i++) {
        const Element& e = m.elements()[i];
        float* o = out + 3 + 6 * i;
        o[0] = e.circle ? 1.0f : 0.0f;
        o[1] = e.x;
        o[2] = e.y;
        o[3] = e.r;
        o[4] = e.w;
        o[5] = e.h;
    }
}

void pad_default_layout(float w, float h, Insets insets, float* out) {
    Model fresh;
    fresh.lay_default(w, h, insets);
    write_layout(fresh, out);
}

void pad_set_layout(float w, float h, Insets insets, const float* placed) {
    std::lock_guard<std::mutex> lock(g_mu);
    Settings keep = g_state.model.settings();
    g_state.model.lay_default(w, h, insets);
    g_state.model.settings() = keep;
    if (!placed) return;
    for (int i = 0; i < kElementCount; i++) {
        const float* p = placed + 3 + 6 * i;
        g_state.model.place(static_cast<Id>(i), p[1], p[2], p[3], p[4], p[5]);
    }
}

void pad_touch(int action, uint64_t pointer, float x, float y, double time_ms) {
    int dx = 0, dy = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_state.instance) return;
        g_state.model.touch(action, pointer, x, y, time_ms);
        if (!g_state.model.take_look(&dx, &dy)) return;
    }
    // Swipe look: the drag moves the game's pointer at once, as a mouse would (no sampling, no stick curve).
    if (bo2vk_mouse_rel) bo2vk_mouse_rel(dx, dy);
}

uint32_t pad_buttons_for_view() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_state.model.buttons_for_view();
}

int pad_held_sticks(HeldStick* out, int capacity) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_state.model.held_sticks(out, capacity);
}

bool pad_active() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_state.instance != 0;
}

void pad_set_look(int mode, float gain, float floor, bool auto_sprint) {
    std::lock_guard<std::mutex> lock(g_mu);
    Settings& s = g_state.model.settings();
    s.look = mode == 1 ? LookMode::Stick : LookMode::Swipe;
    s.look_gain = gain;
    s.look_floor = floor;
    s.auto_sprint = auto_sprint;
}

}  // namespace bo2touch
