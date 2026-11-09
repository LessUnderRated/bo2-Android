#!/usr/bin/env bash
# scripts/phone/capture.sh NAME [--warmup S] [--secs S] [--now] [--no-report]
#
# One measured run on the phone, for comparing builds. Start it, then play: it waits until a match has started
# ("Server: zm_" in the game's log), lets it settle (--warmup, default 30 s), then records for --secs (default 25):
#   perf.data      simpleperf with call graphs, every thread of the app
#   gpu.txt        GPU busy % and clock, once a second
#   top.txt        per-thread CPU, three samples
#   log.txt        the game's log (BO2 tag) from the start of the capture
#   sym/libmain.so the unstripped library of the build on this PC, so the symbols match the capture later
#   commit.txt     the Bo2-APK commit it was built from
# into $BO2_CAPTURES/NAME, prints a summary and, unless --no-report, symbolises it (report.sh, in WSL).
# --now records at once (menus, loading) instead of waiting for a match.
set -uo pipefail
. "$(dirname "$0")/../env.sh"

NAME=${1:?usage: capture.sh NAME [--warmup S] [--secs S] [--now] [--no-report]}
shift
WARM=30; SECS=25; NOW=0; REPORT=1
while [ $# -gt 0 ]; do
    case "$1" in
        --warmup) WARM=$2; shift 2 ;;
        --secs) SECS=$2; shift 2 ;;
        --now) NOW=1; shift ;;
        --no-report) REPORT=0; shift ;;
        *) bo2_die "unknown option $1" ;;
    esac
done
[ -x "$ADB" ] || [ -f "$ADB" ] || bo2_die "adb not found at $ADB (set ADB in scripts/env.local.sh)"
O="$BO2_CAPTURES/$NAME"
[ -e "$O/perf.data" ] && bo2_die "$O already has a capture; pick another name"
mkdir -p "$O/sym"
cd "$O" || exit 1                       # adb.exe gets relative paths only (see env.sh)

[ -n "$BO2_LIBMAIN" ] && cp -f "$BO2_LIBMAIN" sym/libmain.so
git -C "$BO2_REPO" log -1 --format='%h %s' > commit.txt 2>/dev/null

"$ADB" logcat -c
"$ADB" logcat -v time -s BO2:I > log.txt 2>/dev/null &
LOGCAT=$!
trap 'kill $LOGCAT 2>/dev/null' EXIT

if [ "$NOW" = 0 ]; then
    bo2_say "waiting for a match (start one on the phone)"
    until grep -aq "Server: zm_" log.txt 2>/dev/null; do sleep 2; done
    bo2_say "in a match ($(grep -a 'Server: zm_' log.txt | tail -1 | sed 's/.*Server: //')); settling for $WARM s"
    sleep "$WARM"
fi
PID=$("$ADB" shell pidof "$BO2_PKG" | tr -d '\r')
[ -n "$PID" ] || bo2_die "the game is not running"

DEV=/data/local/tmp/bo2-capture.perf.data
"$ADB" shell "rm -f $DEV"
bo2_say "recording $SECS s"
"$ADB" shell "simpleperf record --app $BO2_PKG -g -f 1000 --duration $SECS -o $DEV" > record.txt 2>&1 &
REC=$!
(
    for _ in $(seq "$SECS"); do
        echo "$("$ADB" shell 'cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage /sys/class/kgsl/kgsl-3d0/clock_mhz' |
            tr -d '\r' | tr '\n' ' ')"
        sleep 1
    done
) > gpu.txt &
GPU=$!
for _ in 1 2 3; do
    "$ADB" shell "top -H -b -n 1 -p $PID | head -26" >> top.txt
    echo ----- >> top.txt
    sleep $(( SECS / 3 ))
done
wait $REC; wait $GPU

# simpleperf can still be writing when the shell returns: pull until the file is complete.
for _ in 1 2 3 4 5 6 7 8 9 10; do
    "$ADB" pull "$DEV" perf.data > /dev/null 2>&1 && [ -s perf.data ] &&
        grep -q "Samples recorded" record.txt && break
    sleep 2
done
[ -s perf.data ] || bo2_say "WARNING: no perf.data pulled (record.txt says: $(tail -1 record.txt))"
kill $LOGCAT 2>/dev/null

{
    echo "capture $NAME, build $(cat commit.txt 2>/dev/null)"
    echo "samples: $(grep -a 'Samples recorded' record.txt | sed 's/.*Samples recorded: //')"
    echo "gpu busy%/MHz: $(tr '\n' '|' < gpu.txt | cut -c1-160)"
    grep -a "\[PERF\]" log.txt | tail -$(( SECS / 5 + 1 )) | sed 's/.*\[PERF\] //'
    grep -a "\[HW-ATOMIC\] perf\|\[ADPF\]\|\[AUDIO\]\|\[CRASH\] signal" log.txt | sed 's/.*I\/BO2 *([0-9]*): //' | tail -6
} | tee summary.txt

if [ "$REPORT" = 1 ] && [ -s perf.data ]; then
    bo2_linux "bash $(bo2_q "$BO2_REPO_WSL")/scripts/phone/report.sh '$NAME'"
fi
