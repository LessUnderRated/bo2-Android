package io.github.lessunderrated.bo2;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.text.format.Formatter;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Space;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The launcher: the game data's state, the graphics and control options (the in-game ☰ menu's, saved in the same
 * place), and Play. What this phone is (the Galaxy S26 comes as a Snapdragon/Adreno and an Exynos/Xclipse model,
 * which matters for the GPU driver) goes to the log, not the screen. It also imports the game's data: the player
 * copies their own Steam install of Black Ops II to the phone and picks that folder here (Storage Access
 * Framework, so no storage permission), and its files are copied into the app's external files directory,
 * game/. build-android.sh can push the same folder over USB instead (adb push is much faster than SAF).
 * The flow follows LessUnderRated's Chronicle-rs SetupActivity (ask once, copy with progress, then start).
 */
public class SetupActivity extends Activity {
    private static final int PICK_FOLDER = 0xB02;
    /** What makes a folder a Black Ops II install: its fast files, and at least one of the three games. */
    private static final String[] REQUIRED = {"zone"};
    private static final String[] GAMES = {"t6zm.exe", "t6mp.exe", "t6sp.exe"};
    /** Steam's redistributables are not the game's data. */
    private static final String[] SKIPPED = {"redist", "__CommonRedist", "_CommonRedist"};
    /** Written last: its presence means the import finished. */
    private static final String DONE_MARK = ".bo2-import-complete";

    private static boolean sNativeLoaded;

    private TextView status;
    private ProgressBar progress;
    private Button pick;
    private Button start;
    /** One import at a time per process, whichever SetupActivity started it. */
    private static final AtomicBoolean COPYING = new AtomicBoolean();
    /** The game's activity is alive: importing would rewrite files it has open. Set by Bo2Activity. */
    static volatile boolean gameRunning;

    /** libmain.so, for the log and the device report, before SDLActivity loads it for the game. */
    static synchronized void loadNative() {
        if (!sNativeLoaded) {
            System.loadLibrary("SDL3");
            System.loadLibrary("main");
            sNativeLoaded = true;
        }
    }

    /** Native.log when libmain.so loaded; Logcat only otherwise (a failed load must not crash setup). */
    static void log(String text) {
        try {
            Native.log(text);
        } catch (UnsatisfiedLinkError e) {
            android.util.Log.i("BO2", text);
        }
    }

    static File gameDir(Activity activity) {
        return new File(activity.getExternalFilesDir(null), "game");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        CrashReport.open(this);
        buildViews();
        log(deviceLines());
        String failure = CrashReport.lastRunFailure(this);
        if (failure != null) {
            CrashReport.showFailure(this, failure, this::refresh);
        } else {
            refresh();
        }
        // A newer build on the PC (bo2-logd over Tailscale): offered here, before the game starts.
        Updater.check(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Updater.onResume(this);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    // ---- The launcher: game data, then the options, then Play (laid out like the Skyrim port's launcher) ----------

    private static final int BG = Color.rgb(0x16, 0x15, 0x14);
    private static final int FIELD = Color.rgb(0x29, 0x27, 0x23);
    private static final int TEXT = Color.rgb(0xE8, 0xE4, 0xDA);
    private static final int DIM = Color.rgb(0xA6, 0xA1, 0x96);
    private static final int ACCENT = Color.rgb(0xFF, 0x6A, 0x00);
    private static final int GOOD = Color.rgb(0x8C, 0xC8, 0x78);
    private static final int WARN = Color.rgb(0xE6, 0x96, 0x6E);

    /** The graphics knobs (Bo2Activity.graphics, the in-game ☰ → Graphics menu's); null without libmain.so. */
    private int[] knobs;
    /** Each option row's "show the saved value again", for a preset that changes several rows at once. */
    private final List<Runnable> rows = new ArrayList<>();

    private interface Getter {
        int get();
    }

    private interface Setter {
        void set(int value);
    }

    private interface Namer {
        String name(int value);
    }

    private void buildViews() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        // A readable column on a wide landscape screen: at most 720 dp, centred.
        int width = getResources().getDisplayMetrics().widthPixels;
        int side = Math.max(dp(20), (width - dp(720)) / 2);
        column.setPadding(side, dp(20), side, dp(24));

        TextView title = new TextView(this);
        title.setText(R.string.setup_title);
        title.setTextColor(ACCENT);
        title.setTextSize(26);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        column.addView(title);
        TextView version = new TextView(this);
        version.setText(versionLine());
        version.setTextColor(DIM);
        version.setTextSize(13);
        column.addView(version);

        heading(column, R.string.launcher_data);
        status = new TextView(this);
        status.setTextSize(14);
        status.setPadding(0, 0, 0, dp(8));
        column.addView(status);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setVisibility(ProgressBar.GONE);
        progress.setProgressTintList(ColorStateList.valueOf(ACCENT));
        column.addView(progress);
        pick = button(column, false);
        pick.setOnClickListener(v -> pickFolder());

        SharedPreferences prefs = TouchOverlay.prefs(this);
        try {
            knobs = Bo2Activity.graphics(this);
        } catch (UnsatisfiedLinkError | RuntimeException e) {
            log("launcher: no graphics options (" + e + ")");
            knobs = null;
        }

        heading(column, R.string.launcher_graphics);
        if (knobs != null) {
            option(column, getString(R.string.launcher_preset), new int[] {0, 1, 2, 3, 4},
                v -> Bo2Activity.presetName(this, v), () -> knobs[Bo2Activity.K_PRESET], v -> {
                    if (v == 4) {
                        knobs[Bo2Activity.K_PRESET] = 4;  // custom: the knobs stay as they are
                    } else {
                        knobs = Native.graphicsPreset(v, knobs);
                    }
                    saveKnobs();
                });
        }
        option(column, getString(R.string.launcher_resolution), Bo2Activity.HEIGHTS,
            v -> upper(Bo2Activity.heightName(this, v)),
            () -> prefs.getInt(Bo2Activity.KEY_HEIGHT, Bo2Activity.DEFAULT_HEIGHT),
            v -> { prefs.edit().putInt(Bo2Activity.KEY_HEIGHT, v).apply(); Bo2Activity.applyDisplay(this); });
        if (knobs != null) {
            option(column, getString(R.string.launcher_upscaler), Bo2Activity.UPSCALERS,
                v -> Bo2Activity.upscalerName(this, v), () -> knobs[Bo2Activity.K_UPSCALER],
                v -> { knobs[Bo2Activity.K_UPSCALER] = v; saveKnobs(); });
            note(column, R.string.launcher_power_note);
        }
        option(column, getString(R.string.launcher_screen), new int[] {0, 1},
            v -> upper(getString(v == 0 ? R.string.display_fill : R.string.display_16_9)),
            () -> prefs.getInt(Bo2Activity.KEY_ASPECT, 0),
            v -> { prefs.edit().putInt(Bo2Activity.KEY_ASPECT, v).apply(); Bo2Activity.applyDisplay(this); });
        if (knobs != null) {
            option(column, getString(R.string.launcher_cap), Bo2Activity.CAPS, v -> upper(Bo2Activity.capName(this, v)),
                () -> knobs[Bo2Activity.K_CAP], v -> { knobs[Bo2Activity.K_CAP] = v; saveKnobs(); });
            option(column, getString(R.string.launcher_view), Bo2Activity.VIEWS, v -> Bo2Activity.viewName(this, v),
                () -> knobs[Bo2Activity.K_VIEW], v -> {
                    knobs[Bo2Activity.K_VIEW] = v;
                    knobs[Bo2Activity.K_PRESET] = 4;   // a cost knob: the preset becomes Custom
                    saveKnobs();
                });
            note(column, R.string.launcher_view_note);
            option(column, getString(R.string.launcher_fov), Bo2Activity.FOVS,
                v -> v < 0 ? upper(getString(R.string.gfx_game)) : v + "°", () -> knobs[Bo2Activity.K_FOV],
                v -> { knobs[Bo2Activity.K_FOV] = v; saveKnobs(); });
            // Each of the preset's knobs on its own; changing one makes the preset Custom, as in ☰ → Graphics.
            for (int knob = 2; knob < Bo2Activity.STEPS.length; knob++) {
                final int which = knob;
                option(column, getString(Bo2Activity.KNOB_NAMES[knob]), Bo2Activity.STEPS[knob],
                    v -> upper(Bo2Activity.knobValue(this, which, v)), () -> knobs[which], v -> {
                        knobs[which] = v;
                        knobs[Bo2Activity.K_PRESET] = 4;
                        saveKnobs();
                    });
            }
        }

        heading(column, R.string.launcher_controls);
        check(column, R.string.launcher_touch, !prefs.getBoolean(TouchOverlay.KEY_HIDDEN, false),
            on -> prefs.edit().putBoolean(TouchOverlay.KEY_HIDDEN, !on).apply());
        option(column, getString(R.string.launcher_look), new int[] {0, 1},
            v -> upper(getString(v == 0 ? R.string.menu_look_swipe : R.string.menu_look_stick)),
            () -> prefs.getInt(TouchOverlay.KEY_LOOK, 0), v -> prefs.edit().putInt(TouchOverlay.KEY_LOOK, v).apply());
        check(column, R.string.launcher_auto_sprint, prefs.getBoolean(TouchOverlay.KEY_AUTO_SPRINT, true),
            on -> prefs.edit().putBoolean(TouchOverlay.KEY_AUTO_SPRINT, on).apply());

        heading(column, R.string.launcher_diagnostics);
        String[] logging = getResources().getStringArray(R.array.launcher_logging_values);
        option(column, getString(R.string.launcher_logging), new int[] {0, 1, 2}, v -> logging[v],
            () -> prefs.getInt(Bo2App.KEY_DIAG, 0), v -> {
                prefs.edit().putInt(Bo2App.KEY_DIAG, v).apply();
                try {
                    Native.setDiagnostics(v);   // live: the log, the PC sender and the profiler follow it now
                } catch (UnsatisfiedLinkError ignored) {
                }
            });
        note(column, R.string.launcher_logging_note);

        Space gap = new Space(this);
        column.addView(gap, new LinearLayout.LayoutParams(1, dp(20)));
        start = button(column, true);
        start.setText(R.string.setup_start);
        start.setOnClickListener(v -> startGame());

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG);
        scroll.setFillViewport(true);
        scroll.addView(column);
        setContentView(scroll);
    }

    /** "Version 0.1.51 · 6d237fa · Nov 8, 2026, 2:53 PM": which build the updater installed. */
    private String versionLine() {
        StringBuilder v = new StringBuilder(getString(R.string.launcher_version, BuildConfig.VERSION_NAME));
        if (!BuildConfig.GIT_COMMIT.isEmpty()) {
            v.append(" · ").append(BuildConfig.GIT_COMMIT);
        }
        if (BuildConfig.BUILD_TIME > 0) {
            v.append(" · ").append(java.text.DateFormat.getDateTimeInstance(java.text.DateFormat.MEDIUM,
                java.text.DateFormat.SHORT).format(new java.util.Date(BuildConfig.BUILD_TIME * 1000L)));
        }
        return v.toString();
    }

    /** Saved where the game reads them (Bo2Activity.applyGraphics), and sent now if it is running. */
    private void saveKnobs() {
        try {
            Bo2Activity.saveGraphics(this, knobs);
        } catch (UnsatisfiedLinkError e) {
            log("launcher: graphics not sent (" + e + ")");
        }
        for (Runnable row : rows) {
            row.run();
        }
    }

    private static String upper(String s) {
        return s.isEmpty() ? s : Character.toUpperCase(s.charAt(0)) + s.substring(1);
    }

    private void heading(LinearLayout column, int text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(TEXT);
        t.setTextSize(20);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(22), 0, dp(8));
        column.addView(t);
    }

    private void note(LinearLayout column, int text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(DIM);
        t.setTextSize(12);
        t.setPadding(dp(2), 0, 0, dp(8));
        column.addView(t);
    }

    private Button button(LinearLayout column, boolean primary) {
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setTextSize(16);
        b.setTextColor(primary ? Color.BLACK : TEXT);
        if (primary) {
            b.setTypeface(Typeface.DEFAULT_BOLD);
        }
        b.setBackgroundColor(primary ? ACCENT : FIELD);
        b.setStateListAnimator(null);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(52));
        lp.topMargin = dp(6);
        column.addView(b, lp);
        return b;
    }

    /** A label on the left, its values in a flat field on the right. */
    private void option(LinearLayout column, String label, int[] values, Namer namer, Getter get, Setter set) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView name = new TextView(this);
        name.setText(label);
        name.setTextColor(DIM);
        name.setTextSize(15);
        row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));

        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) {
            names[i] = namer.name(values[i]);
        }
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, names) {
            @Override
            public View getView(int position, View convert, ViewGroup parent) {
                return field((TextView) super.getView(position, convert, parent), false);
            }

            @Override
            public View getDropDownView(int position, View convert, ViewGroup parent) {
                return field((TextView) super.getDropDownView(position, convert, parent), true);
            }
        };
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        Spinner spinner = new Spinner(this, Spinner.MODE_DROPDOWN);
        spinner.setAdapter(adapter);
        spinner.setBackgroundColor(FIELD);
        spinner.setPopupBackgroundDrawable(new ColorDrawable(FIELD));
        row.addView(spinner, new LinearLayout.LayoutParams(0, dp(48), 1));

        // The position on screen, so the spinner's own callbacks (the first layout, a preset's update) set nothing.
        int[] shown = {0};
        Runnable show = () -> {
            shown[0] = Math.max(0, indexOf(values, get.get()));
            spinner.setSelection(shown[0]);
        };
        show.run();
        spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (position == shown[0]) {
                    return;
                }
                shown[0] = position;
                set.set(values[position]);
                log("launcher: " + label + " = " + names[position]);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        rows.add(show);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(6);
        column.addView(row, lp);
    }

    private TextView field(TextView t, boolean dropDown) {
        t.setTextColor(TEXT);
        t.setTextSize(15);
        t.setPadding(dp(14), dp(dropDown ? 12 : 0), dp(14), dp(dropDown ? 12 : 0));
        t.setBackgroundColor(dropDown ? FIELD : Color.TRANSPARENT);
        return t;
    }

    private static int indexOf(int[] values, int v) {
        for (int i = 0; i < values.length; i++) {
            if (values[i] == v) {
                return i;
            }
        }
        return -1;
    }

    private interface Toggle {
        void set(boolean on);
    }

    private void check(LinearLayout column, int label, boolean on, Toggle toggle) {
        CheckBox c = new CheckBox(this);
        c.setText(label);
        c.setTextColor(TEXT);
        c.setTextSize(15);
        c.setButtonTintList(ColorStateList.valueOf(ACCENT));
        c.setChecked(on);
        c.setPadding(dp(8), dp(6), 0, dp(6));
        c.setOnCheckedChangeListener((v, checked) -> {
            toggle.set(checked);
            log("launcher: " + getString(label) + " = " + checked);
        });
        column.addView(c);
    }

    /** This phone, its GPU and the render size: the log's, no longer on screen. */
    private String deviceLines() {
        StringBuilder out = new StringBuilder();
        out.append(getString(R.string.setup_device)).append(": ").append(Build.MANUFACTURER).append(' ')
            .append(Build.MODEL).append(", ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL)
            .append(", Android ").append(Build.VERSION.RELEASE).append('\n');
        try {
            out.append(Native.gpuReport()).append('\n');
            Bo2Activity.applyDisplay(this);
            android.graphics.Rect bounds = getWindowManager().getMaximumWindowMetrics().getBounds();
            int[] p = Native.displayPlan(bounds.width(), bounds.height());
            out.append(getString(R.string.setup_display, p[0], p[1], p[2], p[3])).append('\n');
        } catch (UnsatisfiedLinkError e) {
            out.append("(no native library: ").append(e.getMessage()).append(")\n");
        }
        return out.toString();
    }

    /** The updater put a game core in place (Updater.downloadCore): show it. */
    void coreChanged() {
        refresh();
    }

    private void refresh() {
        File dir = gameDir(this);
        // Only the mark proves a whole import: files arrive in listing order, so zone/ and the executables
        // are there long before a stopped import is finished.
        boolean imported = new File(dir, DONE_MARK).exists();
        boolean busy = COPYING.get();
        if (busy) {
            status.setTextColor(TEXT);
            status.setText(getString(R.string.setup_copying, "…"));
        } else {
            String text = getString(imported ? R.string.launcher_ready : R.string.launcher_missing);
            if (!GameCore.present(this)) {
                text += "\n" + getString(R.string.setup_core_missing);
            }
            status.setTextColor(imported ? GOOD : WARN);
            status.setText(text);
        }
        log("game data: " + (imported ? "imported (" + dir.getPath() + ")" : "not imported") + (busy ? ", importing" : ""));
        pick.setText(imported ? R.string.setup_reimport : R.string.setup_pick);
        start.setEnabled(!busy && (imported || !GameCore.present(this)));
        start.setAlpha(start.isEnabled() ? 1f : 0.4f);
        pick.setEnabled(!busy && !gameRunning);
        pick.setAlpha(pick.isEnabled() ? 1f : 0.4f);
    }

    private void startGame() {
        startActivity(new Intent(this, Bo2Activity.class));
        finish();
    }

    // ---- Import ---------------------------------------------------------------------------------

    private void pickFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(intent, PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_FOLDER) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri tree = data.getData();
        String root;
        List<Entry> top;
        try {
            root = DocumentsContract.getTreeDocumentId(tree);
            top = list(tree, root);
        } catch (RuntimeException e) {  // a provider's SecurityException, a drive pulled out
            log("cannot list " + tree + ": " + e);
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setMessage(getString(R.string.setup_failed, String.valueOf(e.getMessage())))
                .setPositiveButton(android.R.string.ok, null)
                .show();
            return;
        }
        String missing = missingFrom(top);
        if (missing != null) {
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setMessage(getString(R.string.setup_not_bo2, missing))
                .setPositiveButton(android.R.string.ok, null)
                .show();
            return;
        }
        if (gameRunning || !COPYING.compareAndSet(false, true)) {
            return;
        }
        // A finished import picked again is a full copy (a repair or an update); an unfinished one resumes.
        boolean resume = !new File(gameDir(this), DONE_MARK).exists();
        refresh();
        progress.setVisibility(ProgressBar.VISIBLE);
        new Thread(() -> copyAll(tree, root, resume), "bo2-import").start();
    }

    private static String missingFrom(List<Entry> top) {
        for (String name : REQUIRED) {
            if (find(top, name) == null) {
                return name;
            }
        }
        for (String name : GAMES) {
            if (find(top, name) != null) {
                return null;
            }
        }
        return String.join(" / ", GAMES);
    }

    private static Entry find(List<Entry> entries, String name) {
        for (Entry e : entries) {
            if (e.name.equalsIgnoreCase(name)) {
                return e;
            }
        }
        return null;
    }

    private static final class Entry {
        final String id;
        final String name;
        final boolean directory;
        final long size;

        Entry(String id, String name, boolean directory, long size) {
            this.id = id;
            this.name = name;
            this.directory = directory;
            this.size = size;
        }
    }

    private List<Entry> list(Uri tree, String parent) {
        List<Entry> out = new ArrayList<>();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = getContentResolver().query(children, columns, null, null, null)) {
            while (c != null && c.moveToNext()) {
                boolean dir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                out.add(new Entry(c.getString(0), c.getString(1), dir, c.isNull(3) ? 0 : c.getLong(3)));
            }
        }
        return out;
    }

    private void collect(Uri tree, String parent, String path, List<String[]> files, long[] total) {
        for (Entry e : list(tree, parent)) {
            if (e.name.startsWith(".")) {
                continue;
            }
            boolean skip = false;
            for (String s : SKIPPED) {
                skip |= e.directory && e.name.equalsIgnoreCase(s);
            }
            if (skip) {
                continue;
            }
            String child = path.isEmpty() ? e.name : path + "/" + e.name;
            if (e.directory) {
                collect(tree, e.id, child, files, total);
            } else {
                files.add(new String[] {e.id, child, Long.toString(e.size)});
                total[0] += e.size;
            }
        }
    }

    private void copyAll(Uri tree, String root, boolean resume) {
        File dir = gameDir(this);
        new File(dir, DONE_MARK).delete();
        List<String[]> files = new ArrayList<>();
        long[] total = {0};
        String error = null;
        long done = 0;
        try {
            collect(tree, root, "", files, total);
            log("import: " + files.size() + " files, " + total[0] + " bytes from " + tree + (resume ? " (resuming)" : ""));
            ContentResolver resolver = getContentResolver();
            byte[] buffer = new byte[1 << 20];
            long lastShown = 0;
            for (String[] f : files) {
                File target = new File(dir, f[1]);
                long size = Long.parseLong(f[2]);
                File partial = new File(target.getPath() + ".part");
                partial.delete();  // left by a copy that stopped
                // Resuming: a file already there at its full size was copied before the import stopped.
                if (resume && target.isFile() && target.length() == size) {
                    done += size;
                    continue;
                }
                target.getParentFile().mkdirs();
                try (InputStream in = resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, f[0]));
                     OutputStream out = new FileOutputStream(partial)) {
                    for (int n; (n = in.read(buffer)) > 0; ) {
                        out.write(buffer, 0, n);
                        done += n;
                        long now = System.nanoTime();
                        if (now - lastShown > 100_000_000L) {
                            lastShown = now;
                            long shown = done;
                            String name = f[1];
                            runOnUiThread(() -> showProgress(name, shown, total[0]));
                        }
                    }
                }
                if (target.exists() && !target.delete() || !partial.renameTo(target)) {
                    throw new IOException("cannot rename " + partial);
                }
            }
            new FileOutputStream(new File(dir, DONE_MARK)).close();
        } catch (Throwable e) {  // an Error too: the screen must not stay stuck in "copying"
            error = String.valueOf(e.getMessage());
            log("import failed: " + e);
        } finally {
            COPYING.set(false);
        }
        String failure = error;
        int count = files.size();
        long bytes = total[0];
        runOnUiThread(() -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            progress.setVisibility(ProgressBar.GONE);
            refresh();
            String message = failure != null ? getString(R.string.setup_failed, failure)
                                             : getString(R.string.setup_done, count, Formatter.formatFileSize(this, bytes));
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setMessage(message)
                .setPositiveButton(android.R.string.ok, null)
                .show();
        });
    }

    private void showProgress(String name, long done, long total) {
        if (isFinishing() || isDestroyed()) {
            return;
        }
        progress.setMax(1000);
        progress.setProgress(total > 0 ? (int) (done * 1000 / total) : 0);
        status.setTextColor(TEXT);
        status.setText(getString(R.string.setup_copying, name) + "\n" + Formatter.formatFileSize(this, done) + " / "
            + Formatter.formatFileSize(this, total));
    }
}
