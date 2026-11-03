#!/usr/bin/env python3
"""Does the picture CHANGE? Per-shot brightness plus consecutive-pair diffs.

⚠️ WHY THIS EXISTS, and it is the mistake this project keeps paying for.
`screencapture` of the game window includes the TITLE BAR, and the title bar
carries live fps/draw counters. So two captures of a completely frozen renderer
are never byte-identical — the counters tick — and "the file changed" reads as
"the game is rendering" when it means nothing of the kind.

The reverse trap is just as bad. Measured on runs/afg1, the 2026-08-12 run whose
screenshots the tester saw:

    t6min.png and t7min.png are BYTE-IDENTICAL
    t7min vs t8min: title band 9.4% of pixels differ, image body 0 of 924,160

A minute apart, not one body pixel moved. That is NOT proof of a dead renderer:
afghanistan has no level script (60d60cc), so there is no AI and no animation,
and if nobody touches the mouse a perfectly healthy renderer presents identical
frames forever. Identical frames and a frozen presenter look the same from here.

So this tool reports the two bands SEPARATELY and refuses to call it:
  * body pixels differing  -> did the SCENE change
  * title pixels differing -> is the guest still counting
and the only way to turn "static" into evidence is to move the view while
shooting (scripts/shoot_the_level.sh does that through BO2_CLICKPIPE).

    tools/shotdiff.py 'runs/afg2/level_*.png'
    tools/shotdiff.py 'runs/afg2/level_*.png' --title-rows 60
"""
import sys, os, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pxstat import read_png

# The title bar is ~26 points; on a Retina backing store that is ~52-60 pixels.
# Default generously — a few rows of sky counted as "title" costs nothing, while
# one row of counters counted as "body" would fake scene motion every time.
DEFAULT_TITLE_ROWS = 60
STEP = 2          # sample every 2nd pixel both ways; 4x cheaper, same verdict
THRESH = 6        # per-pixel sum-of-channels delta that counts as "changed"


def stats(w, h, ch, px, y0):
    tot = n = 0
    for y in range(y0, h, 3):
        for x in range(0, w, 3):
            o = (y * w + x) * ch
            tot += (px[o] + px[o + 1] + px[o + 2]) // 3
            n += 1
    return tot / max(n, 1)


def band_diff(a, b, w, h, ch, y0, y1):
    diff = n = 0
    maxd = 0
    for y in range(y0, y1, STEP):
        for x in range(0, w, STEP):
            o = (y * w + x) * ch
            d = (abs(a[o] - b[o]) + abs(a[o + 1] - b[o + 1])
                 + abs(a[o + 2] - b[o + 2]))
            n += 1
            if d > THRESH:
                diff += 1
            if d > maxd:
                maxd = d
    return diff, n, maxd


def main():
    args = [a for a in sys.argv[1:]]
    title_rows = DEFAULT_TITLE_ROWS
    if "--title-rows" in args:
        i = args.index("--title-rows")
        title_rows = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return 2
    paths = sorted(glob.glob(args[0]))
    if not paths:
        print(f"no files match {args[0]!r}")
        return 1

    prev = None
    print(f"{'shot':28s} {'mean':>7s}   body-changed      title-changed   verdict")
    for p in paths:
        w, h, ch, px = read_png(p)
        m = stats(w, h, ch, px, title_rows)
        line = f"{os.path.basename(p):28s} {m:7.1f}"
        if prev is None or prev[0] != (w, h, ch):
            line += "        —                 —          (first / size change)"
        else:
            bd, bn, bmax = band_diff(prev[1], px, w, h, ch, title_rows, h)
            td, tn, _ = band_diff(prev[1], px, w, h, ch, 0, title_rows)
            bpct = 100.0 * bd / max(bn, 1)
            tpct = 100.0 * td / max(tn, 1)
            if bpct > 0.5:
                verdict = "SCENE MOVED"
            elif tpct > 0.5:
                verdict = "scene static, guest alive"
            else:
                verdict = "⚠️  NOTHING changed at all"
            line += (f"   {bpct:6.2f}% (max {bmax:3d})   {tpct:6.2f}%      {verdict}")
        print(line)
        prev = ((w, h, ch), px)

    print("\n⚠️  'scene static' is NOT 'renderer dead'. With no level script there is")
    print("   nothing to animate; only a moved VIEW distinguishes them. Shoot with")
    print("   BO2_CLICKPIPE armed so the camera is driven between frames.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
