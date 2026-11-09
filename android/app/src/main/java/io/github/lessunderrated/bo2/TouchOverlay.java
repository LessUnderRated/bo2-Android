package io.github.lessunderrated.bo2;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.Map;

/**
 * The touch controls, drawn over the game. Adapted from LessUnderRated's Chronicle-rs / ChronicleTwo-rs
 * Android TouchOverlay: the native side (touch_model.cpp) owns what the controls press and presents it to
 * SDL as a gamepad; this view draws them, hands it every touch, keeps the player's arrangement, and runs
 * the app's menu button (top right). In edit mode a touch picks a control up and moves it instead, and the
 * activity's panel sets the size of the one picked. One opacity covers them all.
 *
 * Changed for Black Ops II: Xbox ABXY in Xbox colours, LB/RB/LT/RT, a large FIRE button that also aims,
 * melee (R3) and sprint (L3) buttons, and no emulator speed toggle.
 */
@SuppressLint("ViewConstructor")
public class TouchOverlay extends View {
    interface Listener {
        void onMenu();

        void onSelectionChanged(String id);

        /** A control picked up (true) or put down (false) in edit mode, and where it is now. */
        void onEditDrag(boolean dragging, float y);
    }

    private static final String PREFS = "touch";
    private static final String KEY_LAYOUT = "layout";
    private static final String KEY_OPACITY = "opacity";
    static final String KEY_HIDDEN = "hidden";
    static final String KEY_LOOK = "look";
    static final String KEY_LOOK_GAIN = "look_gain";
    static final String KEY_AUTO_SPRINT = "auto_sprint";

    // SDL gamepad button numbers (touch_model.hpp), and LT/RT as bits 30 and 31.
    private static final int SOUTH = 0, EAST = 1, WEST = 2, NORTH = 3, BACK = 4, START = 6, L3 = 7, R3 = 8;
    private static final int LB = 9, RB = 10, UP = 11, DOWN = 12, LEFT = 13, RIGHT = 14;
    private static final Map<String, Integer> BITS = new HashMap<>();
    private static final Map<String, Integer> COLORS = new HashMap<>();
    private static final Map<String, String> LABELS = new HashMap<>();

    static {
        BITS.put("a", 1 << SOUTH);
        BITS.put("b", 1 << EAST);
        BITS.put("x", 1 << WEST);
        BITS.put("y", 1 << NORTH);
        BITS.put("back", 1 << BACK);
        BITS.put("start", 1 << START);
        BITS.put("sprint", 1 << L3);
        BITS.put("melee", 1 << R3);
        BITS.put("lb", 1 << LB);
        BITS.put("rb", 1 << RB);
        BITS.put("lt", 1 << 30);
        BITS.put("rt", 1 << 31);
        BITS.put("fire", 1 << 31);
        COLORS.put("a", Color.rgb(0x5C, 0xC8, 0x5C));
        COLORS.put("b", Color.rgb(0xFF, 0x5A, 0x5A));
        COLORS.put("x", Color.rgb(0x4C, 0x9A, 0xFF));
        COLORS.put("y", Color.rgb(0xFF, 0xC8, 0x40));
        COLORS.put("fire", Color.rgb(0xFF, 0x6A, 0x00));
        LABELS.put("a", "A");
        LABELS.put("b", "B");
        LABELS.put("x", "X");
        LABELS.put("y", "Y");
        LABELS.put("lb", "LB");
        LABELS.put("rb", "RB");
        LABELS.put("lt", "LT");
        LABELS.put("rt", "RT");
        LABELS.put("fire", "FIRE");
        LABELS.put("melee", "R3");
        LABELS.put("sprint", "L3");
        LABELS.put("back", "BACK");
        LABELS.put("start", "START");
        LABELS.put("menu", "☰");
    }

    private final Listener listener;
    private final SharedPreferences prefs;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);

    private String[] ids;
    /** The controls as installed (Native.setLayout), and the window's default. */
    private float[] layout;
    private float[] defaults;
    private float menuX, menuY, menuR;
    private int buttons;
    private float[] sticks = new float[0];
    private boolean active = true;
    private float globalOpacity;
    private boolean hidden;

    private boolean editing;
    private int selected = -1;
    private boolean menuSelected;
    private final Map<Integer, Boolean> onMenu = new HashMap<>();
    private float dragX, dragY;
    private int dragPointer = -1;
    /** The window size the layout was made for (what save() records). */
    private int layoutW, layoutH;
    private float insetLeft, insetTop, insetRight, insetBottom;

    TouchOverlay(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        globalOpacity = prefs.getFloat(KEY_OPACITY, 0.6f);
        hidden = prefs.getBoolean(KEY_HIDDEN, false);
        edge.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setWillNotDraw(false);
        applyLook();
    }

    static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    void applyLook() {
        try {
            Native.setLook(prefs.getInt(KEY_LOOK, 0), prefs.getFloat(KEY_LOOK_GAIN, 1.0f), 0.28f,
                prefs.getBoolean(KEY_AUTO_SPRINT, true));
        } catch (UnsatisfiedLinkError e) {
            layout = null;  // no native side: draw nothing, take no touches
        }
    }

    // A controller connecting takes the touch pad away without a touch; look every half second.
    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (!editing) {
                refreshState();
            }
            postDelayed(this, 500);
        }
    };

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        post(poll);
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(poll);
        super.onDetachedFromWindow();
    }

    // ---- Layout -------------------------------------------------------------------------------

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        // Ignoring visibility: the bars showing or hiding over the game must not move the controls (and drop
        // the fingers on them) in the middle of a fight.
        Insets safe = insets.getInsetsIgnoringVisibility(WindowInsets.Type.systemBars() |
            WindowInsets.Type.systemGestures() | WindowInsets.Type.mandatorySystemGestures() |
            WindowInsets.Type.tappableElement() | WindowInsets.Type.displayCutout());
        insetLeft = safe.left;
        insetTop = safe.top;
        insetRight = safe.right;
        insetBottom = safe.bottom;
        relayout();
        return super.onApplyWindowInsets(insets);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        relayout();
    }

    private void relayout() {
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) {
            return;
        }
        if (editing && layout != null) {
            save();  // keep the edits so far: they are restored, scaled, below
        }
        try {
            if (ids == null) {
                ids = Native.elementIds();
            }
            defaults = Native.defaultLayout(w, h, insetLeft, insetTop, insetRight, insetBottom);
            layout = defaults.clone();
            restore(w, h, prefs.getString(KEY_LAYOUT, null));
            for (int i = 0; i < ids.length; i++) {
                clampToWindow(i, w, h);
            }
            Native.setLayout(w, h, insetLeft, insetTop, insetRight, insetBottom, layout);
            layoutW = w;
            layoutH = h;
        } catch (UnsatisfiedLinkError e) {
            layout = null;
            return;
        }
        float u = Math.min(w, h);
        menuR = u * 0.04f;
        menuX = w - insetRight - u * 0.02f - menuR;
        menuY = insetTop + u * 0.02f + menuR;
        try {
            JSONObject menu = new JSONObject(prefs.getString("menu", "{}"));
            if (menu.has("fx")) {
                menuX = (float) menu.getDouble("fx") * w;
                menuY = (float) menu.getDouble("fy") * h;
            }
        } catch (JSONException ignored) {
        }
        menuX = Math.max(menuR, Math.min(w - menuR, menuX));
        menuY = Math.max(menuR, Math.min(h - menuR, menuY));
        edge.setStrokeWidth(u * 0.005f);
        invalidate();
    }

    /**
     * The player's arrangement over the default: each control it names takes its place and size, scaled from
     * the window it was made on to this one (sizes by the smaller ratio, so a round button stays round).
     */
    private void restore(int w, int h, String saved) {
        if (saved == null) {
            return;
        }
        try {
            JSONObject mine = new JSONObject(saved);
            double sw = mine.optDouble("width"), sh = mine.optDouble("height");
            if (!(sw > 0 && sh > 0)) {
                return;
            }
            float sx = (float) (w / sw), sy = (float) (h / sh), size = Math.min(sx, sy);
            JSONArray list = mine.optJSONArray("elements");
            for (int i = 0; list != null && i < list.length(); i++) {
                JSONObject o = list.optJSONObject(i);
                int at = o == null ? -1 : index(o.optString("id"));
                if (at < 0) {
                    continue;
                }
                int base = Native.HEADER + Native.STRIDE * at;
                boolean circle = layout[base] == 1f;
                if (circle != "circle".equals(o.optString("shape"))) {
                    continue;
                }
                layout[base + 1] = (float) o.optDouble("x") * sx;
                layout[base + 2] = (float) o.optDouble("y") * sy;
                layout[base + 3] = (float) o.optDouble("r", 0) * size;
                layout[base + 4] = (float) o.optDouble("w", 0) * size;
                layout[base + 5] = (float) o.optDouble("h", 0) * size;
            }
        } catch (JSONException ignored) {
        }
    }

    /**
     * Keeps control i on the window: a circle's centre inside it, a rect wholly inside it when it fits. The
     * native side clamps by the same rule (Model::place), so what is drawn is what is hit.
     */
    private void clampToWindow(int i, int w, int h) {
        int base = Native.HEADER + Native.STRIDE * i;
        if (layout[base] == 1f) {
            layout[base + 1] = Math.max(0, Math.min(w, layout[base + 1]));
            layout[base + 2] = Math.max(0, Math.min(h, layout[base + 2]));
        } else {
            layout[base + 1] = Math.max(0, Math.min(Math.max(0, w - layout[base + 4]), layout[base + 1]));
            layout[base + 2] = Math.max(0, Math.min(Math.max(0, h - layout[base + 5]), layout[base + 2]));
        }
    }

    private int index(String id) {
        for (int i = 0; ids != null && i < ids.length; i++) {
            if (ids[i].equals(id)) {
                return i;
            }
        }
        return -1;
    }

    private void save() {
        if (layout == null || layoutW <= 0 || layoutH <= 0) {
            return;
        }
        int w = layoutW, h = layoutH;
        try {
            JSONArray list = new JSONArray();
            for (int i = 0; i < ids.length; i++) {
                int base = Native.HEADER + Native.STRIDE * i;
                boolean circle = layout[base] == 1f;
                JSONObject o = new JSONObject().put("id", ids[i]).put("shape", circle ? "circle" : "rect")
                    .put("x", layout[base + 1]).put("y", layout[base + 2]);
                if (circle) {
                    o.put("r", layout[base + 3]);
                } else {
                    o.put("w", layout[base + 4]).put("h", layout[base + 5]);
                }
                list.put(o);
            }
            JSONObject mine = new JSONObject().put("width", w).put("height", h).put("elements", list);
            JSONObject menu = new JSONObject().put("fx", menuX / w).put("fy", menuY / h);
            prefs.edit().putString(KEY_LAYOUT, mine.toString()).putString("menu", menu.toString()).apply();
        } catch (JSONException ignored) {
        }
        Native.setLayout(w, h, insetLeft, insetTop, insetRight, insetBottom, layout);
    }

    void resetLayout() {
        prefs.edit().remove(KEY_LAYOUT).remove("menu").apply();
        selected = -1;
        menuSelected = false;
        listener.onSelectionChanged(null);
        relayout();
    }

    float globalOpacity() {
        return globalOpacity;
    }

    void setGlobalOpacity(float opacity) {
        globalOpacity = Math.max(0.05f, Math.min(1f, opacity));
        prefs.edit().putFloat(KEY_OPACITY, globalOpacity).apply();
        invalidate();
    }

    boolean hidden() {
        return hidden;
    }

    void setHidden(boolean hide) {
        hidden = hide;
        prefs.edit().putBoolean(KEY_HIDDEN, hide).apply();
        invalidate();
    }

    void setEditing(boolean edit) {
        editing = edit;
        if (!edit) {
            selected = -1;
            menuSelected = false;
            save();
        }
        onMenu.clear();
        invalidate();
    }

    // ---- The picked control's size (edit mode) -------------------------------------------------

    /** The picked control's size against the default's: 1 is the default. */
    float selectedScale() {
        if (selected < 0) {
            return 1f;
        }
        int base = Native.HEADER + Native.STRIDE * selected;
        boolean circle = layout[base] == 1f;
        return circle ? layout[base + 3] / defaults[base + 3] : layout[base + 4] / defaults[base + 4];
    }

    void setSelectedScale(float scale) {
        if (selected < 0) {
            return;
        }
        int base = Native.HEADER + Native.STRIDE * selected;
        if (layout[base] == 1f) {
            layout[base + 3] = defaults[base + 3] * scale;
        } else {
            float cx = layout[base + 1] + layout[base + 4] / 2, cy = layout[base + 2] + layout[base + 5] / 2;
            layout[base + 4] = defaults[base + 4] * scale;
            layout[base + 5] = defaults[base + 5] * scale;
            layout[base + 1] = cx - layout[base + 4] / 2;
            layout[base + 2] = cy - layout[base + 5] / 2;
        }
        invalidate();
    }

    // ---- Touch --------------------------------------------------------------------------------

    private boolean inside(int i, float x, float y, float slack) {
        int base = Native.HEADER + Native.STRIDE * i;
        if (layout[base] == 1f) {
            return Math.hypot(x - layout[base + 1], y - layout[base + 2]) <= layout[base + 3] * slack;
        }
        float w = layout[base + 4], h = layout[base + 5];
        float sx = w * (slack - 1) / 2, sy = h * (slack - 1) / 2;
        return x >= layout[base + 1] - sx && x <= layout[base + 1] + w + sx && y >= layout[base + 2] - sy
            && y <= layout[base + 2] + h + sy;
    }

    private boolean onMenuButton(float x, float y, float slack) {
        return Math.hypot(x - menuX, y - menuY) <= menuR * slack;
    }

    private boolean padShown() {
        return !hidden && active;
    }

    @SuppressLint("ClickableViewAccessibility")
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (layout == null) {
            return false;
        }
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        int pointer = event.getPointerId(index);
        float x = event.getX(index);
        float y = event.getY(index);
        long time = event.getEventTime();
        if (editing) {
            return editTouch(event, action, x, y);
        }
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                if (onMenuButton(x, y, 1.3f)) {
                    onMenu.put(pointer, true);
                } else if (padShown()) {
                    Native.touch(0, pointer, x, y, time);
                } else if (action == MotionEvent.ACTION_DOWN) {
                    return false;
                }
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    int id = event.getPointerId(i);
                    if (!onMenu.containsKey(id) && padShown()) {
                        Native.touch(1, id, event.getX(i), event.getY(i), time);
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                if (onMenu.remove(pointer) != null) {
                    if (action != MotionEvent.ACTION_CANCEL && onMenuButton(x, y, 1.6f)) {
                        listener.onMenu();
                    }
                } else {
                    Native.touch(2, pointer, x, y, time);
                }
                if (action == MotionEvent.ACTION_CANCEL) {
                    for (int i = 0; i < event.getPointerCount(); i++) {
                        Native.touch(2, event.getPointerId(i), 0, 0, time);
                    }
                    onMenu.clear();
                }
                break;
            default:
                break;
        }
        refreshState();
        return true;
    }

    private boolean editTouch(MotionEvent event, int action, float x, float y) {
        switch (action) {
            case MotionEvent.ACTION_DOWN: {
                dragPointer = event.getPointerId(0);
                selected = -1;
                menuSelected = onMenuButton(x, y, 1.3f);
                for (float slack : new float[] {1f, 1.3f}) {
                    for (int i = 0; !menuSelected && selected < 0 && i < ids.length; i++) {
                        if (inside(i, x, y, slack)) {
                            selected = i;
                        }
                    }
                }
                dragX = x;
                dragY = y;
                listener.onSelectionChanged(menuSelected ? "menu" : selected >= 0 ? ids[selected] : null);
                if (menuSelected || selected >= 0) {
                    listener.onEditDrag(true, y);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                // The finger that picked the control up, wherever it now is in the pointer list.
                int at = event.findPointerIndex(dragPointer);
                if (at < 0) {
                    break;
                }
                float nx = event.getX(at), ny = event.getY(at);
                if (menuSelected) {
                    menuX = Math.max(menuR, Math.min(getWidth() - menuR, menuX + nx - dragX));
                    menuY = Math.max(menuR, Math.min(getHeight() - menuR, menuY + ny - dragY));
                } else if (selected >= 0) {
                    int base = Native.HEADER + Native.STRIDE * selected;
                    layout[base + 1] += nx - dragX;
                    layout[base + 2] += ny - dragY;
                    clampToWindow(selected, getWidth(), getHeight());
                }
                dragX = nx;
                dragY = ny;
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                if (menuSelected || selected >= 0) {
                    listener.onEditDrag(false, y);
                }
                break;
            default:
                break;
        }
        invalidate();
        return true;
    }

    void refreshState() {
        try {
            buttons = Native.padButtons();
            sticks = Native.padSticks();
            active = Native.touchActive();
        } catch (UnsatisfiedLinkError e) {
            buttons = 0;
            sticks = new float[0];
        }
        invalidate();
    }

    // ---- Drawing ------------------------------------------------------------------------------

    private int alpha(float part) {
        return Math.round(255 * part * globalOpacity);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (layout == null) {
            return;
        }
        if (editing || padShown()) {
            drawPad(canvas);
        }
        drawMenuButton(canvas);
    }

    private void drawPad(Canvas canvas) {
        for (int i = 0; i < ids.length; i++) {
            int base = Native.HEADER + Native.STRIDE * i;
            String id = ids[i];
            Integer bit = BITS.get(id);
            boolean down = bit != null && (buttons & bit) != 0;
            Integer color = COLORS.get(id);
            if (layout[base] == 0f) {
                RectF r = new RectF(layout[base + 1], layout[base + 2], layout[base + 1] + layout[base + 4],
                    layout[base + 2] + layout[base + 5]);
                float corner = r.height() * 0.3f;
                fill.setColor(Color.WHITE);
                fill.setAlpha(alpha(down ? 0.45f : 0.16f));
                canvas.drawRoundRect(r, corner, corner, fill);
                edge.setColor(Color.WHITE);
                edge.setAlpha(alpha(0.5f));
                canvas.drawRoundRect(r, corner, corner, edge);
                label(canvas, id, r.centerX(), r.centerY(), r.height() * 0.42f, Color.WHITE);
            } else {
                float x = layout[base + 1], y = layout[base + 2], radius = layout[base + 3];
                fill.setColor(color != null ? color : Color.WHITE);
                fill.setAlpha(alpha(down ? 0.55f : 0.16f));
                canvas.drawCircle(x, y, radius, fill);
                edge.setColor(color != null ? color : Color.WHITE);
                edge.setAlpha(alpha(0.7f));
                canvas.drawCircle(x, y, radius, edge);
                if (id.equals("dpad")) {
                    drawDpad(canvas, x, y, radius);
                } else {
                    label(canvas, id, x, y, radius * (id.equals("fire") ? 0.42f : 0.75f),
                        color != null ? color : Color.WHITE);
                }
            }
            if (editing && i == selected) {
                drawSelection(canvas, base);
            }
        }
        float stickR = layout[0];
        boolean leftHeld = false;
        for (int s = 0; s + 4 < sticks.length; s += 5) {
            leftHeld |= sticks[s] == 1f;
            drawStick(canvas, sticks[s + 1], sticks[s + 2], sticks[s + 3], sticks[s + 4], stickR, 1f);
        }
        if (!leftHeld) {
            drawStick(canvas, layout[1], layout[2], layout[1], layout[2], stickR, 0.5f);
        }
    }

    private void label(Canvas canvas, String id, float x, float y, float size, int color) {
        text.setColor(color);
        text.setAlpha(alpha(0.95f));
        text.setTextSize(size);
        canvas.drawText(LABELS.getOrDefault(id, id), x, y + size * 0.36f, text);
    }

    private void drawStick(Canvas canvas, float x, float y, float kx, float ky, float r, float strength) {
        edge.setColor(Color.WHITE);
        edge.setAlpha(alpha(0.45f * strength));
        canvas.drawCircle(x, y, r, edge);
        fill.setColor(Color.WHITE);
        fill.setAlpha(alpha(0.12f * strength));
        canvas.drawCircle(x, y, r, fill);
        fill.setAlpha(alpha(0.35f * strength));
        canvas.drawCircle(kx, ky, r * 0.45f, fill);
    }

    private void drawDpad(Canvas canvas, float x, float y, float r) {
        float arm = r * 0.62f, half = r * 0.2f;
        int[][] arms = {{UP, 0, -1}, {DOWN, 0, 1}, {LEFT, -1, 0}, {RIGHT, 1, 0}};
        for (int[] a : arms) {
            boolean down = (buttons & (1 << a[0])) != 0;
            fill.setColor(Color.WHITE);
            fill.setAlpha(alpha(down ? 0.8f : 0.4f));
            float dx = a[1], dy = a[2];
            float left = x + Math.min(dx * half, dx * arm) - (dx == 0 ? half : 0);
            float right = x + Math.max(dx * half, dx * arm) + (dx == 0 ? half : 0);
            float top = y + Math.min(dy * half, dy * arm) - (dy == 0 ? half : 0);
            float bottom = y + Math.max(dy * half, dy * arm) + (dy == 0 ? half : 0);
            canvas.drawRoundRect(new RectF(left, top, right, bottom), half * 0.4f, half * 0.4f, fill);
        }
    }

    private void drawMenuButton(Canvas canvas) {
        fill.setColor(Color.BLACK);
        fill.setAlpha(Math.round(255 * 0.35f));
        canvas.drawCircle(menuX, menuY, menuR, fill);
        edge.setColor(Color.WHITE);
        edge.setAlpha(Math.round(255 * 0.6f));
        canvas.drawCircle(menuX, menuY, menuR, edge);
        text.setColor(Color.WHITE);
        text.setAlpha(240);
        text.setTextSize(menuR * 1.1f);
        canvas.drawText(LABELS.get("menu"), menuX, menuY + menuR * 0.38f, text);
        if (editing && menuSelected) {
            Paint p = selectionPaint();
            canvas.drawCircle(menuX, menuY, menuR * 1.1f, p);
        }
    }

    private Paint selectionPaint() {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(edge.getStrokeWidth() * 1.5f);
        p.setColor(Color.rgb(0xFF, 0xC8, 0x40));
        return p;
    }

    private void drawSelection(Canvas canvas, int base) {
        Paint p = selectionPaint();
        if (layout[base] == 0f) {
            canvas.drawRect(layout[base + 1], layout[base + 2], layout[base + 1] + layout[base + 4],
                layout[base + 2] + layout[base + 5], p);
        } else {
            canvas.drawCircle(layout[base + 1], layout[base + 2], layout[base + 3] * 1.08f, p);
        }
    }
}
