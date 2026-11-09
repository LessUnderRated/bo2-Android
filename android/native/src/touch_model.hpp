// touch_model.hpp — the on-screen controls for a first-person shooter, as a model with no SDL in it.
//
// Lineage: the scheme is the one LessUnderRated's Chronicle-rs Android port uses (android/src/touch.rs,
// branch ccr-3c2807db-504m84): the screen is a gamepad. The controls press SDL gamepad buttons and
// axes, a free finger on the left half is a floating left stick, and the player can move, resize and
// save every control. touch_pad.cpp then presents the result to SDL as a virtual gamepad, so the game
// (through runtime/input/pad_sdl.cpp, XInput) sees an ordinary controller. A real controller, once
// connected, takes over and the touch pad steps aside.
//
// What changes for Black Ops II, an FPS rather than a PS2 RPG:
//   - Xbox-style ABXY, laid out by position as BO2's default controller layout uses them
//     (A jump, B crouch/prone, X reload/use, Y switch weapon, LB tactical, RB lethal,
//     LT aim down sights, RT fire, L3 sprint, R3 melee, Back scoreboard, Start pause).
//   - A large FIRE button that is also a look surface: the thumb that fires can keep aiming.
//   - Looking defaults to SWIPE: finger speed becomes right-stick deflection, so turning follows the
//     finger like a touchpad. The floating right stick of the Chronicle scheme remains as STICK.
//   - Auto-sprint: pushing the left stick well past its rim, forward, holds L3.
//
// Coordinates are window pixels, y down. Times are milliseconds on one monotonic clock.
#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>

namespace bo2touch {

// SDL3's SDL_GamepadButton and SDL_GamepadAxis numbering (touch_pad.cpp static_asserts it).
enum Button : int {
    kSouth = 0, kEast = 1, kWest = 2, kNorth = 3, kBack = 4, kGuide = 5, kStart = 6,
    kLeftStick = 7, kRightStick = 8, kLeftShoulder = 9, kRightShoulder = 10,
    kDpadUp = 11, kDpadDown = 12, kDpadLeft = 13, kDpadRight = 14, kButtonCount = 15,
};
enum Axis : int { kLeftX = 0, kLeftY = 1, kRightX = 2, kRightY = 3, kLeftTrigger = 4, kRightTrigger = 5, kAxisCount = 6 };

// The controls, in the order the Java view receives them (Native.elementIds()).
enum class Id : int { A, B, X, Y, LB, RB, LT, RT, Fire, Melee, Sprint, Back, Start, Dpad, Count };
constexpr int kElementCount = static_cast<int>(Id::Count);
const char* id_name(Id id);

struct Insets {
    float left = 0, top = 0, right = 0, bottom = 0;
};

struct Element {
    Id id = Id::A;
    bool circle = true;
    float x = 0, y = 0, r = 0;  // a circle: centre and radius
    float w = 0, h = 0;         // a rect: x, y is the top-left corner
    bool contains(float px, float py, float slack) const;
    float cx() const { return circle ? x : x + w / 2; }
    float cy() const { return circle ? y : y + h / 2; }
};

enum class LookMode : int { Swipe = 0, Stick = 1 };

// Swipe look moves the game's mouse pointer: a drag across the screen's short side at gain 1.0 is this many mouse
// counts. With the game's default mouse sensitivity a count turns the view about a tenth of a degree, so this is
// roughly a quarter turn per short side.
constexpr float kLookCountsPerUnit = 800.0f;

struct Settings {
    LookMode look = LookMode::Swipe;
    // Swipe: a multiplier on kLookCountsPerUnit (the drag's turn). Stick look ignores it.
    float look_gain = 1.0f;
    // Swipe: the smallest deflection a moving finger produces, so slow aiming is not eaten by the
    // game's own stick dead zone (XInput's right-stick dead zone is 8689/32767, about 0.27).
    float look_floor = 0.28f;
    bool auto_sprint = true;
};

// What the virtual gamepad reports. Sticks -1..1 with y down (SDL's convention); triggers 0..1.
struct Pad {
    uint32_t buttons = 0;  // 1 << Button
    std::array<float, kAxisCount> axes{};
    bool pressed(int button) const { return (buttons >> button) & 1u; }
};

// A stick a finger holds, for the view to draw.
struct HeldStick {
    bool left;
    float x, y, knob_x, knob_y;
};

class Model {
public:
    Model();

    // The default arrangement for a window and its safe area (clear of the cutout and gesture edges).
    void lay_default(float width, float height, Insets insets);

    // Replaces one control's place and size (the player's saved arrangement, already scaled to this
    // window by the view). Shape is kept: a circle takes r, a rect takes w and h.
    void place(Id id, float x, float y, float r, float w, float h);

    // A touch: action 0 down, 1 move, 2 up or cancelled.
    void touch(int action, uint64_t pointer, float x, float y, double time_ms);
    void release_all();

    // The pad's state at `now_ms`. Swipe look is a rate, so this is where it is measured.
    Pad sample(double now_ms);

    // Swipe look as mouse motion: the whole mouse counts the look fingers have dragged since the last call (the
    // fraction is kept for the next one). False when there is nothing to send.
    bool take_look(int* dx, int* dy);

    // For the view: what each finger holds, without advancing the swipe measurement.
    uint32_t buttons_for_view() const;
    int held_sticks(HeldStick* out, int capacity) const;

    Settings& settings() { return settings_; }
    const std::array<Element, kElementCount>& elements() const { return elements_; }
    float width() const { return width_; }
    float height() const { return height_; }
    float stick_radius() const { return stick_radius_; }
    float rest_x() const { return rest_x_; }
    float rest_y() const { return rest_y_; }

private:
    enum class Control { Element, LeftStick, RightStick, Look };
    struct Finger {
        Control control = Control::LeftStick;
        Id element = Id::A;
        float ox = 0, oy = 0;  // a stick's centre, the d-pad's centre
        float px = 0, py = 0;  // where the finger is
        // Swipe look: where the finger was at the last sample, and when it last moved.
        float sx = 0, sy = 0;
        double last_move_ms = 0, last_sample_ms = 0;
        float held_rx = 0, held_ry = 0;  // the last swipe output, held between touch events
    };

    const Element* hit(float px, float py) const;
    void stick_deflection(const Finger& f, float* dx, float* dy) const;
    uint32_t buttons_of(const Finger& f) const;
    void swipe(Finger& f, double now_ms, float* rx, float* ry);

    Settings settings_;
    std::array<Element, kElementCount> elements_{};
    std::unordered_map<uint64_t, Finger> fingers_;
    float width_ = 0, height_ = 0, unit_ = 1, stick_radius_ = 1, rest_x_ = 0, rest_y_ = 0;
    float look_ax_ = 0, look_ay_ = 0;   // swipe look: mouse counts dragged and not yet taken
};

}  // namespace bo2touch
