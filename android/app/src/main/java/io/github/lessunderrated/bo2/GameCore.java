package io.github.lessunderrated.bo2;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The game core: the runtime and the game recompiled from the player's own copy (libbo2core.so), built on their PC by
 * scripts/package-game.sh. The APK carries none of it. The player puts it in the game folder (with the import, or the
 * PC's updater does); it is copied from there into the app's own storage, where code may be loaded from (shared storage
 * is mounted without execute), made read-only as Android requires of loaded code, and its path handed to the native
 * side as BO2_CORE (core_loader.cpp opens it when the game starts).
 */
final class GameCore {
    static final String FILE = "libbo2core.so";
    private static final String KEY_SOURCE = "core_source";   // size:lastModified of the copied file

    private GameCore() {
    }

    /** Where the player's core arrives: the game folder (shared storage). */
    static File incoming(Context context) {
        File external = context.getExternalFilesDir(null);
        return new File(new File(external != null ? external : context.getFilesDir(), "game"), FILE);
    }

    /** The copy the app loads: its own storage. */
    static File installed(Context context) {
        return new File(new File(context.getFilesDir(), "core"), FILE);
    }

    static boolean present(Context context) {
        return installed(context).isFile() || incoming(context).isFile();
    }

    /**
     * Copies a new or changed core from the game folder into the app's storage (only when its size or time changed),
     * and returns the path to load, or null without one. Runs before the native side starts (Bo2App).
     */
    static String prepare(Context context) {
        File in = incoming(context);
        File out = installed(context);
        SharedPreferences prefs = TouchOverlay.prefs(context);
        if (in.isFile()) {
            String source = in.length() + ":" + in.lastModified();
            if (!out.isFile() || !source.equals(prefs.getString(KEY_SOURCE, ""))) {
                try {
                    copy(in, out);
                    prefs.edit().putString(KEY_SOURCE, source).apply();
                    Log.i("BO2", "[CORE] copied " + in + " (" + in.length() + " bytes) to " + out);
                } catch (IOException e) {
                    Log.e("BO2", "[CORE] could not copy the game core: " + e);
                }
            }
        }
        return out.isFile() ? out.getAbsolutePath() : null;
    }

    /**
     * A core downloaded from the PC (Updater), already unpacked and checked, into place; BO2_CORE points at it for the
     * game started from here. A core in the game folder is then only taken again once it changes.
     */
    static void install(Context context, File unpacked) throws IOException {
        File out = installed(context);
        out.setWritable(true);
        if (out.exists() && !out.delete()) {
            throw new IOException("cannot replace " + out);
        }
        if (!unpacked.renameTo(out)) {
            throw new IOException("cannot move " + unpacked + " to " + out);
        }
        out.setReadOnly();
        File in = incoming(context);
        TouchOverlay.prefs(context).edit()
            .putString(KEY_SOURCE, in.isFile() ? in.length() + ":" + in.lastModified() : "pc").apply();
        try {
            android.system.Os.setenv("BO2_CORE", out.getAbsolutePath(), true);
        } catch (android.system.ErrnoException e) {
            Log.w("BO2", "[CORE] setenv BO2_CORE: " + e);
        }
    }

    private static void copy(File from, File to) throws IOException {
        File dir = to.getParentFile();
        if (dir != null) {
            dir.mkdirs();
        }
        File part = new File(to.getPath() + ".part");
        part.delete();
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(part)) {
            byte[] buf = new byte[1 << 20];
            for (int n; (n = in.read(buf)) > 0; ) {
                out.write(buf, 0, n);
            }
        }
        to.setWritable(true);
        if (to.exists() && !to.delete()) {
            throw new IOException("cannot replace " + to);
        }
        if (!part.renameTo(to)) {
            throw new IOException("cannot rename " + part);
        }
        // Android 14+ refuses to load code from a writable file (dynamic code loading).
        to.setReadOnly();
    }
}
