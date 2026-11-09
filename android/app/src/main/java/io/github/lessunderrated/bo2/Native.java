package io.github.lessunderrated.bo2;

/**
 * libmain.so's side (android/native/src/jni.cpp). Sizes and places are in the window's pixels. A layout
 * is a flat array: [stick radius, left stick rest x, rest y] then, per control in elementIds() order,
 * [circle (1) or rect (0), x, y, r, w, h].
 */
final class Native {
    private Native() {}

    static final int STRIDE = 6;
    static final int HEADER = 3;

    /** Starts this run's log in dir, keeping the last run's (android/native/src/log.cpp). */
    static native void openLog(String dir);

    /** The launcher's Logging option, live: 0 Full, 1 Normal (no sampling profiler), 2 Minimal (log file only). */
    static native void setDiagnostics(int level);

    /** The physical controller's whole state, XInput layout and ranges (PadDirect.java, on each input event). */
    static native void padDirect(int buttons, int lt, int rt, int lx, int ly, int rx, int ry);

    /** A line, or several, for the log. */
    static native void log(String text);

    /** The controls' names, in the layout arrays' order. */
    static native String[] elementIds();

    /** The default layout for a window and its safe-area insets. */
    static native float[] defaultLayout(int width, int height, float left, float top, float right, float bottom);

    /** Installs the default layout with the player's arrangement over it (null: none). */
    static native void setLayout(int width, int height, float left, float top, float right, float bottom, float[] placed);

    /** A touch on the controls: action 0 down, 1 move, 2 up or cancelled; time is MotionEvent.getEventTime(). */
    static native void touch(int action, long pointer, float x, float y, long timeMs);

    /** What is pressed, as SDL gamepad button bits, with LT and RT as bits 30 and 31. */
    static native int padButtons();

    /** Each stick a finger holds: [left (1/0), centre x, centre y, knob x, knob y] per stick. */
    static native float[] padSticks();

    /** Whether the touch pad is attached (it steps aside while a controller is connected). */
    static native boolean touchActive();

    /** Look mode (0 swipe, 1 stick), swipe gain, swipe floor (beats the game's dead zone), auto-sprint. */
    static native void setLook(int mode, float gain, float floor, boolean autoSprint);

    /** The battery's power draw, for the [PERF] log lines; -1 when unknown. */
    static native void setWatts(float watts);

    /** The game's screen: aspect 0 fills the panel, 1 is 16:9 with bars; render height in lines, 0 native. */
    static native void setDisplay(int aspect, int renderHeight);

    /** The plan for a w x h panel: [panel w, h, game w, h, drawn x, y, w, h] (display_plan.cpp). */
    static native int[] displayPlan(int width, int height);

    /**
     * Graphics knobs (graphics.hpp): [preset, frame cap, shadows, ssao, dof, fxaa, msaa, textures, anisotropy,
     * decals, fov, upscaler]; -1 is the game's own value, from the player's config files. upscaler is the port's own
     * (the presentation pass, not a dvar): 0 bilinear, 1 SGSR. An array of any other length is ignored.
     */
    static native void setGraphics(int[] knobs);

    /** The knobs a preset (0 low, 1 medium, 2 high, 3 the game's own) sets, keeping the cap and FOV of current. */
    static native int[] graphicsPreset(int preset, int[] current);

    /** PowerManager's thermal status and headroom, for the [PERF] log lines. */
    static native void setThermal(int status, float headroom);

    /** The Vulkan GPUs and drivers, and which Galaxy S26 model that means. */
    static native String gpuReport();
}
