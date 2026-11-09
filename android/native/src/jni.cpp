// jni.cpp — what io.github.lessunderrated.bo2.Native (the touch view, the menu, the setup screen)
// calls in libmain.so. Layout as flat float arrays: [stick_radius, rest_x, rest_y] then, per control
// in Native.elementIds() order, [circle (1/0), x, y, r, w, h].
#include <jni.h>

#include <atomic>
#include <string>

#include "device_report.hpp"
#include "display_plan.hpp"
#include "graphics.hpp"
#include "log.hpp"
#include "perf.hpp"
#include "touch_pad.hpp"

extern "C" void bo2_graphics_changed() __attribute__((weak));   // game_host.cpp, only with the game core

using namespace bo2touch;

namespace {

constexpr int kLayoutFloats = 3 + 6 * kElementCount;

std::string str(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(s, chars);
    return out;
}

}  // namespace

#define NATIVE(ret, name) extern "C" JNIEXPORT ret JNICALL Java_io_github_lessunderrated_bo2_Native_##name

NATIVE(void, openLog)(JNIEnv* env, jclass, jstring dir) { bo2log::open(str(env, dir).c_str()); }
NATIVE(void, setDiagnostics)(JNIEnv*, jclass, jint level) { bo2log::set_diagnostics(level); }   // launcher: Logging
extern "C" void bo2_pad_direct(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry);
NATIVE(void, padDirect)(JNIEnv*, jclass, jint buttons, jint lt, jint rt, jint lx, jint ly, jint rx, jint ry) {   // PadDirect.java
    bo2_pad_direct((uint16_t)buttons, (uint8_t)lt, (uint8_t)rt, (int16_t)lx, (int16_t)ly, (int16_t)rx, (int16_t)ry);
}

NATIVE(void, log)(JNIEnv* env, jclass, jstring text) { bo2log::write(str(env, text).c_str()); }

NATIVE(jobjectArray, elementIds)(JNIEnv* env, jclass) {
    jobjectArray out = env->NewObjectArray(kElementCount, env->FindClass("java/lang/String"), nullptr);
    for (int i = 0; i < kElementCount; i++)
        env->SetObjectArrayElement(out, i, env->NewStringUTF(id_name(static_cast<Id>(i))));
    return out;
}

NATIVE(jfloatArray, defaultLayout)(JNIEnv* env, jclass, jint w, jint h, jfloat l, jfloat t, jfloat r, jfloat b) {
    float layout[kLayoutFloats];
    pad_default_layout(static_cast<float>(w), static_cast<float>(h), Insets{l, t, r, b}, layout);
    jfloatArray out = env->NewFloatArray(kLayoutFloats);
    env->SetFloatArrayRegion(out, 0, kLayoutFloats, layout);
    return out;
}

NATIVE(void, setLayout)(JNIEnv* env, jclass, jint w, jint h, jfloat l, jfloat t, jfloat r, jfloat b, jfloatArray placed) {
    float layout[kLayoutFloats];
    const bool have = placed && env->GetArrayLength(placed) == kLayoutFloats;
    if (have) env->GetFloatArrayRegion(placed, 0, kLayoutFloats, layout);
    pad_set_layout(static_cast<float>(w), static_cast<float>(h), Insets{l, t, r, b}, have ? layout : nullptr);
}

NATIVE(void, touch)(JNIEnv*, jclass, jint action, jlong pointer, jfloat x, jfloat y, jlong time_ms) {
    pad_touch(action, static_cast<uint64_t>(pointer), x, y, static_cast<double>(time_ms));
}

NATIVE(jint, padButtons)(JNIEnv*, jclass) { return static_cast<jint>(pad_buttons_for_view()); }

NATIVE(jfloatArray, padSticks)(JNIEnv* env, jclass) {
    HeldStick sticks[4];
    const int n = pad_held_sticks(sticks, 4);
    float flat[4 * 5];
    for (int i = 0; i < n; i++) {
        flat[5 * i] = sticks[i].left ? 1.0f : 0.0f;
        flat[5 * i + 1] = sticks[i].x;
        flat[5 * i + 2] = sticks[i].y;
        flat[5 * i + 3] = sticks[i].knob_x;
        flat[5 * i + 4] = sticks[i].knob_y;
    }
    jfloatArray out = env->NewFloatArray(5 * n);
    env->SetFloatArrayRegion(out, 0, 5 * n, flat);
    return out;
}

NATIVE(jboolean, touchActive)(JNIEnv*, jclass) { return pad_active() ? JNI_TRUE : JNI_FALSE; }

NATIVE(void, setLook)(JNIEnv*, jclass, jint mode, jfloat gain, jfloat floor, jboolean auto_sprint) {
    pad_set_look(mode, gain, floor, auto_sprint == JNI_TRUE);
}

NATIVE(void, setWatts)(JNIEnv*, jclass, jfloat watts) { bo2perf::set_watts(watts); }

NATIVE(jstring, gpuReport)(JNIEnv* env, jclass) {
    bo2device::Gpu gpu;
    std::string report = bo2device::gpu_report(&gpu);
    report += "S26 variant: ";
    report += bo2device::s26_variant(gpu.vendor_id);
    return env->NewStringUTF(report.c_str());
}

NATIVE(void, setDisplay)(JNIEnv*, jclass, jint aspect, jint render_height) {
    bo2display::set_settings(bo2display::Settings{aspect == 1 ? bo2display::Aspect::Wide16x9 : bo2display::Aspect::Fill,
                                                  static_cast<int>(render_height)});
}

NATIVE(jintArray, displayPlan)(JNIEnv* env, jclass, jint w, jint h) {
    const bo2display::Plan p = bo2display::plan(w, h, bo2display::settings());
    const jint out[8] = {p.panel_w, p.panel_h, p.guest_w, p.guest_h, p.dest.x, p.dest.y, p.dest.w, p.dest.h};
    jintArray array = env->NewIntArray(8);
    env->SetIntArrayRegion(array, 0, 8, out);
    return array;
}

// Graphics knobs as an int array: [preset, frame_cap, shadows, ssao, dof, fxaa, msaa, textures, anisotropy,
// decals, fov, upscaler, view_distance] (graphics.hpp; -1 = the game's own, except frame_cap and upscaler, the port's own).
namespace {
constexpr int kGfxKnobs = 13;

bo2gfx::Settings gfx_from(const jint* k) {
    bo2gfx::Settings s;
    s.preset = static_cast<bo2gfx::Preset>(k[0]);
    s.frame_cap = k[1];
    s.shadows = k[2];
    s.ssao = k[3];
    s.dof = k[4];
    s.fxaa = k[5];
    s.msaa = k[6];
    s.textures = k[7];
    s.anisotropy = k[8];
    s.decals = k[9];
    s.fov = k[10];
    s.upscaler = k[11];
    s.view_distance = k[12];
    return s;
}

jintArray gfx_to(JNIEnv* env, const bo2gfx::Settings& s) {
    const jint k[kGfxKnobs] = {static_cast<jint>(s.preset), s.frame_cap, s.shadows, s.ssao, s.dof, s.fxaa,
                               s.msaa, s.textures, s.anisotropy, s.decals, s.fov, s.upscaler, s.view_distance};
    jintArray out = env->NewIntArray(kGfxKnobs);
    env->SetIntArrayRegion(out, 0, kGfxKnobs, k);
    return out;
}
}  // namespace

NATIVE(void, setGraphics)(JNIEnv* env, jclass, jintArray knobs) {
    if (!knobs || env->GetArrayLength(knobs) != kGfxKnobs) return;
    jint k[kGfxKnobs];
    env->GetIntArrayRegion(knobs, 0, kGfxKnobs, k);
    bo2gfx::set_settings(gfx_from(k));
    if (bo2_graphics_changed) bo2_graphics_changed();   // the game running: sent now (game_host.cpp)
}

NATIVE(jintArray, graphicsPreset)(JNIEnv* env, jclass, jint preset, jintArray current) {
    bo2gfx::Settings now;
    if (current && env->GetArrayLength(current) == kGfxKnobs) {
        jint k[kGfxKnobs];
        env->GetIntArrayRegion(current, 0, kGfxKnobs, k);
        now = gfx_from(k);
    }
    return gfx_to(env, bo2gfx::preset(static_cast<bo2gfx::Preset>(preset), now));
}

NATIVE(void, setThermal)(JNIEnv*, jclass, jint status, jfloat headroom) { bo2perf::set_thermal(status, headroom); }
