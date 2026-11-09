package io.github.lessunderrated.bo2;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Handler;
import android.os.Looper;

/**
 * The device's power draw for the [PERF] log lines (android/native/src/perf.cpp): the battery's current
 * and voltage, read here on the main thread twice a second and handed to the game, so the game's threads never call
 * into Java for it.
 */
final class Power {
    private static final long PERIOD_MS = 500;

    private final Context context;
    private final BatteryManager battery;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            try {
                Native.setWatts(read());
                Native.setThermal(thermal == null ? -1 : thermal.getCurrentThermalStatus(), headroom());
            } catch (UnsatisfiedLinkError e) {
                return;  // no native side to tell: stop polling
            }
            handler.postDelayed(this, PERIOD_MS);
        }
    };

    private final android.os.PowerManager thermal;

    Power(Context context) {
        this.context = context;
        battery = (BatteryManager) context.getSystemService(Context.BATTERY_SERVICE);
        thermal = (android.os.PowerManager) context.getSystemService(Context.POWER_SERVICE);
    }

    /**
     * How close the phone is to throttling, forecast 10 s ahead: 1.0 is where throttling starts. NaN (no
     * support, or asked too often: Android allows about one call a second) reads as -1.
     */
    private float headroom() {
        if (thermal == null) {
            return -1f;
        }
        float h = thermal.getThermalHeadroom(10);
        return Float.isNaN(h) ? -1f : h;
    }

    void start() {
        handler.removeCallbacks(poll);
        handler.post(poll);
    }

    void stop() {
        handler.removeCallbacks(poll);
    }

    private float read() {
        try {
            if (battery == null) {
                return -1f;
            }
            int current = battery.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW);
            // ACTION_BATTERY_CHANGED is sticky: registering no receiver returns its latest intent.
            Intent status = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            int millivolts = status != null ? status.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1) : -1;
            return watts(current, millivolts);
        } catch (RuntimeException e) {
            return -1f;
        }
    }

    /**
     * Watts from BATTERY_PROPERTY_CURRENT_NOW and the voltage in millivolts, or -1 when either is unknown. The current
     * is microamps by the documentation, but some phones (Samsung's among them) report milliamps: a running game
     * draws hundreds of milliamps, so a magnitude under 20000 is taken as milliamps.
     */
    static float watts(int current, int millivolts) {
        if (current == Integer.MIN_VALUE || current == 0 || millivolts <= 0) {
            return -1f;
        }
        double amps = Math.abs((double) current);
        amps /= amps < 20000 ? 1e3 : 1e6;
        return (float) (amps * millivolts / 1e3);
    }
}
