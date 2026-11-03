#!/bin/bash
# fps_after_spawn.sh <run.log[.gz]> ...
# The ONLY frame rate that means anything for angola: images per second AFTER the
# player spawn marker. 97% of a run's frames are the load, so a whole-run average
# is a measurement of the loading screen (see bo2-measure-after-the-spawn-marker).
# Time is the [CADENCE] clock (sum of elapsed_ms); frames are guestFrames deltas.
for f in "$@"; do
  case "$f" in *.gz) C="gzip -dc" ;; *) C=cat ;; esac
  $C "$f" | LC_ALL=C awk -v name="$f" '
    /^\[CADENCE\]/ {
      if (match($0, /elapsed_ms=[0-9.]+/)) t += substr($0, RSTART+11, RLENGTH-11)/1000
      if (match($0, /guestFrames=[0-9]+/)) { g = substr($0, RSTART+12, RLENGTH-12)+0
        if (sp && !g0) { g0 = g; t0 = t }
        if (sp) { g1 = g; t1 = t } }
    }
    /SETORIGIN\] #2/ && !sp { sp = 1 }
    END {
      if (!sp) { printf "%s: spawn never reached\n", name; exit }
      d = t1 - t0
      fps = 0; if (d > 0) fps = (g1-g0)/d
      printf "%s: %d frames in %.0f s after spawn = %.2f fps\n", name, g1-g0, d, fps
    }'
done
