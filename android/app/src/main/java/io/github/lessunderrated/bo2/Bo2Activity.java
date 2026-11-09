package io.github.lessunderrated.bo2;

import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

/**
 * SDL's activity, loading libSDL3.so and libmain.so (whose SDL_main is android/native/src/main.cpp), with
 * the touch controls over it and the app's own menu. Adapted from LessUnderRated's Chronicle-rs
 * ChronicleActivity: the menu, the controls' editor strip, Back opening the menu, landscape only.
 */
public class Bo2Activity extends SDLActivity implements TouchOverlay.Listener {
    private TouchOverlay overlay;
    private LinearLayout editPanel;
    private TextView editLabel;
    private SeekBar editSize;
    private SeekBar editOpacity;
    private boolean menuOpen;
    private Power power;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) {
            return;  // SDL shows its own error dialog; nothing here may call into libmain.so
        }
        SetupActivity.gameRunning = true;
        CrashReport.open(this);
        applyDisplay(this);
        unbufferedPad();
        applyGraphics(this);
        overlay = new TouchOverlay(this, this);
        addContentView(overlay, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT));
        buildEditPanel();
        power = new Power(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (power != null) {
            power.start();
        }
    }

    @Override
    protected void onPause() {
        if (power != null) {
            power.stop();
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        SetupActivity.gameRunning = false;
        super.onDestroy();
    }

    static final String KEY_ASPECT = "display_aspect";
    static final String KEY_HEIGHT = "display_height";
    /** Render heights the menu offers; 0 is the panel's own. 720 is what bo2-mac renders on a Mac. */
    static final int[] HEIGHTS = {720, 900, 1080, 0};
    /** The first-run height: 1080. Measured on the S26 in a match at 720p, the GPU sat at its lowest clock
     *  (160 MHz of 1.3 GHz) while the CPU set the frame rate, so the full panel height costs little. */
    static final int DEFAULT_HEIGHT = 1080;

    /** The saved screen settings to the native side (main.cpp's plan, later the runtime's monitor). */
    static void applyDisplay(android.content.Context context) {
        SharedPreferences prefs = TouchOverlay.prefs(context);
        try {
            Native.setDisplay(prefs.getInt(KEY_ASPECT, 0), prefs.getInt(KEY_HEIGHT, DEFAULT_HEIGHT));
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    static final String KEY_GRAPHICS = "graphics";
    static final int K_PRESET = 0, K_CAP = 1, K_FOV = 10, K_UPSCALER = 11, K_VIEW = 12, N_KNOBS = 13;
    static final int[] CAPS = {30, 40, 45, 60, 90, 120, 0};
    static final int[] FOVS = {-1, 65, 70, 75, 80, 85, 90};
    static final int[] UPSCALERS = {1, 0};   // SGSR, bilinear (graphics.hpp)
    /** View distance in game units (inches); -1 the map's own (graphics.hpp, view_distance). */
    static final int[] VIEWS = {-1, 12000, 8000, 6000, 5000, 4000, 3000};

    /** The saved graphics knobs, or the High preset with a 60 fps cap the first time. */
    static int[] graphics(android.content.Context context) {
        String saved = TouchOverlay.prefs(context).getString(KEY_GRAPHICS, null);
        if (saved != null) {
            String[] parts = saved.split(",");
            // Fewer knobs: saved before the upscaler (11) or the view distance (12) existed. The upscaler starts as
            // SGSR and the view distance as the saved preset's, as if the preset had been chosen again.
            if (parts.length >= N_KNOBS - 2 && parts.length <= N_KNOBS) {
                int[] k = new int[N_KNOBS];
                k[K_UPSCALER] = 1;
                k[K_VIEW] = -1;
                for (int i = 0; i < parts.length; i++) {
                    k[i] = Integer.parseInt(parts[i].trim());
                }
                if (parts.length < N_KNOBS && k[K_PRESET] >= 0 && k[K_PRESET] <= 3) {
                    k[K_VIEW] = Native.graphicsPreset(k[K_PRESET], k)[K_VIEW];
                }
                return k;
            }
        }
        int[] start = {2, 60, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, -1};
        return Native.graphicsPreset(2, start);
    }

    /** "Map's own", or the distance in metres (a game unit is an inch). */
    static String viewName(android.content.Context c, int units) {
        return units <= 0 ? c.getString(R.string.gfx_view_map) : Math.round(units * 0.0254) + " m";
    }

    static void saveGraphics(android.content.Context context, int[] k) {
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < k.length; i++) {
            text.append(i == 0 ? "" : ",").append(k[i]);
        }
        TouchOverlay.prefs(context).edit().putString(KEY_GRAPHICS, text.toString()).apply();
        Native.setGraphics(k);
    }

    static void applyGraphics(android.content.Context context) {
        try {
            Native.setGraphics(graphics(context));
        } catch (UnsatisfiedLinkError | NumberFormatException ignored) {
        }
    }

    static String presetName(android.content.Context c, int preset) {
        switch (preset) {
            case 0: return c.getString(R.string.gfx_low);
            case 1: return c.getString(R.string.gfx_medium);
            case 2: return c.getString(R.string.gfx_high);
            case 3: return c.getString(R.string.gfx_own);
            default: return c.getString(R.string.gfx_custom);
        }
    }

    static String capName(android.content.Context c, int cap) {
        return cap <= 0 ? c.getString(R.string.gfx_uncapped) : cap + " fps";
    }

    static String upscalerName(android.content.Context c, int upscaler) {
        return c.getString(upscaler == 1 ? R.string.gfx_upscaler_sgsr : R.string.gfx_upscaler_off);
    }

    /** Graphics: the preset, the frame cap, the field of view, the upscaler, and each knob on its own. */
    private void showGraphics() {
        int[] k = graphics(this);
        String[] rows = {
            getString(R.string.gfx_preset, presetName(this, k[K_PRESET])),
            getString(R.string.gfx_cap, capName(this, k[K_CAP])),
            getString(R.string.gfx_fov, k[K_FOV] < 0 ? getString(R.string.gfx_game) : k[K_FOV] + "°"),
            getString(R.string.gfx_upscaler, upscalerName(this, k[K_UPSCALER])),
            getString(R.string.gfx_view, viewName(this, k[K_VIEW])),
            getString(R.string.gfx_advanced),
        };
        dialog()
            .setTitle(R.string.menu_graphics_title)
            .setItems(rows, (d, which) -> {
                if (which == 0) {
                    String[] names = {presetName(this, 0), presetName(this, 1), presetName(this, 2), presetName(this, 3)};
                    dialog().setTitle(R.string.menu_graphics_title)
                        .setSingleChoiceItems(names, Math.min(k[K_PRESET], 3), (d2, p) -> {
                            saveGraphics(this, Native.graphicsPreset(p, k));
                            d2.dismiss();
                            showGraphics();
                        }).setOnCancelListener(d2 -> showGraphics()).show();
                } else if (which == 1) {
                    choose(CAPS, k[K_CAP], v -> capName(this, v), v -> { k[K_CAP] = v; saveGraphics(this, k); });
                } else if (which == 2) {
                    choose(FOVS, k[K_FOV], v -> v < 0 ? getString(R.string.gfx_game) : v + "°",
                        v -> { k[K_FOV] = v; saveGraphics(this, k); });
                } else if (which == 3) {
                    // The port's presentation, not the game's: the preset stays what it was.
                    choose(UPSCALERS, k[K_UPSCALER], v -> upscalerName(this, v), v -> { k[K_UPSCALER] = v; saveGraphics(this, k); });
                } else if (which == 4) {
                    // A cost knob like the advanced ones: the preset becomes Custom.
                    choose(VIEWS, k[K_VIEW], v -> viewName(this, v),
                        v -> { k[K_VIEW] = v; k[K_PRESET] = 4; saveGraphics(this, k); });
                } else {
                    showAdvanced();
                }
            })
            .setNegativeButton(R.string.menu_back, (d, w) -> onMenu())
            .setOnCancelListener(d -> onMenu())
            .show();
    }

    private interface Namer {
        String name(int value);
    }

    private interface Sink {
        void set(int value);
    }

    private void choose(int[] values, int current, Namer namer, Sink sink) {
        String[] names = new String[values.length];
        int at = -1;
        for (int i = 0; i < values.length; i++) {
            names[i] = namer.name(values[i]);
            if (values[i] == current) {
                at = i;
            }
        }
        dialog().setSingleChoiceItems(names, at, (d, which) -> {
            sink.set(values[which]);
            d.dismiss();
            showGraphics();
        }).setOnCancelListener(d -> showGraphics()).show();
    }

    // Advanced: each knob, tapping steps through its values; -1 is the game's own (your config's value).
    static final int[][] STEPS = {
        null, null,
        {-1, 0, 1, 2},          // shadows: own, off, quarter-res sun, full
        {-1, 0},                // ambient occlusion: own, off
        {-1, 0, 1},             // depth of field
        {-1, 0, 1},             // FXAA
        {-1, 1, 2, 4},          // MSAA samples
        {-1, 0, 1, 2},          // textures: own, high, medium, low
        {-1, 1, 2, 4, 8, 16},   // anisotropy
        {-1, 0, 1},             // decals
    };
    static final int[] KNOB_NAMES = {0, 0, R.string.gfx_shadows, R.string.gfx_ssao, R.string.gfx_dof,
        R.string.gfx_fxaa, R.string.gfx_msaa, R.string.gfx_textures, R.string.gfx_aniso, R.string.gfx_decals};

    static String knobValue(android.content.Context c, int knob, int v) {
        if (v < 0) {
            return c.getString(R.string.gfx_game);
        }
        switch (knob) {
            case 2: return c.getResources().getStringArray(R.array.gfx_shadow_values)[v];
            case 7: return c.getResources().getStringArray(R.array.gfx_texture_values)[v];
            case 6: return v == 1 ? c.getString(R.string.off) : v + "×";
            case 8: return v + "×";
            default: return c.getString(v == 0 ? R.string.off : R.string.on);
        }
    }

    private void showAdvanced() {
        int[] k = graphics(this);
        String[] rows = new String[STEPS.length - 2];
        for (int knob = 2; knob < STEPS.length; knob++) {
            rows[knob - 2] = getString(KNOB_NAMES[knob]) + ": " + knobValue(this, knob, k[knob]);
        }
        dialog()
            .setTitle(R.string.gfx_advanced)
            .setItems(rows, (d, which) -> {
                int knob = which + 2;
                int[] steps = STEPS[knob];
                int at = 0;
                for (int i = 0; i < steps.length; i++) {
                    if (steps[i] == k[knob]) {
                        at = i;
                    }
                }
                k[knob] = steps[(at + 1) % steps.length];
                k[K_PRESET] = 4;  // custom
                saveGraphics(this, k);
                showAdvanced();
            })
            .setNegativeButton(R.string.menu_back, (d, w) -> showGraphics())
            .setOnCancelListener(d -> showGraphics())
            .show();
    }

    static String heightName(android.content.Context c, int height) {
        return height == 0 ? c.getString(R.string.display_native) : height + "p";
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    /** Landscape only, either way up; the manifest's sensorLandscape stands over SDL's request. */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    /**
     * The phone's Back opens the menu rather than leaving the game. A controller's Back (an Xbox pad's View
     * button, which Android reports as KEYCODE_BACK) is the game's Back (the scoreboard) instead, and goes to
     * SDL with the controller's other buttons. Chronicle 0.7.18 learned this one the hard way.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        padDirect.key(event);   // the controller straight to the game; SDL still gets it below
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && !fromController(event)) {
            if (event.getAction() == KeyEvent.ACTION_UP) {
                if (editPanel.getVisibility() == View.VISIBLE) {
                    finishEditing();
                } else if (!menuOpen) {
                    onMenu();
                }
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    /**
     * A controller's sticks and triggers. Android sends a joystick's motion to the FOCUSED view, and SDL listens for
     * it on its surface only; once the touch overlay or the menu has focus, the buttons (key events, which the
     * activity hands to SDL) still worked while the sticks went nowhere. SDL's own listener also wants the source to
     * be exactly SOURCE_JOYSTICK, and a pad may report joystick and gamepad together. So the activity takes every
     * joystick-class motion event, whoever has focus, and gives it to SDL's controller handler.
     */
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0) {
            padDirect.motion(event);   // the controller straight to the game; SDL still gets it
            return SDLControllerManager.handleJoystickMotionEvent(event);
        }
        return super.dispatchGenericMotionEvent(event);
    }

    /** The controller's state, handed to the game as each event arrives (PadDirect.java). */
    private final PadDirect padDirect = new PadDirect();

    /**
     * Android holds a joystick's motion events back and delivers them once per frame, batched; a game wants each one
     * as it comes. Asked of SDL's surface (it has focus while playing) and of the window's root, where the events
     * enter the view tree.
     */
    private void unbufferedPad() {
        if (mSurface != null) {
            mSurface.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_JOYSTICK);
        }
        getWindow().getDecorView().requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_JOYSTICK);
    }

    private static boolean fromController(KeyEvent event) {
        int source = event.getSource();
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
    }

    // ---- The menu -----------------------------------------------------------------------------

    /**
     * The app's menu. It does not pause the game: Black Ops II's own pause is Start, and multiplayer
     * against bots keeps running on Windows too while a menu is up.
     */
    @Override
    public void onMenu() {
        menuOpen = true;
        SharedPreferences prefs = TouchOverlay.prefs(this);
        int look = prefs.getInt(TouchOverlay.KEY_LOOK, 0);
        float gain = prefs.getFloat(TouchOverlay.KEY_LOOK_GAIN, 1.0f);
        boolean sprint = prefs.getBoolean(TouchOverlay.KEY_AUTO_SPRINT, true);
        int aspect = prefs.getInt(KEY_ASPECT, 0);
        int height = prefs.getInt(KEY_HEIGHT, DEFAULT_HEIGHT);
        String[] items = {
            getString(R.string.menu_resume),
            getString(R.string.menu_screen, getString(aspect == 0 ? R.string.display_fill : R.string.display_16_9)),
            getString(R.string.menu_resolution, heightName(this, height)),
            getString(R.string.menu_graphics, presetName(this, graphics(this)[K_PRESET]), capName(this, graphics(this)[K_CAP])),
            getString(R.string.menu_edit_controls),
            getString(R.string.menu_look, getString(look == 0 ? R.string.menu_look_swipe : R.string.menu_look_stick)),
            getString(R.string.menu_look_speed, String.format(java.util.Locale.ROOT, "%.2g×", gain)),
            getString(R.string.menu_auto_sprint, getString(sprint ? R.string.on : R.string.off)),
            getString(R.string.menu_opacity, Math.round(overlay.globalOpacity() * 100)),
            overlay.hidden() ? getString(R.string.menu_show_controls) : getString(R.string.menu_hide_controls),
            getString(R.string.menu_share_log),
            getString(R.string.menu_quit),
        };
        dialog()
            .setTitle(R.string.app_name)
            .setItems(items, (d, which) -> {
                if (which == 3) {
                    showGraphics();
                    return;
                }
                switch (which > 3 ? which - 3 : which - 2) {
                    case -1:
                        prefs.edit().putInt(KEY_ASPECT, aspect == 0 ? 1 : 0).apply();
                        applyDisplay(this);
                        onMenu();
                        return;
                    case 0:
                        chooseHeight(prefs, height);
                        return;
                    case 1:
                        startEditing();
                        return;
                    case 2:
                        prefs.edit().putInt(TouchOverlay.KEY_LOOK, look == 0 ? 1 : 0).apply();
                        overlay.applyLook();
                        onMenu();
                        return;
                    case 3:
                        chooseLookSpeed(prefs, gain);
                        return;
                    case 4:
                        prefs.edit().putBoolean(TouchOverlay.KEY_AUTO_SPRINT, !sprint).apply();
                        overlay.applyLook();
                        onMenu();
                        return;
                    case 5:
                        chooseOpacity();
                        return;
                    case 6:
                        overlay.setHidden(!overlay.hidden());
                        resume();
                        return;
                    case 7:
                        CrashReport.share(this, CrashReport.currentRun(this));
                        resume();
                        return;
                    case 8:
                        finishAffinity();
                        System.exit(0);
                        return;
                    default:
                        resume();
                }
            })
            .setOnCancelListener(d -> resume())
            .show();
    }

    private void chooseHeight(SharedPreferences prefs, int current) {
        String[] names = new String[HEIGHTS.length];
        int at = 0;
        for (int i = 0; i < HEIGHTS.length; i++) {
            names[i] = heightName(this, HEIGHTS[i]);
            if (HEIGHTS[i] == current) {
                at = i;
            }
        }
        dialog()
            .setTitle(getString(R.string.menu_resolution, ""))
            .setSingleChoiceItems(names, at, (d, which) -> {
                prefs.edit().putInt(KEY_HEIGHT, HEIGHTS[which]).apply();
                applyDisplay(this);
                d.dismiss();
                onMenu();
            })
            .setOnCancelListener(d -> onMenu())
            .show();
    }

    private static final float[] GAINS = {0.5f, 0.7f, 1.0f, 1.4f, 2.0f, 2.8f, 4.0f};

    private void chooseLookSpeed(SharedPreferences prefs, float current) {
        String[] names = new String[GAINS.length];
        int at = 2;
        for (int i = 0; i < GAINS.length; i++) {
            names[i] = String.format(java.util.Locale.ROOT, "%.2g×", GAINS[i]);
            if (Math.abs(GAINS[i] - current) < 1e-3) {
                at = i;
            }
        }
        dialog()
            .setTitle(getString(R.string.menu_look_speed, ""))
            .setSingleChoiceItems(names, at, (d, which) -> {
                prefs.edit().putFloat(TouchOverlay.KEY_LOOK_GAIN, GAINS[which]).apply();
                overlay.applyLook();
                d.dismiss();
                onMenu();
            })
            .setOnCancelListener(d -> onMenu())
            .show();
    }

    private void chooseOpacity() {
        String[] names = new String[10];
        for (int i = 0; i < 10; i++) {
            names[i] = ((i + 1) * 10) + "%";
        }
        int at = Math.round(overlay.globalOpacity() * 10) - 1;
        dialog()
            .setTitle(getString(R.string.menu_opacity, Math.round(overlay.globalOpacity() * 100)))
            .setSingleChoiceItems(names, Math.max(0, Math.min(9, at)), (d, which) -> {
                overlay.setGlobalOpacity((which + 1) / 10f);
                d.dismiss();
                onMenu();
            })
            .setOnCancelListener(d -> onMenu())
            .show();
    }

    private void resume() {
        menuOpen = false;
    }

    // ---- The controls' editor -----------------------------------------------------------------

    /**
     * A slim strip along the top edge: what is picked, its size, the opacity of all the controls, Reset all
     * and Done. Hidden while a control is dragged, and at the bottom edge while the one placed is near the top.
     */
    private void buildEditPanel() {
        editPanel = new LinearLayout(this);
        editPanel.setOrientation(LinearLayout.HORIZONTAL);
        editPanel.setGravity(Gravity.CENTER_VERTICAL);
        editPanel.setBackgroundColor(Color.argb(0xC0, 0x10, 0x12, 0x1A));
        editPanel.setPadding(dp(12), dp(2), dp(8), dp(2));
        editLabel = new TextView(this);
        editLabel.setTextColor(Color.WHITE);
        editLabel.setTextSize(12);
        editLabel.setMaxLines(2);
        editPanel.addView(editLabel, new LinearLayout.LayoutParams(dp(120), ViewGroup.LayoutParams.WRAP_CONTENT));
        editSize = slider(R.string.edit_size, 25, 250, value -> overlay.setSelectedScale(value / 100f));
        editOpacity = slider(R.string.edit_opacity, 5, 100, value -> overlay.setGlobalOpacity(value / 100f));
        Button reset = new Button(this, null, android.R.attr.borderlessButtonStyle);
        reset.setText(R.string.edit_reset);
        reset.setTextColor(Color.WHITE);
        reset.setOnClickListener(v -> overlay.resetLayout());
        editPanel.addView(reset);
        Button done = new Button(this, null, android.R.attr.borderlessButtonStyle);
        done.setText(R.string.edit_done);
        done.setTextColor(Color.WHITE);
        done.setOnClickListener(v -> finishEditing());
        editPanel.addView(done);
        editPanel.setVisibility(View.GONE);
        addContentView(editPanel, editPanelAt(Gravity.TOP));
    }

    private FrameLayout.LayoutParams editPanelAt(int edge) {
        return new FrameLayout.LayoutParams(Math.min(dp(640), getResources().getDisplayMetrics().widthPixels - dp(32)),
            ViewGroup.LayoutParams.WRAP_CONTENT, edge | Gravity.CENTER_HORIZONTAL);
    }

    @Override
    public void onEditDrag(boolean dragging, float y) {
        if (dragging) {
            editPanel.setVisibility(View.INVISIBLE);
            return;
        }
        int edge = y < overlay.getHeight() * 0.4f ? Gravity.BOTTOM : Gravity.TOP;
        editPanel.setLayoutParams(editPanelAt(edge));
        editPanel.setVisibility(View.VISIBLE);
    }

    private interface IntSink {
        void accept(int value);
    }

    private SeekBar slider(int label, int min, int max, IntSink sink) {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        TextView name = new TextView(this);
        name.setText(label);
        name.setTextColor(Color.LTGRAY);
        name.setTextSize(11);
        column.addView(name);
        SeekBar bar = new SeekBar(this);
        bar.setMin(min);
        bar.setMax(max);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (fromUser) {
                    sink.accept(progress);
                }
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {}
        });
        column.addView(bar);
        editPanel.addView(column, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return bar;
    }

    private void startEditing() {
        menuOpen = true;
        overlay.setEditing(true);
        onSelectionChanged(null);
        editPanel.setLayoutParams(editPanelAt(Gravity.TOP));
        editPanel.setVisibility(View.VISIBLE);
    }

    private void finishEditing() {
        overlay.setEditing(false);
        editPanel.setVisibility(View.GONE);
        resume();
    }

    @Override
    public void onSelectionChanged(String id) {
        boolean any = id != null && !id.equals("menu");
        editLabel.setText(id != null ? getString(R.string.edit_selected, id.toUpperCase()) : getString(R.string.edit_hint));
        editSize.setEnabled(any);
        if (any) {
            editSize.setProgress(Math.round(overlay.selectedScale() * 100));
        }
        editOpacity.setProgress(Math.round(overlay.globalOpacity() * 100));
    }
}
