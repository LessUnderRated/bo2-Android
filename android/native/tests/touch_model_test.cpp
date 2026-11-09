// Host tests for the touch model: c++ -std=c++20 -I../src touch_model_test.cpp ../src/touch_model.cpp
// (scripts/test-host.sh runs them). Several follow Chronicle-rs's android/src/touch.rs tests.
#include "touch_model.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace bo2touch;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

// A Galaxy S-class phone in landscape: 2340 x 1080 with a cutout on the left.
static Model phone() {
    Model m;
    m.lay_default(2340, 1080, Insets{110, 0, 0, 0});
    return m;
}

static const Element& el(const Model& m, Id id) { return m.elements()[static_cast<int>(id)]; }

static void face_buttons_press_their_gamepad_buttons() {
    Model m = phone();
    struct { Id id; int button; } cases[] = {
        {Id::A, kSouth}, {Id::B, kEast}, {Id::X, kWest}, {Id::Y, kNorth}, {Id::LB, kLeftShoulder},
        {Id::RB, kRightShoulder}, {Id::Melee, kRightStick}, {Id::Sprint, kLeftStick}, {Id::Back, kBack},
        {Id::Start, kStart},
    };
    for (auto c : cases) {
        const Element& e = el(m, c.id);
        m.touch(0, 1, e.cx(), e.cy(), 0);
        Pad p = m.sample(1);
        CHECK(p.buttons == (1u << c.button));
        m.touch(2, 1, e.cx(), e.cy(), 2);
        CHECK(m.sample(3).buttons == 0);
    }
}

static void triggers_are_full_axes_and_fire_is_rt() {
    Model m = phone();
    m.touch(0, 1, el(m, Id::LT).cx(), el(m, Id::LT).cy(), 0);
    m.touch(0, 2, el(m, Id::Fire).cx(), el(m, Id::Fire).cy(), 0);
    Pad p = m.sample(1);
    CHECK(p.axes[kLeftTrigger] == 1.0f);
    CHECK(p.axes[kRightTrigger] == 1.0f);
    CHECK(p.buttons == 0);
    CHECK((m.buttons_for_view() & (3u << 30)) == (3u << 30));
}

static void empty_left_half_is_a_floating_stick() {
    Model m = phone();
    const float x = 700, y = 800;  // nothing there on the left half
    m.touch(0, 1, x, y, 0);
    m.touch(1, 1, x + m.stick_radius(), y, 10);
    Pad p = m.sample(11);
    CHECK(std::fabs(p.axes[kLeftX] - 1.0f) < 1e-4f);
    CHECK(std::fabs(p.axes[kLeftY]) < 1e-4f);
    // Past the rim the centre follows, so reversing is immediate.
    m.touch(1, 1, x + 3 * m.stick_radius(), y, 20);
    m.touch(1, 1, x + 2 * m.stick_radius(), y, 30);
    p = m.sample(31);
    CHECK(p.axes[kLeftX] < 0.0f || std::fabs(p.axes[kLeftX]) < 1e-4f);
}

static void small_movements_are_dead_zone() {
    Model m = phone();
    m.touch(0, 1, 700, 800, 0);
    m.touch(1, 1, 700 + m.stick_radius() * 0.1f, 800, 10);
    Pad p = m.sample(11);
    CHECK(p.axes[kLeftX] == 0 && p.axes[kLeftY] == 0);
}

static void auto_sprint_clicks_l3_at_the_rim_forward_only() {
    Model m = phone();
    m.touch(0, 1, 700, 800, 0);
    m.touch(1, 1, 700, 800 - m.stick_radius(), 10);  // straight ahead (y down, so up is negative)
    CHECK(m.sample(11).pressed(kLeftStick));
    m.touch(1, 1, 700 + m.stick_radius(), 800, 20);  // sideways at the rim
    CHECK(!m.sample(21).pressed(kLeftStick));
    m.settings().auto_sprint = false;
    m.touch(1, 1, 700, 800 - 2 * m.stick_radius(), 30);
    CHECK(!m.sample(31).pressed(kLeftStick));
}

static void dpad_diagonals_press_both_directions() {
    Model m = phone();
    const Element& d = el(m, Id::Dpad);
    m.touch(0, 1, d.x, d.y, 0);
    m.touch(1, 1, d.x + d.r * 0.6f, d.y - d.r * 0.6f, 10);  // up-right
    Pad p = m.sample(11);
    CHECK(p.pressed(kDpadUp) && p.pressed(kDpadRight) && !p.pressed(kDpadDown) && !p.pressed(kDpadLeft));
}

static void swipe_look_is_mouse_motion() {
    Model m = phone();
    const float x = 1500, y = 300;  // the right half, clear of every control
    int dx = 0, dy = 0;
    m.touch(0, 1, x, y, 0);
    CHECK(!m.take_look(&dx, &dy));
    // A tenth of the short side to the right, at gain 1: a tenth of kLookCountsPerUnit.
    m.touch(1, 1, x + 108.0f, y, 10);
    CHECK(m.take_look(&dx, &dy));
    CHECK(std::abs(dx - static_cast<int>(kLookCountsPerUnit / 10)) <= 1 && dy == 0);
    // Taken once; the stick stays at rest.
    CHECK(!m.take_look(&dx, &dy));
    CHECK(m.sample(12).axes[kRightX] == 0);
}

static void swipe_keeps_the_fraction_of_slow_drags() {
    // Many tiny moves, each under one count, still add up to the whole drag.
    Model m = phone();
    m.touch(0, 1, 1500, 300, 0);
    int total = 0, dx = 0, dy = 0;
    for (int i = 1; i <= 100; i++) {
        m.touch(1, 1, 1500 + i * 0.5f, 300, i * 8.0);
        if (m.take_look(&dx, &dy)) total += dx;
    }
    const int expected = static_cast<int>(50.0f / 1080.0f * kLookCountsPerUnit);
    CHECK(std::abs(total - expected) <= 1);
}

static void fire_also_aims() {
    Model m = phone();
    const Element& f = el(m, Id::Fire);
    int dx = 0, dy = 0;
    m.touch(0, 1, f.x, f.y, 0);
    m.touch(1, 1, f.x - 108.0f, f.y, 10);
    Pad p = m.sample(11);
    CHECK(p.axes[kRightTrigger] == 1.0f);
    CHECK(m.take_look(&dx, &dy) && dx < 0);
}

static void stick_look_mode_is_a_floating_right_stick() {
    Model m = phone();
    m.settings().look = LookMode::Stick;
    m.touch(0, 1, 1500, 300, 0);
    m.touch(1, 1, 1500, 300 + m.stick_radius(), 10);
    Pad p = m.sample(500);  // a held stick does not decay
    CHECK(std::fabs(p.axes[kRightY] - 1.0f) < 1e-4f);
    HeldStick s[4];
    CHECK(m.held_sticks(s, 4) == 1 && !s[0].left);
}

static void controls_do_not_overlap_and_stay_on_screen() {
    for (auto size : {std::pair{2340.f, 1080.f}, {1920.f, 1080.f}, {2800.f, 1260.f}, {2400.f, 1080.f}}) {
        Model m;
        m.lay_default(size.first, size.second, Insets{110, 0, 0, 40});
        auto box = [](const Element& e, float* l, float* t, float* r, float* b) {
            if (e.circle) { *l = e.x - e.r; *t = e.y - e.r; *r = e.x + e.r; *b = e.y + e.r; }
            else { *l = e.x; *t = e.y; *r = e.x + e.w; *b = e.y + e.h; }
        };
        for (int i = 0; i < kElementCount; i++) {
            float l, t, r, b;
            box(m.elements()[i], &l, &t, &r, &b);
            CHECK(l >= 0 && t >= 0 && r <= size.first && b <= size.second);
            for (int j = i + 1; j < kElementCount; j++) {
                float l2, t2, r2, b2;
                box(m.elements()[j], &l2, &t2, &r2, &b2);
                const Element& ei = m.elements()[i];
                const Element& ej = m.elements()[j];
                const bool overlap = ei.circle && ej.circle
                                         ? std::hypot(ei.x - ej.x, ei.y - ej.y) < ei.r + ej.r
                                         : l < r2 && l2 < r && t < b2 && t2 < b;
                if (overlap) std::fprintf(stderr, "  %s overlaps %s\n", id_name(m.elements()[i].id), id_name(m.elements()[j].id));
                CHECK(!overlap);
            }
        }
    }
}

static void a_saved_place_replaces_the_default() {
    Model m = phone();
    m.place(Id::A, 500, 500, 90, 0, 0);
    CHECK(el(m, Id::A).x == 500 && el(m, Id::A).r == 90);
    m.place(Id::LB, 10, 10, 0, 300, 100);
    CHECK(el(m, Id::LB).w == 300);
    m.place(Id::A, -5, 10, 90, 0, 0);  // dragged past the edge: kept on the window, as the view does
    CHECK(el(m, Id::A).x == 0 && el(m, Id::A).y == 10);
}

static void an_unmeasured_window_gives_no_nan() {
    Model m;
    m.lay_default(0, 0, Insets{});
    m.touch(0, 1, 100, 100, 0);
    m.touch(1, 1, 100, 100, 5);
    Pad p = m.sample(6);
    for (float a : p.axes) CHECK(std::isfinite(a));
}

int main() {
    face_buttons_press_their_gamepad_buttons();
    triggers_are_full_axes_and_fire_is_rt();
    empty_left_half_is_a_floating_stick();
    small_movements_are_dead_zone();
    auto_sprint_clicks_l3_at_the_rim_forward_only();
    dpad_diagonals_press_both_directions();
    swipe_look_is_mouse_motion();
    swipe_keeps_the_fraction_of_slow_drags();
    fire_also_aims();
    stick_look_mode_is_a_floating_right_stick();
    controls_do_not_overlap_and_stay_on_screen();
    a_saved_place_replaces_the_default();
    an_unmeasured_window_gives_no_nan();
    if (failures) {
        std::fprintf(stderr, "touch_model_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("touch_model_test: all passed");
    return 0;
}
