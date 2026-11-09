package io.github.lessunderrated.bo2;

import android.app.Application;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import java.io.File;
import java.util.Locale;

/**
 * Runs before any activity, so before libmain.so is loaded. bo2-mac's runtime reads some of its environment in
 * static initializers (the game folder: kernel32_impl.cpp's g_game_dir) and caches every variable on its first
 * lookup (bo2_getenv.h), and the setup screen loads the library for its device report long before Start: set
 * later, those variables would never be seen. game_host.cpp sets the same values again, plus the ones that
 * depend on the window (BO2_GUEST_W/H, read lazily).
 */
public class Bo2App extends Application {
    /** The launcher's Logging option: 0 Full, 1 Normal, 2 Minimal (native log.hpp, BO2_DIAG). */
    static final String KEY_DIAG = "diagnostics";

    @Override
    public void onCreate() {
        super.onCreate();
        File external = getExternalFilesDir(null);
        String game = new File(external != null ? external : getFilesDir(), "game").getAbsolutePath();
        String internal = getFilesDir().getAbsolutePath();
        set("BO2_GAME_DIR", game);
        set("BO2_PE_SECTIONS", game + "/pe_sections.bin");
        set("BO2_SHADER_CACHE_DIR", internal + "/shader_cache");
        set("BO2_REFUSAL_FILE", internal + "/refusal.txt");
        set("HOME", internal);
        String lang = Locale.getDefault().getLanguage();
        if (lang != null && lang.length() >= 2) {
            set("BO2_LANG", lang);
        }
        // The log server this build was made for (remote_log.cpp), and the commit, for the session's hello.
        if (!BuildConfig.LOG_SERVER.isEmpty()) {
            set("BO2_LOGD", BuildConfig.LOG_SERVER);
        }
        set("BO2_BUILD", BuildConfig.GIT_COMMIT.isEmpty() ? BuildConfig.VERSION_NAME : BuildConfig.GIT_COMMIT);
        // The game core (GameCore.java): the player's own, copied into the app's storage; without it, no game.
        String core = GameCore.prepare(this);
        if (core != null) {
            set("BO2_CORE", core);
        }
        // The launcher's Logging option (log.hpp): read by the log, the PC sender and the profiler when they start.
        set("BO2_DIAG", String.valueOf(TouchOverlay.prefs(this).getInt(KEY_DIAG, 0)));
        // The player's settings, as bo2-mac's scripts/play_steam.sh zm sets them for a game (not a measurement run).
        // BO2_WINDOW is the one that draws: without it the D3D11 layer never hands anything to the renderer.
        // BO2_MAX_IMPORTS=0 stops the per-call import log. The two pipes live in the app's files (no /tmp here).
        // BO2_HOST_THREADS: the job workers (jq) stay coroutines on the scheduler thread. On the S26, with them on
        // their own cores, every thread fought over the runtime's single lock for the game's `lock` instructions
        // (the main thread waited on it 11% of its time); a Green Run match had 2.0 frames over 33 ms per 5 s, and
        // 0.9 with them as coroutines, at the same 60 fps and less power.
        String[][] play = {
            {"BO2_WINDOW", "1"}, {"BO2_STALL_ABORT", "0"}, {"BO2_MAX_IMPORTS", "0"}, {"BO2_FORCE_FOCUS", "1"},
            {"BO2_MUTE", "0"}, {"BO2_XAUDIO", "1"}, {"BO2_KB_PHYSICAL", "1"},
            {"BO2_DEPTH_BIAS", "1"}, {"BO2_DEPTH_DSV", "1"}, {"BO2_SV_INLINE", "1"}, {"BO2_NO_FORCE_MAP", "1"},
            {"BO2_MSGB", "150"}, {"BO2_FFOTD_TU", "13"}, {"BO2_VB_RESIDENT", "1"}, {"BO2_VPX_NATIVE", "1"},
            {"BO2_VPX_REPLACE", "1"}, {"BO2_W32_FAST", "1"}, {"BO2_PREEMPT_W", "80000"}, {"BO2_CS_COOP", "1"},
            {"BO2_RR_FAIR", "1"}, {"BO2_REAL_MUTEX", "1"}, {"BO2_HOST_THREADS", "snd,snddec,srv,backend"},
            {"BO2_HW_ATOMICS", "2"}, {"BO2_SHADER_CREE", "1"},
            // The scheduler thread runs the game's main loop, the bottleneck once zombies are about: it gets the
            // display priority (nice -8) the server thread already has. No 20-second stack census either: it
            // walks every host thread's stack and showed as a periodic hitch.
            {"BO2_QOS", "1"}, {"BO2_STACK_WATCH", "0"},
            // The job workers share the scheduler thread's core, so more of them adds no parallelism, only idle
            // polling (each scans every queue, then yields). Telling the game it has 4 CPUs starts fewer: on the
            // S26 the idle-scan functions fell by about a quarter, at the same 60 fps.
            {"BO2_CPUS", "4"},
            // Hosted threads re-check their waits every 0.5 ms (the runtime's original behaviour). The park-until-
            // deadline change (bo2-mac C25, for hosted job workers, which this layout does not host) coincided with the
            // menu music stuttering: the music is streamed and decoded on a hosted sound thread.
            {"BO2_HOST_PARK_US", "500"},
            // The scheduler thread may sleep when every other coroutine only waits (Sleep, Present, an event, a
            // critical section): bo2-mac's level 3, documented as the Zombies default but defaulting to 1 in code. On the
            // S26 the main menu kept a full core busy cycling coroutines (coop_yield / ordo_repos_si_inactif at the top
            // of the in-app profile), heat that throttles the prime cores before a match even starts.
            {"BO2_ORDO_REPOS", "3"},
            // Audio as in r7, which sounded right: the game mixes its own 5.1 and Android folds it to the speakers. The
            // stereo speaker-setup patch and our own fold (bo2-mac, both on by default) made gunshots muffled and wrong
            // on the S26: the game's 2-wide stereo panning feeds a mix bus it keeps at 6 channels, and its own weapon
            // sounds lean on the centre and LFE channels the fold attenuates. The stutter fixes (normal stream mode,
            // 960-frame callbacks, 0.5 ms waits) stay.
            {"BO2_AUDIO_STEREO", "0"}, {"BO2_AUDIO_DOWNMIX", "0"},
            // No `+set snd_speakerConfiguration Stereo` on the command line any more (it saved part of the audio
            // thread's third of a core): without the stereo patch it put the game in its Stereo mode while the mix bus
            // stays 6-wide and Android folds it as 5.1, and gunshots came out of the left ear (e26a468 on the S26).
            // The game's own 5.1, as r7.
            // BO2_PREEMPT_W (above): the scheduler thread gives the core to the next coroutine every N guest writes.
            // At 20000 the main loop was preempted every few tens of microseconds, and each preemption cycled every
            // idle job worker through a scan of all its queues (10% of the core in the S26 profile). The main loop
            // runs pending jobs itself while it waits on them, and still yields whenever it blocks or spins on a
            // lock, so a longer slice (80000) costs the jobs nothing on one core.
            {"BO2_CMDPIPE", internal + "/bo2zms.cmd"}, {"BO2_KEYPIPE", internal + "/bo2zms.key"},
        };
        for (String[] kv : play) {
            set(kv[0], kv[1]);
        }
        // Debugging: game/bo2_env.txt, one NAME=VALUE per line ('#' comments), read last so it can override. It is
        // how bo2-mac's diagnostic switches are turned on from the PC (adb push) without rebuilding the APK.
        File extra = new File(game, "bo2_env.txt");
        if (extra.isFile()) {
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(extra))) {
                for (String line; (line = r.readLine()) != null; ) {
                    line = line.trim();
                    int eq = line.indexOf('=');
                    if (line.isEmpty() || line.startsWith("#") || eq <= 0) {
                        continue;
                    }
                    set(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                    Log.i("BO2", "bo2_env.txt: " + line);
                }
            } catch (java.io.IOException e) {
                Log.w("BO2", "bo2_env.txt: " + e);
            }
        }
        applyRemoteEnv();
    }

    /**
     * Runtime switches from the PC (bo2-logd's GET /v1/env: NAME=VALUE lines), applied last, so an A/B or a power
     * setting can be tried on the phone without adb or a new build. At most about a second at launch; off the PC's
     * network, nothing changes. What was applied goes into BO2_REMOTE_ENV, which the session's hello reports.
     */
    private static void applyRemoteEnv() {
        if (BuildConfig.LOG_SERVER.isEmpty()) return;
        final StringBuilder body = new StringBuilder();
        Thread fetch = new Thread(() -> {
            java.net.HttpURLConnection c = null;
            try {
                c = (java.net.HttpURLConnection) new java.net.URL("http://" + BuildConfig.LOG_SERVER + "/v1/env")
                        .openConnection();
                c.setConnectTimeout(700);
                c.setReadTimeout(700);
                if (c.getResponseCode() != 200) return;
                try (java.io.BufferedReader r = new java.io.BufferedReader(
                        new java.io.InputStreamReader(c.getInputStream(), java.nio.charset.StandardCharsets.UTF_8))) {
                    for (String line; (line = r.readLine()) != null; ) {
                        synchronized (body) { body.append(line).append('\n'); }
                    }
                }
            } catch (Exception e) {
                Log.i("BO2", "remote env: " + e);
            } finally {
                if (c != null) c.disconnect();
            }
        }, "bo2-remote-env");
        fetch.start();
        try {
            fetch.join(1200);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        StringBuilder applied = new StringBuilder();
        synchronized (body) {
            for (String line : body.toString().split("\n")) {
                line = line.trim();
                int eq = line.indexOf('=');
                if (line.isEmpty() || line.startsWith("#") || eq <= 0) continue;
                set(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                if (applied.length() > 0) applied.append(';');
                applied.append(line);
            }
        }
        if (applied.length() > 0) {
            set("BO2_REMOTE_ENV", applied.toString());
            Log.i("BO2", "remote env: " + applied);
        }
    }

    private static void set(String name, String value) {
        try {
            Os.setenv(name, value, true);
        } catch (ErrnoException e) {
            Log.w("BO2", "setenv " + name + ": " + e);
        }
    }
}
