#!/usr/bin/env bash
# scripts/phone/compare.sh A B [THREAD] — two captures side by side (text only; runs anywhere).
#
#   frames:  mean fps, median p99, worst frame, frames over 33 ms, mean power, from the [PERF] lines
#   threads: each thread's share of the samples
#   THREAD (default the game's main loop, bo2_ordonnanceu): its top functions in A and B, with the change
set -uo pipefail
. "$(dirname "$0")/../env.sh"
A=${1:?usage: compare.sh A B [THREAD]}
B=${2:?usage: compare.sh A B [THREAD]}
T=${3:-bo2_ordonnanceu}
DA="$BO2_CAPTURES/$A"; DB="$BO2_CAPTURES/$B"
for d in "$DA" "$DB"; do [ -f "$d/summary.txt" ] || bo2_die "no capture at $d"; done

export LC_ALL=C   # join and sort must agree on the order
perf_stats() {   # the [PERF] lines of a capture -> one line of stats (plain awk: WSL's is mawk)
    local lines med
    lines=$(grep -a "fps, frame ms" "$1/summary.txt")
    [ -n "$lines" ] || { echo "no [PERF] lines"; return; }
    med=$(echo "$lines" | awk '{for (i = 1; i <= NF; i++) if ($i == "p99") print $(i+1)}' | sort -n |
        awk '{v[NR] = $1} END {print v[int((NR + 1) / 2)]}')
    echo "$lines" | awk -v med="$med" '
        { for (i = 1; i <= NF; i++) {
              if ($(i+1) == "fps,") fps += $i
              if ($i == "max") { m = $(i+1); sub(/,/, "", m); if (m + 0 > worst) worst = m + 0 }
              if ($i == "ms:") { s = $(i+1); sub(/,/, "", s); slow += s }
              if ($(i+1) == "W,") w += $i
          }
          n++ }
        END { printf "%5.1f fps | p99 median %5.1f ms | worst %6.1f ms | >33 ms: %3d | %4.1f W  (%d windows)\n",
                  fps / n, med, worst, slow, w / n, n }'
}
printf "%-24s %s" "$A" "$(perf_stats "$DA")"; echo
printf "%-24s %s" "$B" "$(perf_stats "$DB")"; echo

echo
echo "threads (share of samples)            $A -> $B"
join -a1 -a2 -e 0 -o 0,1.2,2.2 \
    <(awk '/^ *[0-9.]+%/ {sub(/%/, "", $1); print $3, $1}' "$DA/threads.txt" 2> /dev/null | sort) \
    <(awk '/^ *[0-9.]+%/ {sub(/%/, "", $1); print $3, $1}' "$DB/threads.txt" 2> /dev/null | sort) |
    sort -k3 -n -r | head -10 | awk '{printf "  %-34s %6.2f%% %6.2f%%  %+6.2f\n", $1, $2, $3, $3 - $2}'

echo
echo "$T top functions (% of the thread)    $A -> $B"
fa="$DA/fn_$T.txt"; fb="$DB/fn_$T.txt"
if [ -f "$fa" ] && [ -f "$fb" ]; then
    join -t $'\t' -a1 -a2 -e 0 -o 0,1.2,2.2 \
        <(awk '{p = $1; sub(/%/, "", p); $1 = ""; $2 = ""; sub(/^ +/, ""); print $0 "\t" p}' "$fa" | sort -t $'\t' -k1,1) \
        <(awk '{p = $1; sub(/%/, "", p); $1 = ""; $2 = ""; sub(/^ +/, ""); print $0 "\t" p}' "$fb" | sort -t $'\t' -k1,1) |
        sort -t $'\t' -k2 -n -r | head -20 |
        awk -F '\t' '{printf "  %-46.46s %6.2f%% %6.2f%%  %+6.2f\n", $1, $2, $3, $3 - $2}'
else
    echo "  (no fn_$T.txt in one of the captures: run report.sh)"
fi
