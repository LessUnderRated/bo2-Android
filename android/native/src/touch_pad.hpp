// touch_pad.hpp — the touch model presented to SDL as a virtual gamepad.
//
// C++ port of Chronicle-rs's android/src/touch.rs glue (LessUnderRated): touches arrive on the
// activity's thread (jni.cpp) and only change the model under a mutex; SDL's joystick calls run on
// the thread that pumps SDL events, in the virtual gamepad's Update callback and in pump(). Neither
// lock is held while waiting for the other. While a real controller is connected the touch pad is
// detached, so the game reads exactly one pad.
#pragma once

#include <cstdint>
#include <string>

#include "touch_model.hpp"

namespace bo2touch {

// Once SDL is initialised, on the SDL thread.
void pad_init();
// Between frames, on the SDL thread: attach or detach as real controllers come and go.
void pad_pump();

// From the view (any thread).
void pad_default_layout(float w, float h, Insets insets, float* out /* 3 + 6 * kElementCount */);
void pad_set_layout(float w, float h, Insets insets, const float* placed /* same format, may be null */);
void pad_touch(int action, uint64_t pointer, float x, float y, double time_ms);
uint32_t pad_buttons_for_view();
int pad_held_sticks(HeldStick* out, int capacity);
bool pad_active();
void pad_set_look(int mode, float gain, float floor, bool auto_sprint);
double monotonic_ms();  // CLOCK_MONOTONIC, the clock MotionEvent.getEventTime() uses

}  // namespace bo2touch
