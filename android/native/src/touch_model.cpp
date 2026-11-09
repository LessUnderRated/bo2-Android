// touch_model.cpp — see touch_model.hpp.
#include "touch_model.hpp"

#include <algorithm>
#include <cmath>

namespace bo2touch {

namespace {

constexpr float kPi = 3.14159265358979f;

Element circle(Id id, float x, float y, float r) {
    Element e;
    e.id = id;
    e.circle = true;
    e.x = x;
    e.y = y;
    e.r = r;
    return e;
}

Element rect(Id id, float x, float y, float w, float h) {
    Element e;
    e.id = id;
    e.circle = false;
    e.x = x;
    e.y = y;
    e.w = w;
    e.h = h;
    return e;
}

}  // namespace

const char* id_name(Id id) {
    switch (id) {
        case Id::A: return "a";
        case Id::B: return "b";
        case Id::X: return "x";
        case Id::Y: return "y";
        case Id::LB: return "lb";
        case Id::RB: return "rb";
        case Id::LT: return "lt";
        case Id::RT: return "rt";
        case Id::Fire: return "fire";
        case Id::Melee: return "melee";
        case Id::Sprint: return "sprint";
        case Id::Back: return "back";
        case Id::Start: return "start";
        case Id::Dpad: return "dpad";
        case Id::Count: break;
    }
    return "?";
}

bool Element::contains(float px, float py, float slack) const {
    if (circle) return std::hypot(px - x, py - y) <= r * slack;
    const float sx = w * (slack - 1) / 2, sy = h * (slack - 1) / 2;
    return px >= x - sx && px <= x + w + sx && py >= y - sy && py <= y + h + sy;
}

Model::Model() { lay_default(1920, 1080, Insets{}); }

// Landscape. u is the window's shorter side. The top band stays clear for the HUD (BO2's compass and
// minimap are top left in multiplayer). Shoulders sit on the side edges under it, as they are held;
// the d-pad under the left ones (killstreaks, Zombies' equipment); the face buttons bottom right; the
// big FIRE button inboard of them, where the right thumb rests; Back and Start at the top centre.
void Model::lay_default(float width, float height, Insets in) {
    // A view not measured yet reports 0 x 0; a zero stick radius would make deflections NaN.
    if (!(width >= 64) || !(height >= 64)) {
        width = 1920;
        height = 1080;
        in = Insets{};
    }
    width_ = width;
    height_ = height;
    const float u = std::min(width, height);
    unit_ = u;
    const float margin = u * 0.04f;
    const float l = in.left + margin, r = width - in.right - margin, b = height - in.bottom - margin;
    const float top_band = in.top + u * 0.14f;

    const float face_r = u * 0.07f, face_gap = u * 0.12f;
    const float fx = r - face_gap - face_r, fy = b - face_gap - face_r;

    const float shoulder_w = u * 0.20f, shoulder_h = u * 0.095f, gap = u * 0.025f;
    const float t_y = top_band + gap, s_y = t_y + shoulder_h + gap;

    const float dpad_r = u * 0.10f;
    const float dpad_y = s_y + shoulder_h + gap + dpad_r;

    const float pill_w = u * 0.16f, pill_h = u * 0.065f, pill_y = in.top + margin * 0.5f;
    const float small_r = u * 0.055f;

    auto at = [&](Id id) -> Element& { return elements_[static_cast<int>(id)]; };
    at(Id::A) = circle(Id::A, fx, fy + face_gap, face_r);
    at(Id::B) = circle(Id::B, fx + face_gap, fy, face_r);
    at(Id::X) = circle(Id::X, fx - face_gap, fy, face_r);
    at(Id::Y) = circle(Id::Y, fx, fy - face_gap, face_r);
    at(Id::LT) = rect(Id::LT, l, t_y, shoulder_w, shoulder_h);
    at(Id::LB) = rect(Id::LB, l, s_y, shoulder_w, shoulder_h);
    at(Id::RT) = rect(Id::RT, r - shoulder_w, t_y, shoulder_w, shoulder_h);
    at(Id::RB) = rect(Id::RB, r - shoulder_w, s_y, shoulder_w, shoulder_h);
    at(Id::Fire) = circle(Id::Fire, fx - face_gap * 2.6f, fy - face_gap * 0.9f, u * 0.11f);
    at(Id::Melee) = circle(Id::Melee, fx + face_gap * 0.9f, fy - face_gap * 2.2f, small_r);
    at(Id::Sprint) = circle(Id::Sprint, l + dpad_r * 2.6f, dpad_y, small_r);
    at(Id::Back) = rect(Id::Back, width * 0.42f - pill_w / 2, pill_y, pill_w, pill_h);
    at(Id::Start) = rect(Id::Start, width * 0.58f - pill_w / 2, pill_y, pill_w, pill_h);
    at(Id::Dpad) = circle(Id::Dpad, l + dpad_r, dpad_y, dpad_r);

    stick_radius_ = u * 0.13f;
    // Where the left stick is drawn at rest: bottom left, clear of the d-pad above it.
    rest_x_ = l + stick_radius_ * 1.1f + u * 0.06f;
    rest_y_ = b - stick_radius_ * 1.1f;
    fingers_.clear();
}

void Model::place(Id id, float x, float y, float r, float w, float h) {
    if (id == Id::Count) return;
    Element& e = elements_[static_cast<int>(id)];
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    // Kept on the window, by the same rule as the view (TouchOverlay.clampToWindow): what is drawn
    // and what is hit must never disagree.
    x = std::clamp(x, 0.0f, width_);
    y = std::clamp(y, 0.0f, height_);
    if (e.circle) {
        if (!(r > 0)) return;
        e.r = r;
    } else {
        if (!(w > 0) || !(h > 0)) return;
        e.w = w;
        e.h = h;
    }
    e.x = x;
    e.y = y;
}

const Element* Model::hit(float px, float py) const {
    for (const Element& e : elements_)
        if (e.contains(px, py, 1.0f)) return &e;
    for (const Element& e : elements_)
        if (e.contains(px, py, 1.3f)) return &e;
    return nullptr;
}

void Model::touch(int action, uint64_t pointer, float x, float y, double time_ms) {
    if (action == 0) {
        Finger f;
        const Element* e = hit(x, y);
        if (e) {
            f.control = Control::Element;
            f.element = e->id;
            f.ox = e->cx();
            f.oy = e->cy();
        } else if (x < width_ / 2) {
            f.control = Control::LeftStick;
            f.ox = x;
            f.oy = y;
        } else {
            f.control = settings_.look == LookMode::Stick ? Control::RightStick : Control::Look;
            f.ox = x;
            f.oy = y;
        }
        f.px = f.sx = x;
        f.py = f.sy = y;
        f.last_move_ms = f.last_sample_ms = time_ms;
        fingers_[pointer] = f;
    } else if (action == 1) {
        auto it = fingers_.find(pointer);
        if (it == fingers_.end()) return;
        Finger& f = it->second;
        // Swipe look (and the fire button's swipe): the drag itself is the turn. Mouse counts = distance x
        // kLookCountsPerUnit x gain / the short side, so a drag across the short side at gain 1 moves the
        // pointer kLookCountsPerUnit counts; the game's mouse sensitivity scales it from there.
        if (f.control == Control::Look || (f.control == Control::Element && f.element == Id::Fire)) {
            const float k = kLookCountsPerUnit * std::max(settings_.look_gain, 0.05f) / std::max(unit_, 1.0f);
            look_ax_ += (x - f.px) * k;
            look_ay_ += (y - f.py) * k;
        }
        f.px = x;
        f.py = y;
        f.last_move_ms = time_ms;
        // A stick's centre follows a finger that runs past its rim, so reversing is immediate.
        if (f.control == Control::LeftStick || f.control == Control::RightStick) {
            const float dx = x - f.ox, dy = y - f.oy, len = std::hypot(dx, dy);
            if (len > stick_radius_) {
                const float excess = (len - stick_radius_) / len;
                f.ox += dx * excess;
                f.oy += dy * excess;
            }
        }
    } else {
        fingers_.erase(pointer);
    }
}

void Model::release_all() { fingers_.clear(); }

void Model::stick_deflection(const Finger& f, float* dx, float* dy) const {
    const float x = f.px - f.ox, y = f.py - f.oy, len = std::hypot(x, y);
    if (len <= 0 || len < stick_radius_ * 0.12f) {
        *dx = *dy = 0;
        return;
    }
    const float scale = std::min(len / stick_radius_, 1.0f) / len;
    *dx = x * scale;
    *dy = y * scale;
}

uint32_t Model::buttons_of(const Finger& f) const {
    if (f.control != Control::Element) return 0;
    switch (f.element) {
        case Id::A: return 1u << kSouth;
        case Id::B: return 1u << kEast;
        case Id::X: return 1u << kWest;
        case Id::Y: return 1u << kNorth;
        case Id::LB: return 1u << kLeftShoulder;
        case Id::RB: return 1u << kRightShoulder;
        case Id::Melee: return 1u << kRightStick;
        case Id::Sprint: return 1u << kLeftStick;
        case Id::Back: return 1u << kBack;
        case Id::Start: return 1u << kStart;
        case Id::Dpad: {
            const float dx = f.px - f.ox, dy = f.py - f.oy;
            if (std::hypot(dx, dy) <= stick_radius_ * 0.15f) return 0;
            // Eight directions: a diagonal presses both of its neighbours.
            int sector = static_cast<int>(std::lround(std::atan2(dy, dx) / (kPi / 4)));
            sector = ((sector % 8) + 8) % 8;
            uint32_t out = 0;
            if (sector == 7 || sector <= 1) out |= 1u << kDpadRight;
            if (sector >= 1 && sector <= 3) out |= 1u << kDpadDown;
            if (sector >= 3 && sector <= 5) out |= 1u << kDpadLeft;
            if (sector >= 5) out |= 1u << kDpadUp;
            return out;
        }
        default: return 0;  // LT, RT and FIRE are axes
    }
}

// Swipe look: the finger's speed between its last two touch events, as a deflection. Measured from
// event times, not sample times, so a game that polls faster or slower than the screen reports
// touches sees the same speed. Between events the last output is held for up to 50 ms; after that a
// finger that has stopped stops the camera.
void Model::swipe(Finger& f, double now_ms, float* rx, float* ry) {
    if (f.px != f.sx || f.py != f.sy) {
        const double dt = std::clamp(f.last_move_ms - f.last_sample_ms, 4.0, 34.0);
        const float vx = static_cast<float>((f.px - f.sx) / dt * 1000.0);  // pixels per second
        const float vy = static_cast<float>((f.py - f.sy) / dt * 1000.0);
        const float full = unit_ / std::max(settings_.look_gain, 0.05f);
        const float speed = std::hypot(vx, vy);
        const float mag = speed / full;
        if (mag < 0.004f) {
            f.held_rx = f.held_ry = 0;
        } else {
            const float floor = std::clamp(settings_.look_floor, 0.0f, 0.9f);
            const float out = floor + (1 - floor) * std::min(mag, 1.0f);
            f.held_rx = vx / speed * out;
            f.held_ry = vy / speed * out;
        }
        f.sx = f.px;
        f.sy = f.py;
        f.last_sample_ms = f.last_move_ms;
    } else if (now_ms - f.last_move_ms > 50.0) {
        f.held_rx = f.held_ry = 0;
    }
    *rx = f.held_rx;
    *ry = f.held_ry;
}

bool Model::take_look(int* dx, int* dy) {
    const int ix = static_cast<int>(look_ax_), iy = static_cast<int>(look_ay_);   // toward zero: the rest stays
    look_ax_ -= static_cast<float>(ix);
    look_ay_ -= static_cast<float>(iy);
    *dx = ix;
    *dy = iy;
    return ix != 0 || iy != 0;
}

Pad Model::sample(double now_ms) {
    Pad pad;
    float look_x = 0, look_y = 0;
    for (auto& [pointer, f] : fingers_) {
        (void)pointer;
        pad.buttons |= buttons_of(f);
        if (f.control == Control::Element) {
            if (f.element == Id::LT) pad.axes[kLeftTrigger] = 1;
            if (f.element == Id::RT) pad.axes[kRightTrigger] = 1;
            if (f.element == Id::Fire) pad.axes[kRightTrigger] = 1;   // its swipe turns as mouse motion (touch())
        } else if (f.control == Control::LeftStick) {
            float x, y;
            stick_deflection(f, &x, &y);
            pad.axes[kLeftX] = x;
            pad.axes[kLeftY] = y;
            // Auto-sprint: the stick at its rim, within 30 degrees of straight ahead, clicks L3.
            if (settings_.auto_sprint && std::hypot(x, y) > 0.95f && -y > std::cos(30 * kPi / 180) * std::hypot(x, y))
                pad.buttons |= 1u << kLeftStick;
        } else if (f.control == Control::RightStick) {
            float x, y;
            stick_deflection(f, &x, &y);
            look_x += x;
            look_y += y;
        }
        // Control::Look: swipe look is mouse motion (touch(), take_look()), not a stick.
    }
    const float len = std::hypot(look_x, look_y);
    if (len > 1) {
        look_x /= len;
        look_y /= len;
    }
    pad.axes[kRightX] = look_x;
    pad.axes[kRightY] = look_y;
    return pad;
}

uint32_t Model::buttons_for_view() const {
    uint32_t out = 0;
    for (const auto& [pointer, f] : fingers_) {
        (void)pointer;
        out |= buttons_of(f);
        if (f.control == Control::Element) {
            if (f.element == Id::LT) out |= 1u << 30;
            if (f.element == Id::RT || f.element == Id::Fire) out |= 1u << 31;
        } else if (f.control == Control::LeftStick && settings_.auto_sprint) {
            float x, y;
            stick_deflection(f, &x, &y);
            if (std::hypot(x, y) > 0.95f && -y > std::cos(30 * kPi / 180) * std::hypot(x, y)) out |= 1u << kLeftStick;
        }
    }
    return out;
}

int Model::held_sticks(HeldStick* out, int capacity) const {
    int n = 0;
    for (const auto& [pointer, f] : fingers_) {
        (void)pointer;
        if (n >= capacity) break;
        if (f.control != Control::LeftStick && f.control != Control::RightStick) continue;
        float x, y;
        stick_deflection(f, &x, &y);
        out[n++] = HeldStick{f.control == Control::LeftStick, f.ox, f.oy, f.ox + x * stick_radius_,
                             f.oy + y * stick_radius_};
    }
    return n;
}

}  // namespace bo2touch
