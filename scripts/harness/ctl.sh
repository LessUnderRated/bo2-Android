#!/usr/bin/env bash
# scripts/harness/ctl.sh <command> ... — send commands to a running harness (runtime/harness/harness_main.cpp).
#   ctl.sh a                     press A          ctl.sh down 200     hold d-pad down 200 ms
#   ctl.sh cmd "set foo 1"       console command  ctl.sh shot name    save the next frame to $RUN/shots/name.png
#   ctl.sh wait <regex> [sec]    wait for a log line (default 120 s); exit 1 on timeout or if the harness died
set -uo pipefail
RUN="${HARNESS_RUN:-$HOME/harness-run}"
alive() { kill -0 "$(cat "$RUN/pid")" 2> /dev/null; }
case "${1:?command}" in
    wait)
        re="${2:?regex}"; t="${3:-120}"
        for _ in $(seq "$t"); do
            grep -qE "$re" "$RUN/log.txt" && exit 0
            alive || { echo "harness exited"; tail -5 "$RUN/log.txt"; exit 1; }
            sleep 1
        done
        echo "timeout waiting for: $re"; exit 1 ;;
    shot)
        name="${2:?name}"; ppm="$RUN/shots/$name.ppm"
        rm -f "$ppm"; echo "shot $ppm" > "$RUN/ctl"
        for _ in $(seq 120); do [ -s "$ppm" ] && break; alive || { echo "harness exited"; exit 1; }; sleep 1; done
        [ -s "$ppm" ] || { echo "no frame within 120 s"; exit 1; }
        sleep 1; python3 "$(dirname "$0")/ppm2png.py" "$ppm" "$RUN/shots/$name.png" && rm -f "$ppm"
        echo "$RUN/shots/$name.png" ;;
    *)
        alive || { echo "harness is not running"; exit 1; }
        echo "$*" > "$RUN/ctl" ;;
esac
