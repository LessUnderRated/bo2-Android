#!/bin/bash
# THE WHOLE CHAIN, PHOTOGRAPHED: intro video -> title screen -> Press Start ->
# main menu -> whatever the frontend does next. One run, seven pictures.
#
# WHY NOT menu_run.sh: that one forces BO2_NOVIDEO=1 (so there is no intro at
# all) and photographs with BO2_SHOT, which does NOT capture the interface layer
# — a menu plainly readable on screen is absent from a BO2_SHOT capture. Ground
# truth for anything with UI on it is the WINDOW, so every picture here comes
# from scripts/shot.sh (CoreGraphics, by window id).
#
#   scripts/full_run.sh <outdir> [extra BO2_* env assignments...]
#
# ⚠️ EVERY WAIT IS ANCHORED TO THE EVENT IT CLAIMS TO FOLLOW. The first version
# of this script searched the WHOLE log, so "a menu that is not LockOutMenu is
# open" matched `open now: HUD` from early boot and reported success ONE SECOND
# after the keystroke, before the main menu existed. That is the same trap the
# guide records twice (§9): an instrument that cannot place itself relative to
# the event will invert the answer. wait_for only ever reads the bytes appended
# AFTER the offset it was given.
set -u
OUT="${1:?usage: full_run.sh <outdir> [VAR=VAL ...]}"; shift
cd "$(dirname "$0")/.."

# 3x the waits when the videos play, 1x otherwise; an explicit BO2_WAIT_SCALE wins.
if [ -z "${BO2_WAIT_SCALE:-}" ]; then
    case " $* " in *" BO2_NOVIDEO=1 "*) BO2_WAIT_SCALE=1 ;; *) BO2_WAIT_SCALE=3 ;; esac
fi
export BO2_WAIT_SCALE
# The key hold must outlast an engine frame. A no-video run sits near 18 fps (55 ms);
# the video-enabled one was measured at 5.8 fps (172 ms) in its own title bar, and
# the old fixed 60 ms hold fell entirely between two input polls.
if [ "$BO2_WAIT_SCALE" = "1" ]; then KEYHOLD=60; else KEYHOLD=400; fi
echo "  waits scaled x$BO2_WAIT_SCALE, key hold ${KEYHOLD}ms"
LOG="$OUT/run.log"

# ⚠️ REFUSE FIRST, THEN TOUCH THE OUTPUT. The other order destroyed a running
# measurement: a second invocation with the same outdir deleted the live run's
# log before discovering it should not start at all, leaving the game writing to
# an unlinked inode and the run unreadable.
# The pattern excludes this script and its shell wrapper — matching the bare
# string also matched the zsh -c command line that launched it, so the check
# refused against ITSELF.
if pgrep -f '\./boot_cache/boot ' > /dev/null; then
    echo "REFUSING: another boot_cache/boot is already running:" >&2
    pgrep -fl '\./boot_cache/boot ' >&2
    exit 1
fi

# ⚠️ A LOCKED MAC LOOKS EXACTLY LIKE A CODE REGRESSION, and this project has
# paid for it at least seven times across four sessions. With the screen locked
# the window server publishes no window, so EVERY capture fails with "could not
# create image from window" — and CoreAudio refuses to start, which matters
# because the engine's clock IS the audio buffer-completion count. The run then
# produces a log full of plausible nothing.
#
# Say it at the START, where it is one line, instead of discovering it from a
# screenshot two-thirds of the way through a 25-minute measurement. Not fatal on
# its own: a log/sample-based measurement is still valid while locked (the
# 2026-08-07 zone-load stall was measured that way and frames kept advancing).
# So warn loudly, name what will be unreliable, and continue.
if ioreg -n Root -d1 | grep -q 'CGSSessionScreenIsLocked.*Yes'; then
    echo "⚠️  ⚠️  THE MAC IS LOCKED (CGSSessionScreenIsLocked=Yes)." >&2
    echo "        Every screenshot in this run WILL fail, and CoreAudio may" >&2
    echo "        refuse to start — which stalls the engine clock." >&2
    echo "        Log- and sample-based results stay valid; pictures do not." >&2
    echo "        Unlock and re-run before judging anything visual." >&2
fi
# A full disk breaks codesign, and the binary is 405 MB.
AVAIL_G=$(df -g /System/Volumes/Data | awk 'NR==2{print $4}')
if [ "${AVAIL_G:-99}" -lt 5 ]; then
    echo "⚠️  only ${AVAIL_G} GB free — codesign fails below a few GB." >&2
    echo "    \$TMPDIR/boot_* lift scratch dirs are safe to delete when no lift runs." >&2
fi
mkdir -p "$OUT"; rm -f "$OUT"/*.png "$OUT"/run.log

logsize() { wc -c < "$LOG" | tr -d ' '; }

# wait_for <regex> <seconds> <byte-offset>: match only in what was written after
# <byte-offset>. Returns 1 on timeout and says so — a wait that gives up quietly
# is how a keystroke reaches a screen that is not up yet.
# Every wait is scaled by BO2_WAIT_SCALE. A run WITH video is markedly slower —
# libvpx runs its scalar C kernels on the one core the cooperative scheduler gives
# it — and the first video-enabled campaign run timed out during NAVIGATION, at
# 25 s and 20 s, long before reaching the difficulty confirm. That is an instrument
# failure, not a result about the game, and reading a null from it would have been
# the capped-view mistake in another costume. Default to 3x whenever video is on.
wait_for() {
    local pat="$1" secs="$2" off="$3" i
    secs=$(( secs * BO2_WAIT_SCALE ))
    for i in $(seq 1 "$secs"); do
        sleep 1
        kill -0 "$PID" 2>/dev/null || { echo "  DIED while waiting for: $pat"; tail -5 "$LOG"; return 2; }
        if python3 -c "
import re,sys
f=open('$LOG','rb'); f.seek($off)
sys.exit(0 if re.search(r'''$pat''', f.read().decode('utf-8','replace')) else 1)"; then
            echo "  matched after ${i}s: $pat"; return 0
        fi
    done
    echo "  TIMEOUT after ${secs}s waiting for: $pat"
    return 1
}

# NEVER swallow a failed capture. An earlier version sent shot.sh's output to
# /dev/null, and two of seven pictures simply did not exist at the end of a run
# that otherwise looked perfect — a missing picture read as "nothing to see".
# Bounded BY POLLING. `wait` was the wrong tool: killing the subshell leaves the
# real capture process alive, and the loop stalled anyway — twice, costing two
# 25-minute measurement runs. Poll the pid, and on timeout kill the whole tree
# INCLUDING screencapture/sips, then carry on.
# ⚠️ A CAPTURE THAT PRODUCED NO FILE MUST NOT PRINT "shot: <name>". It did, for a
# whole run, because this only ever checked whether the subprocess had exited —
# never whether the picture existed. With the Mac's screen LOCKED the window
# server does not publish the game window at all, so every capture failed and
# the run still read as complete. Check the file, and let shot.sh's reason out.
shot() {
    local name="$1" i=0
    ./scripts/shot.sh "$OUT/$name.png" >"$OUT/.shot.err" 2>&1 &
    local sp=$!
    while [ $i -lt 25 ] && kill -0 $sp 2>/dev/null; do sleep 1; i=$((i+1)); done
    if kill -0 $sp 2>/dev/null; then
        pkill -P $sp 2>/dev/null; kill -9 $sp 2>/dev/null
        pkill -9 screencapture 2>/dev/null; pkill -9 sips 2>/dev/null
        echo "  ⚠️  SHOT TIMEOUT ($name) — capture killed, the run continues"
    elif [ -s "$OUT/$name.png" ]; then
        echo "  shot: $name"
    else
        echo "  ⚠️  NO PICTURE ($name):"
        sed 's/^/      /' "$OUT/.shot.err" 2>/dev/null | head -5
    fi
}

# BO2_VPX_NATIVE + BO2_VPX_REPLACE are what make the intro actually appear: the
# lifted VP8 decoder is bypassed and real arm64 libvpx decodes the frames. Without
# them the logos "play" — the .webm files are opened, the engine advances — and the
# window stays BLACK, which is exactly what the first version of this script
# photographed and nearly reported as a broken intro.
env DYLD_LIBRARY_PATH="$PWD/boot_cache/hlslcc_build" \
    BO2_STALL_ABORT=0 BO2_SV_INLINE=1 BO2_WINDOW=1 BO2_XAUDIO=1 \
    BO2_SND_UNMUTE=1 BO2_UIX_PROBE=1 BO2_VPX_NATIVE=1 BO2_VPX_REPLACE=1 \
    "$@" \
    ./boot_cache/boot boot_cache/pe_sections.bin > "$LOG" 2>&1 &
PID=$!
echo "pid=$PID  log=$LOG"

# 1. THE INTRO. Anchored on the game opening the first logo video, not on the
#    first frame the compositor presents — at that point nothing has decoded yet
#    and the window is legitimately black.
echo "[1/4] intro cinematic"
wait_for 'video/dolby\.webm' 120 0 || true
sleep 4;  shot 1a_intro_dolby
wait_for 'video/intro\.webm' 90 0 || true
sleep 4;  shot 1b_intro

# 2. THE TITLE SCREEN — by the menu registry, not by a pixel count.
echo "[2/4] title screen (LockOutMenu)"
wait_for 'open now:.*LockOutMenu' 1200 0 || { echo "no title screen; aborting"; kill $PID; exit 2; }
sleep 2;  shot 2_title

# 3. PRESS START. Native: no BO2_LUA_SRC, nothing injected — the game runs its
#    own select_save_device command and the shim answers it.
echo "[3/4] Return -> main menu"
KEYOFF=$(logsize)
./scripts/sendkey "$PID" 0x24 "$KEYHOLD" && echo "  Return sent"
wait_for '\[SAVEDEV\] select_save_device' 30 "$KEYOFF" || echo "  ⚠️  the game never ran select_save_device"
wait_for '\[UIX-MENUS\] open now:[^\n]*MainMenu' 60 "$KEYOFF" || echo "  ⚠️  MainMenu never entered the open-menu registry"
sleep 3;  shot 3_mainmenu

# 3b. KEYBOARD NAVIGATION. Down, Down, Up — anchored on their own log offset so
#     what they did (or did not do) can be attributed to them and to nothing
#     else. macOS virtual keycodes: 0x7D down, 0x7E up, 0x7B left, 0x7C right.
echo "[3b] arrow keys"
ARROWOFF=$(logsize)
# ⚠️ NET EFFECT, NOT SYMMETRY. The first version sent down,up,left,right — which
#     on a vertical list nets to ZERO, so the photograph showed the focus exactly
#     where it started and proved nothing either way. Two DOWNs have an
#     unambiguous expected outcome: CAMPAIGN -> ZOMBIES -> QUIT.
for k in 0x7D 0x7D; do   # bas, bas
 ./scripts/sendkey "$PID" $k "$KEYHOLD" >/dev/null; sleep 1; done
sleep 3;  shot 3b_after_arrows
python3 -c "
f=open('$LOG','rb'); f.seek($ARROWOFF)
t=f.read().decode('utf-8','replace')
import re
hits=[l for l in t.splitlines() if l.startswith(('[LUI-KEY]','[LUI-EVENT]','[KB-NAV]','[UI-OPENMENU]'))]
print('  lignes clavier apres les fleches:', len(hits))
for l in hits[:10]: print('   ', l[:150])
"

# 3c. BACK UP, then VALIDATE. Two UPs from QUIT must land on CAMPAIGN — starting
#     from a known non-default position, so this proves "up" on its own rather
#     than undoing the previous step into an ambiguous picture. Then Return.
#     ⚠️ NEVER press Return while QUIT holds the focus.
echo "[3c] two UPs, then validate"
./scripts/sendkey "$PID" 0x7E "$KEYHOLD" >/dev/null; sleep 1
./scripts/sendkey "$PID" 0x7E "$KEYHOLD" >/dev/null; sleep 2
shot 3c_back_on_campaign
VALOFF=$(logsize)
./scripts/sendkey "$PID" 0x24 "$KEYHOLD" >/dev/null && echo "  Return (validate) sent"
sleep 5;  shot 3d_after_validate
python3 -c "
f=open('$LOG','rb'); f.seek($VALOFF)
t=f.read().decode('utf-8','replace')
hits=[l for l in t.splitlines() if l.startswith(('[UIX-MENUS]','[LUI-EVENT]','[MISSING]','[STUB-CALL]'))]
print('  apres la validation:', len(hits), 'lignes')
for l in hits[:12]: print('   ', l[:150])
"

# 3e. TRANCHE 5: validate NEW STORY. It is already the focused entry when
#     CampaignMenu opens. The point of this step is NOT that it succeeds — it is
#     to obtain the LIST of what a mission load actually needs, because none of
#     that code has ever executed. Give it real time and photograph the whole way.
# ⚠️ Read the flag from the ARGUMENTS, not from this script's environment. The
# extra VAR=VAL arguments are expanded inside the `env ... "$@"` line, so they
# belong to the GAME's environment and this shell never sees them — the first
# version tested $BO2_TRY_MISSION and the whole step silently never ran, in a
# run that otherwise looked complete.
case " $* " in *" BO2_TRY_MISSION=1 "*) TRY_MISSION=1 ;; *) TRY_MISSION=0 ;; esac
if [ "$TRY_MISSION" = "1" ]; then
    echo "[3e] validate NEW STORY (mission load attempt)"
    MISSOFF=$(logsize)
    # ⚠️ ANCHOR THE KEY ON THE MENU BEING OPEN, do not fire it blind. With video
    # enabled the frontend is markedly slower, the Return landed before
    # CampaignMenu had focus, and the run sat on CampaignMenu for the rest of its
    # life — no popup, no DifficultyMenu, no start_difficulty. That looked exactly
    # like "the game refuses to start a mission" and was a mistimed keystroke.
    # ⚠️ DO NOT anchor on CampaignMenu from MISSOFF: the menu opens in the PREVIOUS
    # step, so the pattern is already behind this offset and the wait can only ever
    # time out. Anchor from the offset of the step that opened it.
    wait_for 'open now:[^\n]*CampaignMenu' 20 "$VALOFF" \
        || echo "  ⚠️  CampaignMenu never appeared — sending Return anyway"
    ./scripts/sendkey "$PID" 0x24 "$KEYHOLD" >/dev/null && echo "  Return on NEW STORY sent"
    # The frontend answers with ConfirmNoSaveDevicePopup — "you are not connected
    # to a save device… continue?" — because OUR OWN payload sets
    # sv_saveDeviceAvailable=false to aim at MainMenu instead of the first-time
    # popups. It is the game behaving correctly, not a defect. YES is already
    # focused, so one more Return continues.
    # ⚠️ TWO DIFFERENT POPUPS, AND THEY DEFAULT TO DIFFERENT ANSWERS.
    #   ConfirmNoSaveDevicePopup  "not connected to a save device"  -> YES focused
    #   the profile popup          "not signed to a profile"         -> NO  focused
    # Which one appears depends on BO2_NO_SAVEDEV. Pressing Return blindly
    # answered NO on the second one and quietly cancelled the whole mission
    # launch — the run looked like it had proceeded and had not.
    sleep 8
    if wait_for 'open now:[^\n]*ConfirmNoSaveDevicePopup' 25 "$MISSOFF"; then
        shot 3e_savedevice_popup
        ./scripts/sendkey "$PID" 0x24 "$KEYHOLD" >/dev/null && echo "  YES sent (save-device popup, YES was focused)"
    elif wait_for 'open now:[^\n]*Popup' 20 "$MISSOFF"; then
        shot 3e_profile_popup
        # ⚠️ DO NOT ASSUME WHICH BUTTON IS FOCUSED. The old code hard-coded "the
        # profile popup starts on NO, so UP moves to YES" and announced success it
        # never checked. The screenshot from the video-enabled run shows the
        # opposite — "You are currently not signed to a profile… Would you like to
        # continue?" with YES already ORANGE — so UP moved focus OFF Yes, the popup
        # was answered NO, and the run bounced back to CampaignMenu while the log
        # cheerfully printed "difficulty (REGULAR) confirmed". The initial focus is
        # NOT stable between configurations, so it cannot be guessed at all.
        #
        # Answer it BOTH ways instead, verifying against DifficultyMenu actually
        # opening. Two attempts, every one printed, and a loud failure at the end.
        for answer in direct up_first; do
            case "$answer" in
                direct)   echo "  popup: answering the focused button as-is" ;;
                up_first) echo "  popup: retrying with UP first (other button)"
                          ./scripts/sendkey "$PID" 0x7E "$KEYHOLD"; sleep 2
                          shot 3e_profile_popup_other ;;
            esac
            ./scripts/sendkey "$PID" 0x24 "$KEYHOLD"
            if wait_for 'open now:[^\n]*DifficultyMenu' 20 "$MISSOFF"; then
                echo "  ✅ DifficultyMenu reached (answer: $answer)"
                POPUP_OK=1; break
            fi
            echo "  ⚠️  no DifficultyMenu after answering \"$answer\""
            # Bounced back to CampaignMenu: re-open the popup for the next answer.
            if [ "$answer" = "direct" ]; then
                wait_for 'open now:[^\n]*CampaignMenu' 15 "$MISSOFF" \
                    && ./scripts/sendkey "$PID" 0x24 "$KEYHOLD" \
                    && wait_for 'open now:[^\n]*Popup' 20 "$MISSOFF" >/dev/null
            fi
        done
        [ "${POPUP_OK:-0}" = "1" ] || echo "  ⚠️  BOTH popup answers failed — no DifficultyMenu"
    else
        echo "  (aucun popup)"
    fi
    # Then SELECT DIFFICULTY (RECRUIT / REGULAR / HARDENED / VETERAN), REGULAR
    # already focused. One more Return is what actually starts the load.
    sleep 12
    shot 3e_difficulty
    # ⚠️ ASSERT, DO NOT ANNOUNCE. This used to print "difficulty (REGULAR) confirmed"
    # off the exit status of sendkey — i.e. off "the keystroke was posted", never off
    # anything the game did. It printed that line in a run whose next screenshot was
    # the CAMPAIGN menu. Anchor it on the response the confirm actually produces.
    ./scripts/sendkey "$PID" 0x24 "$KEYHOLD" >/dev/null && echo "  Return sent on the difficulty menu"
    if wait_for 'start_difficulty' 25 "$MISSOFF"; then
        echo "  ✅ difficulty confirmed — the game accepted the mission start"
    else
        echo "  ⚠️  no start_difficulty response — the mission was NOT started"
    fi
    # PHOTOGRAPH THE STALL ITSELF. The engine drops to 0.0 fps here and no
    # marker names a cause, so the instrument has to be the process: `sample`
    # names the lifted function it is spinning in. Under this runtime every
    # guest thread is a coroutine on one core, so a spin IS a deadlock (§9).
    sleep 25
    if kill -0 "$PID" 2>/dev/null; then
        echo "  sampling the frozen process for 5 s..."
        sample "$PID" 5 -file "$OUT/sample.txt" >/dev/null 2>&1
        python3 -c "
import re,collections
try: t=open('$OUT/sample.txt',errors='replace').read()
except Exception as e: print('   pas de sample:',e); raise SystemExit
subs=collections.Counter(re.findall(r'\b(sub_[0-9a-f]{6})\b', t))
print('   fonctions liftees les plus vues dans la pile :')
for f,n in subs.most_common(12): print(f'     {n:6d}  {f}')
imp=collections.Counter(re.findall(r'\b(win32_[a-z_]+|bo2_[a-z_0-9]+)\b', t))
print('   cote shim :')
for f,n in imp.most_common(8): print(f'     {n:6d}  {f}')
"
    fi
    # BO2_PATIENCE=1 — the CRC reading says the engine is COMPUTING, not stuck.
    # The only way to tell slowness from a real hang is to wait long enough, so
    # sample every 60 s for 15 minutes and report whether anything ever changes.
    case " $* " in *" BO2_PATIENCE=1 "*)
        echo "  patience: 15 min, one shot per minute"
        for m in $(seq 1 15); do
            sleep 60
            kill -0 "$PID" 2>/dev/null || { echo "  DIED at ~${m} min"; break; }
            shot "3e_patience_${m}min"
            python3 -c "
import collections
f=open('$LOG','rb'); f.seek($MISSOFF)
t=f.read().decode('utf-8','replace')
c=collections.Counter(l[:l.find(']')+1] for l in t.splitlines() if l.startswith('['))
big=[f'{k}x{v}' for k,v in c.most_common(6)]
print('   ${m} min:', ' '.join(big) if big else '(rien de neuf)')
"
        done
        ;;
    esac
    for t in 15 35 60 95 140; do
        sleep $( [ $t = 15 ] && echo 15 || echo 25 )
        kill -0 "$PID" 2>/dev/null || { echo "  DIED during the mission load"; break; }
        shot "3e_mission_t${t}s"
    done
    python3 -c "
import collections
f=open('$LOG','rb'); f.seek($MISSOFF)
t=f.read().decode('utf-8','replace')
c=collections.Counter(); ex={}
for l in t.splitlines():
    for m in ('[MISSING]','[STUB-CALL]','[NULL-CALL]','[RUNAWAY]','[STALL-ABORT]','[UIX-MENUS]','[ZONE-LOAD]','[DB-LOADXFILE]','[GIVEUP','[HARDCAP]'):
        if l.startswith(m):
            c[m]+=1
            ex.setdefault(m,[]).append(l[:150])
print('  --- ce que la tentative de mission a produit ---')
for m,n in c.most_common():
    print(f'  {m} x{n}')
    for l in ex[m][:5]: print('     ', l)
"
fi

# 4. WHAT IT DOES NEXT, SAMPLED. The frontend keeps animating (the room camera
#    moves when an item takes focus), so one picture at a guessed moment says
#    nothing about where it settles. Take four.
echo "[4/4] what the frontend does next"
for t in 10 20 35 50; do
    sleep 10; shot "4_t${t}s"
done

kill "$PID" 2>/dev/null
sleep 2

echo "--- what the run said ---"
python3 -c "
pats=('[SAVEDEV]','[BO2-LUA]','[BO2-LUA-BEACON]','[UIX-MENUS]','[MISSING]','[STUB-CALL]',
      '[NULL-CALL]','[RUNAWAY]','[STALL-ABORT]','[HARDCAP]','[NOVIDEO]')
seen={}
for l in open('$LOG',errors='replace'):
    for p in pats:
        if l.startswith(p):
            seen.setdefault(p,[]).append(l.rstrip()[:200])
for p in pats:
    v=seen.get(p)
    if not v: continue
    print(f'{p}  x{len(v)}')
    for l in v[:8]: print('   ',l)
    if len(v)>8: print(f'    ... and {len(v)-8} more')
"
ls "$OUT"/*_s.png 2>/dev/null
