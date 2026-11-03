#!/bin/bash
# confirm3.sh <prefix> [budget_s] [extra VAR=VAL ...]
# Three angola runs in the synchronised game (menu profile = zero guards), one line
# of summary each: spawn time, last advancing frame, seconds of play after spawn,
# VM leak, sound fatal, lock waits, [MISSING], crash, [SPIN] sample, unwind hole.
# ⚠️ `missing=` counts only "[MISSING] block at". An indirect call to an address
# the shim cannot resolve prints "function_call: unknown target" instead, and that
# is the class that ended w28_3 (and that hid 2,199 calls to the unlifted GSC
# builtin absangleclamp360). `unk=` carries the top three, with their counts.
# Times are the [CADENCE] clock (sum of elapsed_ms). Example after a reboot:
#     bash scripts/confirm3.sh w26 720
set -u
cd "$(dirname "$0")/.."
P=${1:?prefix}; B=${2:-720}; shift; [ $# -gt 0 ] && shift
for i in 1 2 3; do
  d=runs/${P}_$i; echo "${P}_$i START $(date +%H:%M:%S) disk=$(df -h / | tail -1 | awk '{print $4}')"
  BO2_RUN_PROFILE=menu bash scripts/angola_run.sh "$d" "$B" BO2_RUNAWAY_IMPORTS=0 BO2_RR_FAIR=1 BO2_REAL_MUTEX=1 BO2_GSC_TRACE=1 ${@+"$@"} > "$d.out" 2>&1
  L=$d/run.log
  if [ ! -f "$L" ]; then echo "${P}_$i: REFUSED — $(grep -m1 -i 'refus' "$d.out")"; continue; fi
  clk=$(LC_ALL=C awk '/^\[CADENCE\]/{ if (match($0,/elapsed_ms=[0-9.]+/)) t+=substr($0,RSTART+11,RLENGTH-11)/1000; if (match($0,/guestFrames=[0-9]+\(\+[0-9]+\)/)) { g=substr($0,RSTART,RLENGTH); if (g !~ /\(\+0\)/) last=t } } /SETORIGIN\] #2/ && !sp {sp=t} END{printf "spawn@%.0fs last_frame@%.0fs (+%.0fs after spawn) end@%.0fs", sp, last, last-sp, t}' "$L")
  g() { LC_ALL=C grep -a "$@" "$L"; }
  echo "${P}_$i: $clk | leak=$(g -c 'pass #[0-9]* over') snd=$(g -c 'sound globals have been') rwread=$(g -c 'RWLOCK-READ') cas=$(g -c 'CAS-SPIN #') missing=$(g -o 'MISSING\] block at 0x[0-9a-f]*' | sort -u | tr '\n' ' ') unk=$(g -o 'unknown target 0x[0-9a-f]*' | sort | uniq -c | sort -rn | head -3 | tr -s ' \n' ' ') crash=$(g -o 'CRASH (signal [0-9]*)\|remill_error\] pc=0x[0-9a-f]*' | head -2 | tr '\n' ' ') hardcap=$(g -c HARDCAP) unwind=$(g -o 'unwind\] tid=[0-9]* NO LIFTED BLOCK at 0x[0-9a-f]*' | head -1) spin=$(g -A1 'SPIN\] (frozen' | tail -1 | cut -c1-60)"
  gzip -9 -f "$L"
done
