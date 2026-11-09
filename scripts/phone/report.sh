#!/usr/bin/env bash
# scripts/phone/report.sh NAME [N] — symbolise a capture (WSL/Linux; capture.sh calls it).
#
# Writes into the capture: threads.txt (each thread's share of the samples) and fn_<thread>.txt (the top functions
# of each of the N busiest threads, default 6), symbolised with the library saved at capture time (sym/libmain.so).
# The busiest threads are found, not named: host threads are numbered by start order (bo2_h6_..., bo2_h11_...) and
# the number of a role changes between runs.
set -uo pipefail
. "$(dirname "$0")/../env.sh"
NAME=${1:?usage: report.sh NAME [N]}
N=${2:-6}
D="$BO2_CAPTURES/$NAME"
SP=$BO2_SIMPLEPERF
[ -x "$SP" ] || bo2_die "simpleperf not found at $SP (set BO2_SIMPLEPERF)"
[ -s "$D/perf.data" ] || bo2_die "no perf.data in $D"

"$SP" report -i "$D/perf.data" --symdir "$D/sym" --sort comm -n 2> /dev/null > "$D/threads.txt"
mapfile -t TOP < <(awk '/^ *[0-9.]+%/ {print $3}' "$D/threads.txt" | head -"$N")
rm -f "$D"/fn_*.txt
for t in "${TOP[@]}"; do
    "$SP" report -i "$D/perf.data" --symdir "$D/sym" --comms "$t" --sort symbol -n --percent-limit 0.5 2> /dev/null |
        awk '/^ *[0-9.]+%/' | cut -c1-200 > "$D/fn_$t.txt"
done
echo "== $NAME: busiest threads"
awk '/^ *[0-9.]+%/' "$D/threads.txt" | head -"$N"
for t in "${TOP[@]}"; do
    echo "== $t"
    head -8 "$D/fn_$t.txt" | sed -E 's/ +/ /g'
done
