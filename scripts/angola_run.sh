#!/bin/bash
# angola_run.sh — the campaign run, bounded, with the REFERENCE CONFIGURATION and
# the error catalogue armed. This is the runner replay_verify.sh drives for the
# stage-2 success criterion: two replays of one journal must produce the same
# BO2_ERR_CATALOGUE in the same order.
#
#   scripts/angola_run.sh <outdir> [seconds] [extra BO2_* assignments...]
#
# THE REFERENCE CONFIGURATION is not a guess — it is the last commit's measured
# best-known set. Two entries carry warnings that cost runs:
#
#   ⚠️ BO2_CURSOR_CLAMP IS DELIBERATELY ABSENT. It is our own clamp and it
#      corrupts the base of the GSC locals. Do not add it back.
#   ⚠️ BO2_MSGB is capped at 150 ON PURPOSE. Above that the level stops loading,
#      so a bigger catalogue buys nothing and loses the run.
#   ⚠️ BO2_NOVIDEO is NOT set. 8d7e14e measured that the intro path is
#      load-bearing for the zone load; without video the level never loads and
#      every measurement taken from such a run is about a run that did not happen.
set -u
OUT="${1:?usage: angola_run.sh <outdir> [seconds] [VAR=VAL ...]}"; shift
SECS="${1:-300}"; case "$SECS" in ''|*[!0-9]*) SECS=300 ;; *) shift ;; esac
cd "$(dirname "$0")/.."
mkdir -p "$OUT"; rm -f "$OUT"/run.log

# ── 🎯 A FAILED BUILD MUST NOT LET A RUN START, AND A LOG MUST NAME ITS BINARY ──
#
# Twice today a link error left boot_cache/boot at the PREVIOUS build while runs
# launched anyway: the flags under test were not in the binary, the A/B compared
# a build against itself, and nothing said so. That is not a silent probe, it is
# a silent broken LINK — a step that fails without stopping anything.
#
# (a) refuse to start if the binary is older than the newest source file. A stale
#     binary is exactly the failed-build signature, and it is cheap to detect.
# (b) stamp every run.log with the binary's fingerprint, so any log can be shown
#     afterwards to have come from the build one thinks it did. Without this an
#     A/B can never be proved to have compared two different binaries — and one
#     has already been retracted for precisely that.
BIN="boot_cache/boot"
[ -x "$BIN" ] || { echo "REFUSING: $BIN missing — the build did not produce a binary"; exit 1; }
NEWER=$(find runtime scripts/build_only.sh -newer "$BIN" -type f 2>/dev/null | head -3)
if [ -n "$NEWER" ]; then
  echo "REFUSING: $BIN is OLDER than sources that changed since it was linked."
  echo "  the build almost certainly failed; these are newer:"
  printf '    %s\n' $NEWER
  echo "  rebuild and check BUILD_RC before running."
  exit 1
fi
BIN_STAMP="$(shasum -a 256 "$BIN" 2>/dev/null | cut -c1-16) size=$(stat -f%z "$BIN") mtime=$(stat -f%Sm -t %Y-%m-%dT%H:%M:%S "$BIN")"
echo "[BINARY] $BIN sha256=$BIN_STAMP" | tee -a "$OUT"/run.log

AVAIL_G=$(df -g . | awk 'NR==2{print $4}')
SWAP_FREE=$(sysctl -n vm.swapusage | sed -E 's/.*free = ([0-9]+).*/\1/')
echo "disk ${AVAIL_G} GiB free, swap ${SWAP_FREE} MB free"
# ⚠️ THE PRE-FLIGHT NUMBER HAS TO COVER WHAT THE RUN ITSELF CONSUMES, NOT WHAT IT
# NEEDS TO START. Measured twice today, both times painfully: a 300-second replay
# grows the macOS swap file by SEVERAL GIGABYTES, so a run begun at 6 GiB free
# ended with 221 MiB and "no space left on device" mid-write — and the first time
# this happened the machine had to be force-rebooted. 4 GiB was a check on the
# wrong quantity.
[ "${AVAIL_G:-99}" -lt 8 ] && {
    echo "REFUSING: ${AVAIL_G} GiB free, need 8. A run of this length grows swap by" >&2
    echo "  several GiB; starting at less has filled the disk and hung the Mac." >&2
    echo "  Free space, or wait for macOS to shrink the swap file (it does, when idle)." >&2
    exit 3; }

# Alimentation : témoin seulement. Le refus sur batterie (b6d1c0f, P650) est retiré
# à la demande du testeur le 2026-09-25 : ce qui avait faussé le run 4 de P650 était
# le CAPOT FERMÉ sur batterie (Clamshell Sleep), pas la batterie. Garder le capot
# ouvert pendant un run de mesure hors secteur.
echo "[ALIM] $(pmset -g batt 2>/dev/null | head -1 | sed "s/^Now drawing from //")"

# ⚠️ MATCH THE PROCESS NAME, NOT THE COMMAND LINE. `pgrep -f <anything>` also
# matches a shell that merely MENTIONS the path, so a monitoring loop waiting for
# the game to exit refuses the next run — that happened twice today, once with a
# loose pattern and once with an anchored one. `pgrep -x boot` matches the
# executable's name, which a shell cannot impersonate.
if pgrep -x boot > /dev/null; then
    echo "REFUSING: another boot_cache/boot is already running:" >&2
    pgrep -xl boot >&2; exit 1
fi

KEYPIPE="${BO2_KEYPIPE:-$TMPDIR/bo2.angola.key}"
CLICKPIPE="${BO2_CLICKPIPE:-$TMPDIR/bo2.angola.click}"
rm -f "$KEYPIPE" "$CLICKPIPE"; mkfifo "$KEYPIPE" "$CLICKPIPE" || exit 3

# ⚠️ The redirect below is `>>`, not `>`: `>` silently ate the [BINARY] stamp
# written above, so rule (f) shipped broken and no run.log ever named its binary.
# Line 23 already rm -f's the log, so >> starts empty either way.
# ⚠️ AND: never put a comment between a `\` continuation and its command. Doing
# exactly that here would split `env ...` from `./boot_cache/boot`, launching the
# binary with NONE of the reference configuration — and `bash -n` accepts it,
# because it is valid syntax meaning something else.
# BO2_NO_FORCE_MAP=1 on the runner selects natural menu navigation. Remove any
# inherited injection flag as well; do not merely postpone spmap by a large count.
# 🏆 SAFE=2 par defaut depuis 2026-09-13 : ne pas lancer la mission pendant
# que les logos de demarrage jouent. Sans lui, sub_6cd5e0 se consomme a
# promouvoir la video suivante au lieu d'arreter la courante, donc
# cinematicTextures n'est jamais depile et le demontage LUI meurt en
# "free does not match allocation". Le bras par defaut ne passait que par
# accident de lenteur -- mesure A/B, docs/SPEED.md etape 55.
#
# 🎯 LA CARTE VIENT D'UNE SEULE VARIABLE : BO2_LEVEL (defaut angola), lue dans
# l'environnement OU parmi les arguments. Elle sert a la fois a l'injection et a
# la ligne [ENV]. Avant le 2026-09-24 l'injection etait codee en dur « angola »
# pendant que [ENV] ecrivait ${BO2_LEVEL} : BO2_LEVEL=afghanistan chargeait
# angola et journalisait « afghanistan ». Passer BO2_FORCE_MAP_NOW en argument
# est donc REFUSE : deux sources pour la carte, c'est un journal qui peut mentir.
LEVEL="${BO2_LEVEL:-angola}"
for arg in "$@"; do
    case "$arg" in
        BO2_LEVEL=*) LEVEL="${arg#BO2_LEVEL=}" ;;
        BO2_FORCE_MAP_NOW=*|BO2_FORCE_MAP=*)
            [ "${BO2_NO_FORCE_MAP:-0}" = 1 ] && continue   # refuse plus bas, avec son propre message
            echo "REFUSING: $arg — la carte se choisit par BO2_LEVEL=<carte>, et par elle seule" >&2
            exit 3 ;;
    esac
done
case "$LEVEL" in ''|*[!a-z0-9_]*) echo "REFUSING: BO2_LEVEL='$LEVEL' n'est pas un nom de zone" >&2; exit 3 ;; esac
MAP_ENV=(BO2_FORCE_MAP_NOW="$LEVEL" BO2_FORCE_MAP_SAFE="${BO2_FORCE_MAP_SAFE:-2}")

# 🏆 CS_COOP=1 par defaut depuis 2026-09-13 : une section critique invitee doit
# exclure les AUTRES threads invites. Le mutex hote est recursif et appartient au
# thread hote que toutes les coroutines partagent, donc seul il n'excluait rien :
# [CS-SHARED] mesure 21 entrees par run dans une section qu'un autre thread
# invite detient -- sur les reserves de variables de script et l'allocateur MT,
# les structures memes dont le gel d'angola est fait. Cout mesure : ~1 tour
# d'ordonnanceur par attente (runs/ab_coop : 4096 attentes, 4102 tours).
# Reparation de fidelite, pas garde : elle se justifie sans l'oracle.
# 🏆 Les trois correctifs de fidelite ensemble depuis 2026-09-13, et ils vont
# ensemble : CS_COOP fait ATTENDRE sur le detenteur d'une section, donc sans le
# tourniquet equitable il transforme une violation silencieuse en interblocage
# vivant (mesure, runs/coop1..3 : spawn 0/3, « tid 10 WAITS ... held by tid 3 »
# pendant que tid 11 tourne). RR_FAIR ne coute rien au chargement : spawn a
# 135-141 s, la fourchette du profil de reference (runs/fid_1..2).
CS_ENV=(BO2_W32_FAST="${BO2_W32_FAST:-1}"
        BO2_PREEMPT_W="${BO2_PREEMPT_W:-20000}"
        BO2_CS_COOP="${BO2_CS_COOP:-1}"
        BO2_RR_FAIR="${BO2_RR_FAIR:-1}"
        BO2_REAL_MUTEX="${BO2_REAL_MUTEX:-1}")
if [ "${BO2_NO_FORCE_MAP:-0}" = 1 ]; then
    unset BO2_FORCE_MAP_NOW BO2_FORCE_MAP
    MAP_ENV=()
    for arg in "$@"; do
        case "$arg" in BO2_FORCE_MAP_NOW=*|BO2_FORCE_MAP=*)
            echo "REFUSING: BO2_NO_FORCE_MAP conflicts with $arg" >&2; exit 3;;
        esac
    done
    echo "[MAP-START] natural menu; BO2_FORCE_MAP_NOW and BO2_FORCE_MAP absent" | tee -a "$OUT/run.log"
else
    echo "[MAP-START] forced map configuration" | tee -a "$OUT/run.log"
fi

# The historical menu runner did not enable the campaign's experimental guards.
# Keep reference as the default and make this diagnostic configuration explicit.
#
# 🔴 BO2_TEARDOWN_GUARD RETIRE DES DEFAUTS LE 2026-09-21 (P614, n=3) — ELLE
# FABRIQUAIT LE « MUR DES 28 s ». Elle depile jusqu'a 4096 entrees de la liste
# 0x2f3b818 toutes les ~5 s sur la seule condition « tete et compte inchanges
# 3 tics hote », sans aucun test que l'invite soit dans la boucle 0x477460 --
# or cette condition est VRAIE EN PLEIN JEU, ou rien ne se demonte. Et
# 0x2f3b818 est le `pauseArrayId` : chaque fil en `waittill` y a une entree, et
# la passe 1 (Scr_ShutdownSystem) la parcourt pour annuler les listes de
# notification. On vidait d'avance la liste qu'elle devait traiter.
#
# Mesure, 3 runs contre 3 : passe 1 a 165/33/165 appels contre 1/1/1 ; passe 2
# (le fil-piege) 0/0/0 contre 16/9/20 ; soudure de la liste libre 0/0/0 contre
# 8/8/8 ; monde simule 91,5/98,6/90,5 s contre 27,6/27,7/29,6 s, avec 4/1/5
# RECHARGEMENTS DE POINT DE CONTROLE franchis contre 0/0/0. Cout en debit :
# AUCUN -- 6,9/7,5/6,8 img/s en jeu contre 8,1/7,5/6,9 (scratchpad/ingame_fps.py).
# Details : scratchpad/P78.md P614, docs/HANDOFF.md 2026-09-21 03:31.
#
# ⚠️ Le code de la garde reste en place et reste armable a la main ; ce qui
# change est qu'on ne l'arme plus par defaut. Regle 7 d'AGENTS.md : zero garde.
CAMPAIGN_ENV=(BO2_SEARCH_GUARD=1 BO2_MT_NULLGUARD=2 BO2_MT_GUARD=2
    BO2_SWEEP=3 BO2_PVAR_GUARD=1 BO2_CORPSE_UNLINK=2
    BO2_SL_DOUBLEFREE=2 BO2_SND_SHIELD=1 BO2_RESOLVE_KEY=1a4e
    BO2_FIELD_SEED=1a4e BO2_LJ_GUARD=1
    # [SLCHAIN] ne coute RIEN en regime normal : win32_slchain_scan n'est appele
    # que par le fil de veille HOTE au gel (kernel32_impl.cpp). Or c'est le
    # predicat meme du gel -- il nomme les enregistrements ranges dans le mauvais
    # seau. L'avoir desarme par defaut nous a fait perdre deux runs de diagnostic.
    BO2_SLCHAIN_SCAN=1)
# `clean` : AUCUNE des gardes de campagne (ni BO2_SLCHAIN_SCAN). Identique a
# `menu` par son contenu ; le nom dit l'intention — mesurer le port sans nos
# rustines, et en particulier sans BO2_MT_NULLGUARD, dont la seule DEFINITION
# (quelle que soit sa valeur) coupe la voie rapide des ecritures 8/16 bits
# (mt_hooks_on(), win32_runtime.cpp).
case "${BO2_RUN_PROFILE:-reference}" in
    reference) ;;
    menu|clean) CAMPAIGN_ENV=() ;;
    *) echo "REFUSING: unknown BO2_RUN_PROFILE" >&2; exit 3 ;;
esac
echo "[RUN-PROFILE] ${BO2_RUN_PROFILE:-reference}" | tee -a "$OUT/run.log"
# Défauts de rendu de scripts/play.sh (2026-10-24) : sans eux, cratères en carrés dans les runs de mesure (P899).

env DYLD_LIBRARY_PATH="$PWD/boot_cache/hlslcc_build" \
    BO2_STALL_ABORT=0 BO2_SV_INLINE=1 BO2_WINDOW=1 BO2_XAUDIO=1 \
    BO2_SND_UNMUTE=1 BO2_MUTE=0 BO2_VPX_NATIVE=1 BO2_VPX_REPLACE=1 \
    BO2_FORCE_FOCUS=1 BO2_KEYPIPE="$KEYPIPE" BO2_CLICKPIPE="$CLICKPIPE" BO2_CURSOR_GRAB=0 \
    BO2_MIP_CIBLES="${BO2_MIP_CIBLES:-0}" BO2_DEPTH_BIAS="${BO2_DEPTH_BIAS:-1}" \
    ${MAP_ENV[@]+"${MAP_ENV[@]}"} \
    ${CS_ENV[@]+"${CS_ENV[@]}"} \
    ${CAMPAIGN_ENV[@]+"${CAMPAIGN_ENV[@]}"} \
    BO2_ERR_CATALOGUE=1 BO2_MSGB=150 \
    BO2_PAD=0 BO2_MAX_IMPORTS=0 \
    "$@" \
    ./boot_cache/boot boot_cache/pe_sections.bin >> "$OUT/run.log" 2>&1 &
PID=$!

# 🔴 2026-09-20 — L'ECONOMISEUR D'ECRAN EST UN CONFONDANT DE DEBIT, et il est
# produit par NOTRE PROPRE DISCIPLINE. La regle « reste hors du CPU pendant une
# mesure » rend le Mac inactif ; l'economiseur demarre alors en plein run et
# `WindowServer` monte a ~46 % (mesure le 2026-09-20 a 12:53, legacyScreenSaver
# actif depuis 1 h). Sur une machine ou `boot` demande ~140 %, c'est un demi-
# coeur pris a l'invite — du meme ordre que l'ecart de 2,13x deja documente.
# `caffeinate -w` meurt tout seul quand le run finit. Il empeche l'economiseur
# de DEMARRER pendant le run — ce qui est le vrai risque, puisque c'est une
# transition en plein milieu d'une mesure.
#
# ⚠️ Un economiseur DEJA affiche ne se congedie PAS comme ca : mesure le
# 2026-09-20, `killall -9 legacyScreenSaver` le tue et macOS le relance dans la
# seconde (PID neuf, ELAPSED 01:36), parce que le systeme reste en etat « ecran
# de veille » tant qu'aucune entree utilisateur n'arrive. Cout mesure au repos :
# WindowServer 49,7 % + economiseur 7,6 %, soit ~0,5 coeur CONSTANT. Une
# constante ne fausse pas une comparaison entre bras, et elle ne peut pas
# fabriquer une falaise en plein run — ne pas lui attribuer une transition.
killall -9 legacyScreenSaver ScreenSaverEngine 2>/dev/null || true
caffeinate -dimsu -w "$PID" >/dev/null 2>&1 &
CAFF=$!
SS=$(pgrep -x legacyScreenSaver | wc -l | tr -d " ")
echo "[CAFFEINE] caffeinate -dimsu -w $PID (pid=$CAFF) ; economiseur encore present=$SS" | tee -a "$OUT/run.log"

# 🔴 CAPOT FERMÉ = RUN MORT, MÊME SUR SECTEUR (P661, 2026-09-26). caffeinate n'empêche pas
# la veille « Clamshell » : le Mac passe en DarkWake, écran éteint, et la vidéo d'intro
# ne dépasse plus le 3e clip ; spmap n'est jamais injecté (9 runs sur 9, 00:15-03:49).
# Témoin au début ET à la fin : on peut fermer le capot en plein run.
capot() { ioreg -r -k AppleClamshellState -d 4 2>/dev/null | grep -q '"AppleClamshellState" = Yes' && echo FERME || echo ouvert; }
CAPOT_T0=$(date '+%Y-%m-%d %H:%M:%S')
echo "[CAPOT] debut=$(capot)" | tee -a "$OUT/run.log"

# BO2_MAX_IMPORTS=0 : le garde-fou [RUNAWAY] (3 M d'imports sans image) coupait le lanceur nu AVANT la premiere image :
# le demarrage en prend 3,1 a 3,2 M (Z-NATIF-23d, 2 runs morts en 17-25 s). Toutes les series le passaient deja ;
# le budget en secondes borne le run. Un appelant peut le rearmer ("$@" vient apres).
# BO2_PAD=0 : une manette branchee (DualSense, Xbox) ne doit pas changer une mesure ; le jeu voit
# « aucune manette », comme avant runtime/input/pad_gc.mm. Un run pilote passe BO2_PAD_SCRIPT=<fichier>
# (le script l'emporte sur BO2_PAD=0 : il ne lit jamais la manette reelle).
# 🎯 A run's log must say what produced it. Without this, "which runs had the
# fast path on?" is unanswerable after the fact — it blocked a census on
# 2026-09-12 and turned a one-run result into a headline claim that did not hold.
# ⚠️ UNE seule ecriture : boot ecrit deja ce journal. Ecrite morceau par morceau, la ligne a ete coupee par un
# [IMPORT] d'un fil heberge (P797, run 0_001730) et ligne.py, sans map=, a refuse le run (« niveau jamais charge »).
env_ligne='[ENV]'; for v in "$@"; do env_ligne+=" $v"; done
# ⚠️ Le defaut doit etre le MEME que celui du `case` 20 lignes plus haut
# (`:-reference`). Il disait `:-default`, donc tout run sans BO2_RUN_PROFILE
# s'est journalise « profile=default » — un profil qui n'existe pas et que le
# `case` refuserait. Le `[RUN-PROFILE]` juste au-dessus disait « reference »
# dans le meme journal : deux lignes contradictoires sur la meme variable.
printf '%s | profile=%s map=%s budget=%ss\n' "$env_ligne" "${BO2_RUN_PROFILE:-reference}" "$LEVEL" "$SECS" >> "$OUT/run.log"
echo "pid=$PID  log=$OUT/run.log  budget=${SECS}s"

# Poll rather than sleep, so a run that ends on its own is not waited out.
# ⚠️ AND WATCH THE DISK WHILE IT RUNS. A pre-flight check cannot see swap growth
# that happens later; only a live check can. Kill the run rather than let the
# filesystem fill — a truncated log is recoverable, a wedged machine is not.
i=0
GEL_SAMPLE=0
while [ $i -lt "$SECS" ]; do
    sleep 5; i=$((i+5))
    kill -0 "$PID" 2>/dev/null || { echo "ended on its own after ${i}s"; break; }
    # 🎯 UN GEL QUI DURE SE DIAGNOSTIQUE PENDANT QU'IL DURE. [FROZEN #16] = 16 relevés du HUD sans
    # Present invité neuf (~30 s) : jamais vu dans un run vivant (un seul [FROZEN #1] transitoire au
    # chargement). La pile HÔTE des fils hébergés n'est visible qu'à `sample` : sans elle, le gel P677
    # (Main sur la section critique #27 tenue par srv) est resté sans cause. Une fois par run, 5 s.
    if [ $GEL_SAMPLE -eq 0 ] && tail -c 2000000 "$OUT/run.log" 2>/dev/null | grep -aq '^\[FROZEN #16\]'; then
        GEL_SAMPLE=1
        echo "[GEL-SAMPLE] a ${i}s : sample $PID 5 -> $OUT/gel.sample.txt" >> "$OUT/run.log"
        sample "$PID" 5 -file "$OUT/gel.sample.txt" >/dev/null 2>&1 &
    fi
    FREE_M=$(df -m . | awk 'NR==2{print $4}')
    if [ "${FREE_M:-99999}" -lt 1500 ]; then
        echo "⛔ ABORTING at ${i}s: only ${FREE_M} MiB free — the disk is filling." >&2
        echo "   The log so far is kept; this is the swap growth, not the log." >&2
        kill "$PID" 2>/dev/null
        break
    fi
done
kill "$PID" 2>/dev/null
i=0; while kill -0 "$PID" 2>/dev/null && [ $i -lt 20 ]; do sleep 1; i=$((i+1)); done
rm -f "$KEYPIPE" "$CLICKPIPE"
N_CLAM=$(pmset -g log 2>/dev/null | awk -v t0="$CAPOT_T0" '($1" "$2) >= t0' | grep -c "Clamshell Sleep")
echo "[CAPOT] fin=$(capot) veilles_clamshell_pendant_le_run=$N_CLAM" | tee -a "$OUT/run.log"

# ⚠️ PRESERVE THE LOG OF A SHORT RUN, ALWAYS. angola ends by itself at ~10,000
# lines roughly one run in three — no [RUNAWAY], no crash banner, no frames — and
# that log was deleted during cleanup TWICE before anyone read it. A run that ends
# far short of its budget is the single most valuable log this script produces, so
# it is copied somewhere cleanup does not reach.
LINES=$(wc -l < "$OUT/run.log" | tr -d ' ')
if [ "${LINES:-0}" -lt 40000 ]; then
    KEEP="runs/SHORT-RUNS/$(date +%Y%m%d-%H%M%S)-${LINES}lines"
    mkdir -p "$KEEP" && cp "$OUT/run.log" "$KEEP/run.log"
    echo "⚠️  SHORT RUN ($LINES lines, expected ~110k) — log preserved in $KEEP"
    echo "    Inspect [CRASH] signal=, [RUNAWAY], and the final log records; short does not imply crash-free."
fi

echo "--- results ---"
python3 - "$OUT/run.log" "$LEVEL" <<'PY'
import sys, re, collections
lines = open(sys.argv[1], errors='replace').read().splitlines()
level = sys.argv[2]
def n(k): return sum(1 for l in lines if k in l)
print(f"lines                {len(lines)}")
# Le temoin de niveau est [DB-LOADXFILE] name="<carte>", et rien d'autre
# ([SETORIGIN] #2 sort aussi au menu). Un run sans lui est un run MORT.
lvl = sum(1 for l in lines if 'DB-LOADXFILE' in l and f'name="{level}"' in l)
print(f"niveau {level:13s} {'CHARGE' if lvl else 'NON CHARGE -- run mort'} ({lvl} lignes DB-LOADXFILE)")
for k in ("[SETORIGIN]", "[RUNAWAY]", "[ERR-CAT", "[ENGINE-MSG", "[D3D-PRESENT] frame"):
    print(f"{k:20s} {n(k)}")
pres = [l for l in lines if "[D3D-PRESENT] frame" in l]
print(f"last present         {pres[-1].strip() if pres else '(none)'}")
# The error catalogue IN ORDER — this is the stage-2 comparison target.
cat = [re.sub(r'^\[ERR-CAT[^\]]*\]\s*', '', l).strip() for l in lines if '[ERR-CAT' in l]
print(f"error catalogue      {len(cat)} entries")
for c in cat[:40]: print(f"   {c[:150]}")
PY
