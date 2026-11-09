package io.github.lessunderrated.bo2;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;
import android.util.Log;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.DateFormat;
import java.util.Date;
import java.util.List;

/**
 * The app's log, for a player with no computer to read logcat from. Every run writes logs/
 * (android/native/src/log.cpp, which also writes down where a crash happened); when the last run ended
 * badly, the setup screen says so and offers its log for sharing, and the menu offers this run's.
 */
final class CrashReport {
    private static final String TAG = "BO2";
    /** What a share or the clipboard carries comfortably: the end of the log, where the trouble is. */
    private static final int MAX_CHARS = 60_000;

    private CrashReport() {}

    private static File logDir(Context context) {
        return new File(context.getFilesDir(), "logs");
    }

    private static boolean sOpened;

    /** Starts this run's log (the native side keeps the last run's) and records Java exceptions in it. */
    static synchronized void open(Context context) {
        if (sOpened) {
            return;
        }
        try {
            SetupActivity.loadNative();
            Native.openLog(logDir(context).getPath());
            Native.log(device(context));
            sOpened = true;
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "no native log", e);
            return;
        }
        Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            try {
                Native.log("java exception in thread " + thread.getName() + ":\n" + Log.getStackTraceString(error));
            } catch (Throwable ignored) {
            }
            if (previous != null) {
                previous.uncaughtException(thread, error);
            }
        });
    }

    private static String device(Context context) {
        String version = "?";
        try {
            version = context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName;
        } catch (Exception ignored) {
        }
        return "BO2-APK " + version + " on " + Build.MANUFACTURER + " " + Build.MODEL
            + " (" + Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL + "), Android " + Build.VERSION.RELEASE
            + " (API " + Build.VERSION.SDK_INT + ")";
    }

    // ---- The last run -------------------------------------------------------------------------

    /**
     * The report on the last run when it crashed, was stopped for not responding, or quit with an
     * error, and has not been reported yet; null otherwise. Call after open, which keeps its log.
     */
    static String lastRunFailure(Context context) {
        List<ApplicationExitInfo> exits;
        try {
            exits = context.getSystemService(ActivityManager.class)
                .getHistoricalProcessExitReasons(null, 0, 1);
        } catch (RuntimeException e) {
            return null;
        }
        if (exits.isEmpty()) {
            return null;
        }
        ApplicationExitInfo exit = exits.get(0);
        SharedPreferences prefs = context.getSharedPreferences("crash-report", Context.MODE_PRIVATE);
        if (exit.getTimestamp() <= prefs.getLong("reported", 0)) {
            return null;
        }
        prefs.edit().putLong("reported", exit.getTimestamp()).apply();
        if (!failed(exit)) {
            return null;
        }
        StringBuilder report = new StringBuilder();
        report.append(device(context)).append('\n');
        report.append("The last run ended ")
            .append(DateFormat.getDateTimeInstance().format(new Date(exit.getTimestamp())))
            .append(": ").append(reason(exit)).append(", status ").append(exit.getStatus());
        if (exit.getDescription() != null) {
            report.append(", ").append(exit.getDescription());
        }
        report.append('\n');
        if (exit.getReason() == ApplicationExitInfo.REASON_ANR) {
            report.append("\n--- threads ---\n").append(tail(trace(exit), MAX_CHARS / 3));
        }
        report.append("\n--- log ---\n");
        report.append(tail(read(new File(logDir(context), "bo2-previous.log")), MAX_CHARS));
        return report.toString();
    }

    private static boolean failed(ApplicationExitInfo exit) {
        switch (exit.getReason()) {
            case ApplicationExitInfo.REASON_CRASH:
            case ApplicationExitInfo.REASON_CRASH_NATIVE:
            case ApplicationExitInfo.REASON_ANR:
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE:
                return true;
            case ApplicationExitInfo.REASON_EXIT_SELF:
                return exit.getStatus() != 0;
            case ApplicationExitInfo.REASON_SIGNALED:
            case ApplicationExitInfo.REASON_LOW_MEMORY:
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE:
                // Killed while on screen, not cleared away in the background.
                return exit.getImportance() <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_VISIBLE;
            default:
                return false;
        }
    }

    private static String reason(ApplicationExitInfo exit) {
        switch (exit.getReason()) {
            case ApplicationExitInfo.REASON_CRASH:
                return "crashed (Java)";
            case ApplicationExitInfo.REASON_CRASH_NATIVE:
                return "crashed (native)";
            case ApplicationExitInfo.REASON_ANR:
                return "stopped responding";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE:
                return "failed to start";
            case ApplicationExitInfo.REASON_EXIT_SELF:
                return "quit with an error";
            case ApplicationExitInfo.REASON_SIGNALED:
                return "killed by a signal";
            case ApplicationExitInfo.REASON_LOW_MEMORY:
                return "closed by Android for memory";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE:
                return "closed by Android for using too much";
            default:
                return "reason " + exit.getReason();
        }
    }

    private static String trace(ApplicationExitInfo exit) {
        try (InputStream in = exit.getTraceInputStream()) {
            if (in == null) {
                return "";
            }
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            for (int n; (n = in.read(buffer)) > 0 && out.size() < (1 << 20); ) {
                out.write(buffer, 0, n);
            }
            return out.toString(StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    // ---- This run -----------------------------------------------------------------------------

    static String currentRun(Context context) {
        return device(context) + "\n\n--- log ---\n" + tail(read(new File(logDir(context), "bo2.log")), MAX_CHARS);
    }

    /** The last bytes of a file, which is all of a log worth sending. */
    private static String read(File file) {
        try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
            long length = in.length();
            int size = (int) Math.min(length, MAX_CHARS * 2L);
            byte[] bytes = new byte[size];
            in.seek(length - size);
            in.readFully(bytes);
            return new String(bytes, StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "(no log: " + e.getMessage() + ")";
        }
    }

    private static String tail(String text, int chars) {
        if (text.length() <= chars) {
            return text;
        }
        String end = text.substring(text.length() - chars);
        int line = end.indexOf('\n');
        return "[…]\n" + (line >= 0 ? end.substring(line + 1) : end);
    }

    // ---- Dialogs ------------------------------------------------------------------------------

    /** Says the last run failed and offers its report; then runs next. */
    static void showFailure(Activity activity, String report, Runnable next) {
        new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(R.string.crash_title)
            .setMessage(R.string.crash_message)
            .setCancelable(false)
            .setPositiveButton(R.string.log_share, (d, w) -> {
                share(activity, report);
                next.run();
            })
            .setNeutralButton(R.string.log_copy, (d, w) -> {
                copy(activity, report);
                next.run();
            })
            .setNegativeButton(R.string.crash_continue, (d, w) -> next.run())
            .show();
    }

    static void share(Activity activity, String report) {
        Intent send = new Intent(Intent.ACTION_SEND)
            .setType("text/plain")
            .putExtra(Intent.EXTRA_SUBJECT, activity.getString(R.string.log_subject))
            .putExtra(Intent.EXTRA_TEXT, report);
        try {
            activity.startActivity(Intent.createChooser(send, activity.getString(R.string.log_share)));
        } catch (RuntimeException e) {
            copy(activity, report);
        }
    }

    static void copy(Context context, String report) {
        context.getSystemService(ClipboardManager.class)
            .setPrimaryClip(ClipData.newPlainText(context.getString(R.string.log_subject), report));
        Toast.makeText(context, R.string.log_copied, Toast.LENGTH_SHORT).show();
    }
}
