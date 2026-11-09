#!/usr/bin/env bash
# scripts/phone/fx-ab.sh LOG — what the game's effects cost, from one BO2_FX_AB session (docs/GRAPHICS.md, "Effects").
#
# With BO2_FX_AB=<seconds> (remote env), the app cycles the effects through three states, each held that long, and
# logs `[FX-AB] effects <state> for N s`:
#   drawn  — as usual;
#   frozen — fx_freeze 1: the game stops updating its effects (their simulation's CPU cost);
#   hidden — the particle-like draws dropped in the backend (what drawing them costs our layer and the GPU).
# Here every 5 s [PERF], [THREADS] and [SYSINFO] window and every 10 s [FX-DRAWS] window that lies wholly inside one
# state, once a map is loading or loaded (the menus are left out), is added to that state, and each state's averages
# are printed side by side. LOG is a log server session's log.txt, a logcat dump or a capture's log.txt.
set -euo pipefail
LOG=${1:?usage: fx-ab.sh LOG}
[ -f "$LOG" ] || { echo "no such file: $LOG" >&2; exit 1; }
LC_ALL=C awk '
    function num(s) { gsub(/[^0-9.]/, "", s); return s + 0 }
    # The value after "key" in a line of "key value" pairs (the [PERF] format), or after "key=" ([THREADS]).
    function after(line, key,   i, rest, parts) {
        i = index(line, key); if (!i) return -1
        rest = substr(line, i + length(key)); sub(/^[= ]*/, "", rest); split(rest, parts, /[ ,%@]/); return parts[1] + 0
    }
    /Loading fastfile .zm_/ { inmap = 1 }
    /\[FX-AB\] effects / {
        for (i = 1; i <= NF; i++) if ($i == "effects") st = $(i + 1)
        t0 = $1 + 0; next
    }
    !inmap || st == "" { next }
    /\[PERF\] 5\.0 s:/ {
        t = $1 + 0; if (t - t0 < 5.5) next
        n[st]++; fps[st] += after($0, "s:"); p50[st] += after($0, "p50")
        w = $0; sub(/ W,.*/, "", w); sub(/.* /, "", w); watt[st] += w
        next
    }
    /\[THREADS\]/ {
        t = $1 + 0; if (t - t0 < 5.5) next
        nt[st]++; tot[st] += after($0, "total")
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^bo2_ordonnanceu=/) { split($i, a, /[=%]/); mainc[st] += a[2] }
            if ($i ~ /^bo2_game=/) { split($i, a, /[=%]/); vk[st] += a[2] }
            if ($i ~ /^bo2_h6_/) { split($i, a, /[=%]/); h6[st] += a[2] }
        }
        next
    }
    /\[SYSINFO\]/ {
        t = $1 + 0; if (t - t0 < 5.5) next
        for (i = 1; i <= NF; i++) if ($i ~ /^gpu=/) { split($i, a, /[=%@M]/); ns[st]++; gb[st] += a[2]; gm[st] += a[4] }
        next
    }
    /\[FX-DRAWS\]/ {
        t = $1 + 0; if (t - t0 < 10.5) next
        nf[st]++
        line = $0; sub(/.*frames: /, "", line); split(line, a, / /); fxd[st] += a[1]
        line = $0; sub(/.*particle-like draws \(/, "", line); split(line, a, / /); fxk[st] += a[1]
        line = $0; sub(/.*the CPU\), /, "", line); split(line, a, / /); fxs[st] += a[1]
        line = $0; sub(/.*buffers, of /, "", line); split(line, a, / /); fxa[st] += a[1]
        next
    }
    function avg(s, c) { return c ? sprintf("%.1f", s / c) : "-" }
    END {
        split("drawn frozen hidden", S, " ")
        printf "%-34s %10s %10s %10s\n", "", S[1], S[2], S[3]
        printf "%-34s %10s %10s %10s\n", "[PERF] 5 s windows", n[S[1]] + 0, n[S[2]] + 0, n[S[3]] + 0
        printf "%-34s %10s %10s %10s\n", "fps", avg(fps[S[1]], n[S[1]]), avg(fps[S[2]], n[S[2]]), avg(fps[S[3]], n[S[3]])
        printf "%-34s %10s %10s %10s\n", "frame ms p50", avg(p50[S[1]], n[S[1]]), avg(p50[S[2]], n[S[2]]), avg(p50[S[3]], n[S[3]])
        printf "%-34s %10s %10s %10s\n", "watts", avg(watt[S[1]], n[S[1]]), avg(watt[S[2]], n[S[2]]), avg(watt[S[3]], n[S[3]])
        printf "%-34s %10s %10s %10s\n", "CPU, all threads (% of a core)", avg(tot[S[1]], nt[S[1]]), avg(tot[S[2]], nt[S[2]]), avg(tot[S[3]], nt[S[3]])
        printf "%-34s %10s %10s %10s\n", "game logic thread %", avg(mainc[S[1]], nt[S[1]]), avg(mainc[S[2]], nt[S[2]]), avg(mainc[S[3]], nt[S[3]])
        printf "%-34s %10s %10s %10s\n", "game render thread (h6) %", avg(h6[S[1]], nt[S[1]]), avg(h6[S[2]], nt[S[2]]), avg(h6[S[3]], nt[S[3]])
        printf "%-34s %10s %10s %10s\n", "Vulkan thread %", avg(vk[S[1]], nt[S[1]]), avg(vk[S[2]], nt[S[2]]), avg(vk[S[3]], nt[S[3]])
        printf "%-34s %10s %10s %10s\n", "GPU busy %", avg(gb[S[1]], ns[S[1]]), avg(gb[S[2]], ns[S[2]]), avg(gb[S[3]], ns[S[3]])
        printf "%-34s %10s %10s %10s\n", "GPU MHz", avg(gm[S[1]], ns[S[1]]), avg(gm[S[2]], ns[S[2]]), avg(gm[S[3]], ns[S[3]])
        printf "%-34s %10s %10s %10s\n", "particle-like draws / frame", avg(fxd[S[1]], nf[S[1]]), avg(fxd[S[2]], nf[S[2]]), avg(fxd[S[3]], nf[S[3]])
        printf "%-34s %10s %10s %10s\n", "  their CPU-built vertices KB", avg(fxk[S[1]], nf[S[1]]), avg(fxk[S[2]], nf[S[2]]), avg(fxk[S[3]], nf[S[3]])
        printf "%-34s %10s %10s %10s\n", "static translucent draws / frame", avg(fxs[S[1]], nf[S[1]]), avg(fxs[S[2]], nf[S[2]]), avg(fxs[S[3]], nf[S[3]])
        printf "%-34s %10s %10s %10s\n", "all draws / frame", avg(fxa[S[1]], nf[S[1]]), avg(fxa[S[2]], nf[S[2]]), avg(fxa[S[3]], nf[S[3]])
        if (!n[S[1]] && !n[S[2]] && !n[S[3]])
            print "(no [FX-AB] states in a map: was BO2_FX_AB set, and was a map played?)"
    }
' "$LOG"
