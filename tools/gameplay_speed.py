#!/usr/bin/env python3
"""gameplay_speed.py — real in-game speed with the RIGHT denominator.

    tools/gameplay_speed.py runs/v_ang_1 runs/v_afg_1 ...

Uses the first frame with >=500 draw records as a level-rendering proxy.
Only complete CADENCE windows after that marker count; the crossing window
is excluded. This proxy does not establish player control or mission progress.
Missing markers and allocation errors are reported, not silently pooled.
"""
import gzip, os, re, sys, statistics

CAD = re.compile(r"guestFrames=(\d+)\(\+(\d+)\).*?elapsed_ms=([0-9.]+)")

def open_log(d):
    for f in (f"{d}/run.log", f"{d}/run.log.gz"):
        if os.path.exists(f):
            return gzip.open(f, "rt", errors="replace") if f.endswith(".gz") else open(f, errors="replace")
    return None

def pct(v, p):
    if not v: return float("nan")
    s = sorted(v); k = (len(s) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)

def analyse(d):
    fh = open_log(d)
    if not fh: return None
    sha = "?"; set2 = spawn1 = draws1 = None; freemm = 0; line_no = 0
    win_all = []           # (line_no, frames, secs)
    for line in fh:
        line_no += 1
        if line.startswith("[BINARY]"):
            m = re.search(r"sha256=(\w+)", line); sha = m.group(1)[:16] if m else "?"
        elif set2 is None and line.startswith("[SETORIGIN] #2 "): set2 = line_no
        elif spawn1 is None and line.startswith("[SPAWN] info_player_start #1"): spawn1 = line_no
        elif "free does not match allocation" in line: freemm += 1
        elif draws1 is None and "this frame" in line:
            mm = re.search(r"this frame (\d+) records", line)
            if mm and int(mm.group(1)) >= 500: draws1 = line_no   # first LEVEL frame
        elif "elapsed_ms=" in line:
            m = CAD.search(line)
            if m:
                secs = float(m.group(3)) / 1000
                if secs > 0: win_all.append((line_no, int(m.group(2)), secs))
    fh.close()
    # ⚠️ THE MARKER IS THE PICTURE, NOT THE SCRIPT. [SETORIGIN] #2 is a GSC builtin
    # and afghanistan has no level script; [SPAWN] info_player_start #1 fires
    # ~90 s of 26 fps intro cinematic BEFORE the player is placed (measured:
    # ~2,300 frames — ten times the effect being looked for). The first frame
    # with >=500 records is level geometry (cinematic ~3, level ~1,900) and on
    # angola it lands 32 lines before SETORIGIN #2. Same marker on both levels.
    marker = ("DRAWS>=500", draws1) if draws1 else (None, None)
    post = [(f, s) for (ln, f, s) in win_all if marker[1] and ln > marker[1]][1:]
    fps = [f / s for f, s in post]
    return dict(run=os.path.basename(d.rstrip("/")), sha=sha, marker=marker[0], set2=set2, spawn1=spawn1, draws1=draws1,
                freemm=freemm, nwin=len(post), frames=sum(f for f, _ in post),
                secs=sum(s for _, s in post), fps=fps)

def main():
    rows = [r for r in (analyse(d) for d in sys.argv[1:]) if r]
    print(f"{'run':10} {'binaire':>16} {'marqueur':>11} {'free-mm':>7} {'fen.':>5} {'images':>7} {'s jeu':>7} "
          f"{'p10':>6} {'p50':>6} {'p90':>6}  {'moy':>6}")
    for r in rows:
        ok = r["marker"] and r["freemm"] == 0
        tail = (f"{pct(r['fps'],.1):6.2f} {pct(r['fps'],.5):6.2f} {pct(r['fps'],.9):6.2f}  "
                f"{(r['frames']/r['secs'] if r['secs'] else 0):6.2f}") if ok and r["fps"] else "   (mesure indisponible — pas de chiffre)"
        print(f"{r['run']:10} {r['sha']:>16} {str(r['marker']):>11} {r['freemm']:7} {r['nwin']:5} {r['frames']:7} "
              f"{r['secs']:7.1f} {tail}")
        if r["draws1"]:
            print(f"{'':10} (DRAWS>=500 ligne {r['draws1']}; SETORIGIN#2 ligne {r['set2']}; SPAWN#1 ligne {r['spawn1']})")
    # pooled per level
    for lvl in ("ang", "afg"):
        sel = [r for r in rows if lvl in r["run"] and r["marker"] and r["freemm"] == 0]
        allf = [x for r in sel for x in r["fps"]]
        if not allf: continue
        print(f"\n{lvl}: {len(sel)} runs mesurables | fenêtres de jeu poolées={len(allf)} | "
              f"p10={pct(allf,.1):.2f} p50={pct(allf,.5):.2f} p90={pct(allf,.9):.2f} fps | "
              f"secondes de jeu médiane={statistics.median(r['secs'] for r in sel):.1f}")

if __name__ == "__main__":
    main()
