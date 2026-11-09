#!/usr/bin/env bash
# scripts/phone/symbolize-prof.sh LOG [libmain.so] [N] — the app's own sampling profile, by function (WSL/Linux).
#
# LOG is any text holding the app's [PROF] lines (a log server session, a logcat dump, a capture's log.txt). The
# app samples its busiest threads by CPU time (android/native/src/prof.cpp) and logs, every 30 s, each thread's most
# frequent program counters as offsets into libmain.so, with that library's build id. Here every window of every
# thread is added up, the offsets are symbolised with the NDK's llvm-symbolizer against the unstripped libmain.so of
# the same build, and each thread's top N functions (default 25) are printed with their share of its samples.
#
# The library: the second argument, or $BO2_SYMBOLS/<build id>/libmain.so (BO2_SYMBOLS, scripts/env.sh;
# scripts/build.sh archives each build's unstripped library there by build id).
# A log holding several builds is symbolised per build.
#
# Samples in other libraries (libc, the Vulkan driver, libc++) are named when the app could name them (`fn=` and
# `via=` tokens, prof.cpp): each thread's list is followed by those functions and by the libmain functions that
# called them (from the link register). `memcpy*`: the implementation an ifunc resolved to; `?<free`: hidden code
# called from free; `?`: hidden code. Logs from before these tokens print as before.
set -euo pipefail
. "$(dirname "$0")/../env.sh"
LOG=${1:?usage: symbolize-prof.sh LOG [libmain.so] [N]}
LIB_ARG=${2:-}
N=${3:-25}
SYMBOLS=$BO2_SYMBOLS
TC=${BO2_NDK_BIN:-$HOME/sdk/android-ndk-r28c/toolchains/llvm/prebuilt/linux-x86_64/bin}
SYM=$TC/llvm-symbolizer
[ -x "$SYM" ] || bo2_die "llvm-symbolizer not found at $SYM (set BO2_NDK_BIN)"
[ -f "$LOG" ] || bo2_die "no such file: $LOG"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export LC_ALL=C

# count<TAB>lib!name[<TAB>more] on stdin: the name demangled when it is a mangled C++ name and the NDK has llvm-cxxfilt.
demangle() {
    cat > "$W/dm.in"
    if [ -x "$TC/llvm-cxxfilt" ] && [ -s "$W/dm.in" ]; then
        awk -F '\t' '{i = index($2, "!"); print substr($2, i + 1)}' "$W/dm.in" | "$TC/llvm-cxxfilt" > "$W/dm.out" 2> /dev/null || cp "$W/dm.in" "$W/dm.out"
        awk -F '\t' -v OFS='\t' 'FNR == NR {d[FNR] = $0; next}
            {i = index($2, "!"); if (FNR in d) $2 = substr($2, 1, i) d[FNR]; print}' "$W/dm.out" "$W/dm.in"
    else
        cat "$W/dm.in"
    fi
}

# One record per token: build thread kind key count. kind: S (samples of a window), M (in libmain), O (other DSO),
# P (pc offset).
grep -a "\[PROF\] build=" "$LOG" | sed 's/.*\[PROF\] //' | awk '
    { build = ""; thread = ""
      for (i = 1; i <= NF; i++) {
          split($i, kv, "=")
          if (kv[1] == "build") build = kv[2]
          else if (kv[1] == "thread") thread = kv[2]
          else if (kv[1] == "samples") print build, thread, "S", "-", kv[2]
          else if (kv[1] == "libmain") print build, thread, "M", "-", kv[2]
          else if (kv[1] == "other" && kv[2] != "-") {
              n = split(kv[2], parts, ",")
              for (j = 1; j <= n; j++) { split(parts[j], o, ":"); print build, thread, "O", o[1], o[2] }
          } else if (kv[1] == "off") { split(kv[2], o, ":"); print build, thread, "P", o[1], o[2] }
          else if (kv[1] == "fn" || kv[1] == "via") {
              # fn=lib!name:count, via=lib!name@offset:count (names hold no spaces, colons or equals signs)
              v = substr($i, length(kv[1]) + 2); c = v; sub(/.*:/, "", c); sub(/:[0-9]+$/, "", v)
              print build, thread, (kv[1] == "fn" ? "F" : "V"), v, c
          }
      } }' > "$W/records"
[ -s "$W/records" ] || bo2_die "no [PROF] lines in $LOG"

for build in $(awk '{print $1}' "$W/records" | sort -u); do
    LIB=$LIB_ARG
    [ -n "$LIB" ] || LIB="$SYMBOLS/$build/libmain.so"
    echo "== build $build ($(grep -c "^$build " "$W/records") records), symbols: $LIB"
    if [ ! -f "$LIB" ]; then
        echo "   no libmain.so for this build: offsets only"
        : > "$W/syms"
    else
        # Every distinct offset once, symbolised in one call: two lines per address (function, file:line).
        awk -v b="$build" '$1 == b && $3 == "P" {print "0x" $4}' "$W/records" | sort -u > "$W/addrs"
        "$SYM" --obj="$LIB" --demangle --functions=linkage --no-inlines --output-style=GNU < "$W/addrs" 2> /dev/null |
            awk 'NR % 2 == 1' > "$W/funcs"
        paste -d '\t' "$W/addrs" "$W/funcs" | sed 's/^0x//' > "$W/syms"
    fi
    # The libmain callers of other libraries' functions: function and file:line (which call it was).
    : > "$W/vsyms"
    awk -v b="$build" '$1 == b && $3 == "V" {n = split($4, p, "@"); print "0x" p[n]}' "$W/records" | sort -u > "$W/vaddrs"
    if [ -f "$LIB" ] && [ -s "$W/vaddrs" ]; then
        "$SYM" --obj="$LIB" --demangle --functions=linkage --no-inlines --output-style=GNU < "$W/vaddrs" 2> /dev/null |
            awk 'NR % 2 == 1 {f = $0; next} {n = split($0, p, "/"); l = p[n]; sub(/ \(discriminator.*/, "", l); print l "  " f}' > "$W/vfuncs"
        paste -d '\t' "$W/vaddrs" "$W/vfuncs" | sed 's/^0x//' > "$W/vsyms"
    fi
    for thread in $(awk -v b="$build" '$1 == b {print $2}' "$W/records" | sort -u); do
        total=$(awk -v b="$build" -v t="$thread" '$1 == b && $2 == t && $3 == "S" {s += $5} END {print s + 0}' "$W/records")
        [ "$total" -gt 0 ] || continue
        inmain=$(awk -v b="$build" -v t="$thread" '$1 == b && $2 == t && $3 == "M" {s += $5} END {print s + 0}' "$W/records")
        echo "-- $thread: $total samples, $(awk -v a="$inmain" -v t="$total" 'BEGIN {printf "%.1f", 100 * a / t}')% in libmain"
        {
            # pc offsets -> functions (or the bare offset without symbols); other libraries as themselves
            awk -v b="$build" -v t="$thread" -F ' ' 'FILENAME == ARGV[1] {split($0, s, "\t"); fn[s[1]] = s[2]; next}
                $1 == b && $2 == t && $3 == "P" {f = ($4 in fn && fn[$4] != "??") ? fn[$4] : "libmain+0x" $4; c[f] += $5}
                $1 == b && $2 == t && $3 == "O" {c["[" $4 "]"] += $5}
                END {for (f in c) printf "%d\t%s\n", c[f], f}' "$W/syms" "$W/records"
        } | sort -t $'\t' -k1,1 -n -r | head -"$N" |
            awk -F '\t' -v t="$total" '{printf "   %5.1f%%  %6d  %s\n", 100 * $1 / t, $1, substr($2, 1, 110)}'
        # Other libraries by function, then by libmain caller. Each window keeps only its top names: lower bounds.
        awk -v b="$build" -v t="$thread" '$1 == b && $2 == t && $3 == "F" {c[$4] += $5}
            END {for (f in c) printf "%d\t%s\n", c[f], f}' "$W/records" | sort -t $'\t' -k1,1 -n -r | head -"$N" > "$W/fns"
        if [ -s "$W/fns" ]; then
            echo "   other libraries, by function:"
            demangle < "$W/fns" | awk -F '\t' -v t="$total" '{i = index($2, "!")
                printf "   %5.1f%%  %6d  [%s] %s\n", 100 * $1 / t, $1, substr($2, 1, i - 1), substr($2, i + 1, 100)}'
        fi
        awk -v b="$build" -v t="$thread" 'FILENAME == ARGV[1] {split($0, s, "\t"); fn[s[1]] = s[2]; next}
            $1 == b && $2 == t && $3 == "V" {n = split($4, p, "@"); o = p[n]; f = substr($4, 1, length($4) - length(o) - 1)
                w = (o in fn && fn[o] !~ /^\?\?/) ? fn[o] : "libmain+0x" o; c[f "\t" w] += $5}
            END {for (k in c) printf "%d\t%s\n", c[k], k}' "$W/vsyms" "$W/records" |
            sort -t $'\t' -k1,1 -n -r | head -"$N" > "$W/via"
        if [ -s "$W/via" ]; then
            echo "   ... by caller in libmain:"
            demangle < "$W/via" | awk -F '\t' -v t="$total" '{i = index($2, "!")
                printf "   %5.1f%%  %6d  [%s] %s <- %s\n", 100 * $1 / t, $1, substr($2, 1, i - 1), substr($2, i + 1, 60),
                       substr($3, 1, 100)}'
        fi
    done
done
