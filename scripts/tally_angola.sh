#!/usr/bin/env bash
# Run angola N times and TALLY the outcome, because a single run here is not
# evidence: the same build has produced registry depths of 27, 152 and 22, and
# [KILLTHREAD] fires from the teardown in some runs and not others. Quoting one
# run has already produced three retracted conclusions in this investigation.
#
# Each run is disk-guarded (this machine hard-hangs when free space runs out).
N=${N:-5}
SECS=${SECS:-360}
FLAGS="BO2_KB_PHYSICAL=1 BO2_SEARCH_GUARD=1 BO2_MT_NULLGUARD=2 BO2_MT_GUARD=2 BO2_SWEEP=3 BO2_PVAR_GUARD=1 BO2_ASSET_WAIT_TYPES=25 BO2_ASSET_WAIT_MS=2000 $EXTRA"
alive() { [ "$(ps -Ao args | grep -c '[b]oot_cache/boot')" -gt 0 ]; }
echo "run,depth,killthread_teardown,unlinks,decref_zero,free_registered,setorigin,crash,frames"
for r in $(seq 1 "$N"); do
  # ⚠️ play.sh REFUSES to start while a game is still running:
  #     "REFUSING: a game is already running (kill it first…)"
  # The first version waited at most 16s for the previous run to die and never
  # checked whether the launch actually took. Result: runs 2..N were refused in
  # SILENCE and tallied as near-empty rows (depth=0, frames=0), which read as
  # "the game is unstable" when it was the harness. Wait properly, then VERIFY.
  pkill -f boot_cache/boot 2>/dev/null
  for k in $(seq 1 30); do sleep 2; alive || break; done
  if alive; then
    echo "$r,HARNESS-ERROR-previous-run-would-not-die,,,,,,," ; continue
  fi
  rm -f runs/play-angola/run.log
  # shellcheck disable=SC2086
  nohup scripts/play.sh angola $FLAGS >/tmp/tally_launch.out 2>&1 &
  sleep 20
  if ! alive; then
    if grep -qi REFUS /tmp/tally_launch.out 2>/dev/null; then
      echo "$r,HARNESS-ERROR-launch-refused,,,,,,," ; continue
    fi
    echo "$r,HARNESS-ERROR-launch-did-not-start,,,,,,," ; continue
  fi
  t=20
  while [ "$t" -lt "$SECS" ]; do
    sleep 12; t=$((t+12))
    alive || break
    [ "$(df -m / | tail -1 | awk '{print $4}')" -lt 1200 ] && { pkill -f boot_cache/boot; break; }
  done
  pkill -f boot_cache/boot 2>/dev/null; sleep 3
  L=runs/play-angola/run.log
  depth=$(grep -a "inst=0 head\[0x02f3b818\]" "$L" 2>/dev/null | grep -ao "registryDepth=[0-9]*" | tail -1 | cut -d= -f2)
  kt=$(grep -a "KILLTHREAD" "$L" 2>/dev/null | grep -ac "0x0047748e")
  ul=$(grep -ac "UNSTICK\] head" "$L" 2>/dev/null)
  dz=$(grep -ac "DECREF-ZERO\] decref2" "$L" 2>/dev/null)
  fr=$(grep -ac "FREE-WHILE-REGISTERED\] decref2" "$L" 2>/dev/null)
  so=$(grep -ac "SETORIGIN" "$L" 2>/dev/null)
  cr=$(grep -ac "=== CRASH" "$L" 2>/dev/null)
  gf=$(grep -ao "guestFrames=[0-9]*" "$L" 2>/dev/null | tail -1 | cut -d= -f2)
  # Keep the lines that matter from EVERY run, because the log is deleted before
  # the next one and the interesting case shows up in only some runs.
  mkdir -p runs/tally
  grep -a "KILLTHREAD\|vars\[0x\|FREE-WHILE-REGISTERED\|registryDepth\|=== CRASH\|RUNAWAY" \
       "$L" > "runs/tally/run$r.txt" 2>/dev/null
  echo "$r,${depth:-none},$kt,$ul,$dz,$fr,$so,$cr,${gf:-0}"
done
