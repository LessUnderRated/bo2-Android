package io.github.lessunderrated.bo2;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * The physical controller straight to the game: each input event Android delivers (on the UI thread, unbuffered: see
 * Bo2Activity) updates the pad's whole state here and hands it to the native side (Native.padDirect, pad_sdl.cpp), where
 * the game's next XInputGetState reads it. Before, the state reached the game through SDL's event pump and a copy made
 * 120 times a second on SDL's thread, which falls behind when the phone is busy. SDL still sees every event (the
 * activity forwards them as before): it opens the pad, plays its rumble and handles the touch pad.
 *
 * Android's standard gamepad layout, as SDL reads it on Android: buttons by position (south A, east B, west X, north Y),
 * left stick AXIS_X/AXIS_Y, right stick AXIS_Z/AXIS_RZ, triggers AXIS_LTRIGGER/AXIS_RTRIGGER (or BRAKE/GAS), the d-pad
 * as keys or as the hat axes. BO2_PAD_DIRECT=0 (remote env) leaves the pad to SDL's copy, as before.
 */
final class PadDirect {
    private static final int UP = 0x0001, DOWN = 0x0002, LEFT = 0x0004, RIGHT = 0x0008, START = 0x0010, BACK = 0x0020,
        LTHUMB = 0x0040, RTHUMB = 0x0080, LSHOULDER = 0x0100, RSHOULDER = 0x0200, A = 0x1000, B = 0x2000, X = 0x4000,
        Y = 0x8000;
    private static final int HAT_BITS = UP | DOWN | LEFT | RIGHT;

    private final boolean enabled;
    private int keys;       // buttons held, from key events (the d-pad included when it comes as keys)
    private int hat;        // the d-pad from the hat axes
    private float lx, ly, rx, ry, lt, rt;
    private int sentButtons = -1, sentLt = -1, sentRt = -1, sentLx, sentLy, sentRx, sentRy;

    PadDirect() {
        String e = null;
        try {
            e = android.system.Os.getenv("BO2_PAD_DIRECT");
        } catch (RuntimeException ignored) {
        }
        enabled = e == null || !e.equals("0");
    }

    static boolean isPad(InputDevice device) {
        if (device == null || device.isVirtual()) {
            return false;
        }
        int s = device.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static int bit(int keyCode, boolean fromController) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return A;
            case KeyEvent.KEYCODE_BUTTON_B: return B;
            case KeyEvent.KEYCODE_BUTTON_X: return X;
            case KeyEvent.KEYCODE_BUTTON_Y: return Y;
            case KeyEvent.KEYCODE_BUTTON_L1: return LSHOULDER;
            case KeyEvent.KEYCODE_BUTTON_R1: return RSHOULDER;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return LTHUMB;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return RTHUMB;
            case KeyEvent.KEYCODE_BUTTON_START: return START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return BACK;
            case KeyEvent.KEYCODE_BACK: return fromController ? BACK : 0;   // an Xbox pad's View button
            case KeyEvent.KEYCODE_DPAD_UP: return UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return RIGHT;
            default: return 0;
        }
    }

    /** A key event from a controller; the caller still hands it on to SDL. */
    void key(KeyEvent event) {
        if (!enabled || !isPad(event.getDevice()) || event.getRepeatCount() > 0) {
            return;
        }
        int b = bit(event.getKeyCode(), true);
        if (b == 0) {
            return;
        }
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            keys |= b;
        } else if (event.getAction() == KeyEvent.ACTION_UP) {
            keys &= ~b;
        }
        send();
    }

    /** A joystick motion event (sticks, triggers, hat); the caller still hands it on to SDL. */
    void motion(MotionEvent event) {
        if (!enabled || !isPad(event.getDevice()) || event.getAction() != MotionEvent.ACTION_MOVE) {
            return;
        }
        lx = event.getAxisValue(MotionEvent.AXIS_X);
        ly = event.getAxisValue(MotionEvent.AXIS_Y);
        rx = event.getAxisValue(MotionEvent.AXIS_Z);
        ry = event.getAxisValue(MotionEvent.AXIS_RZ);
        lt = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE));
        rt = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS));
        float hx = event.getAxisValue(MotionEvent.AXIS_HAT_X), hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        hat = (hx < -0.5f ? LEFT : 0) | (hx > 0.5f ? RIGHT : 0) | (hy < -0.5f ? UP : 0) | (hy > 0.5f ? DOWN : 0);
        send();
    }

    private static int stick(float v) {   // -1..1 to XInput's -32768..32767
        return Math.max(-32768, Math.min(32767, Math.round(v * 32767f)));
    }

    private static int trigger(float v) {
        return Math.max(0, Math.min(255, Math.round(v * 255f)));
    }

    private void send() {
        // A d-pad that comes as keys and as a hat at once: either one held counts.
        int buttons = keys | (hat & HAT_BITS);
        int l = trigger(lt), r = trigger(rt);
        // Android's Y points down, XInput's up.
        int x1 = stick(lx), y1 = stick(-ly), x2 = stick(rx), y2 = stick(-ry);
        if (buttons == sentButtons && l == sentLt && r == sentRt && x1 == sentLx && y1 == sentLy && x2 == sentRx
            && y2 == sentRy) {
            return;
        }
        sentButtons = buttons; sentLt = l; sentRt = r; sentLx = x1; sentLy = y1; sentRx = x2; sentRy = y2;
        try {
            Native.padDirect(buttons, l, r, x1, y1, x2, y2);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }
}
